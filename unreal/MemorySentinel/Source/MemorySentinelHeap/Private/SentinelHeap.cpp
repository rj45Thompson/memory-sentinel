// The heap sampler (UC-3, R5-R9). Classes, each named for the step it serves:
//   FSentinelHeap   the allocator proxy: picks samples, writes canaries, quarantines frees, answers sizes (UC-3.1-3.3)
//   FBlockTable     sampled blocks by address, read without a lock on every free (UC-3.2)
//   FQuarantine     freed sampled blocks held back from reuse, filled with a pattern (UC-3.3)
//   FGuardPool      GWP-ASan style slots: a block right against a no-access page (UC-3.6)
//   FRotor          the thread that loads DR0-DR3 on every thread with the likeliest words (UC-3.5)
//   FaultHandler    the vectored exception handler: watched writes and guard faults (UC-3.5, 3.6)
//   FEventRing      events recorded from any thread, even inside an exception handler, drained by the game thread (UC-3.7)
//
// Nothing on the allocation path allocates through the engine: tables come from VirtualAlloc, stacks from
// RtlCaptureStackBackTrace, locks are spin locks. The proxy is never removed once installed, because blocks it sampled
// must come back through it.
#include "SentinelHeap.h"

#include "HAL/MemoryBase.h"
#include "HAL/PlatformTLS.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_WINDOWS && PLATFORM_64BITS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#include <TlHelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
#define MEMORYSENTINEL_HEAP 1
#else
#define MEMORYSENTINEL_HEAP 0
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMemorySentinelHeap, Log, All);

namespace MemorySentinel
{
#if MEMORYSENTINEL_HEAP
namespace
{
	constexpr uint32 StackDepth = 16;
	static_assert(sizeof(FHeapStack::Frames) == StackDepth * sizeof(uint64), "FHeapStack holds StackDepth frames");
	constexpr uint64 EmptyKey = 0;
	constexpr uint64 TombKey = 1;
	constexpr uint32 StateLive = 1, StateQuarantined = 2, StateGuardLive = 3, StateGuardFreed = 4;
	constexpr uint32 FlagReported = 1;
	constexpr uint64 PageBytes = 4096;
	constexpr uint64 MaxSampleSize = 1ull << 20;        // larger blocks are never sampled
	constexpr uint64 MaxQuarantineBlock = 64ull << 10;  // larger sampled blocks are checked on free, then freed at once
	constexpr uint64 QuarantineBudget = 32ull << 20;
	constexpr int32 QuarantineSlots = 4096;
	constexpr int32 GuardSlots = 256;
	constexpr int32 EventSlots = 512;
	constexpr int32 RecentSlots = 64;
	constexpr uint64 SuspectRingSlots = 16;
	constexpr int32 MaxSuspects = 16;

	inline uint64 Align8(uint64 V) { return (V + 7) & ~7ull; }
	/** Canary bytes after a block: up to the next 8-byte boundary, then 16 more (so one whole aligned word to watch). */
	inline uint64 TailBytes(uint64 Size) { return Align8(Size) - Size + 16; }
	inline uint8 CanaryByte(uint64 I) { return uint8(0xA7 ^ (I * 0x3D)); }
	inline uint8 FreedByte(uint64 I) { return uint8(0x5F ^ (I * 0x2B)); }

	void* OsAlloc(SIZE_T Bytes, DWORD Protect = PAGE_READWRITE)
	{
		return ::VirtualAlloc(nullptr, Bytes, MEM_RESERVE | MEM_COMMIT, Protect);
	}

	struct FSpin
	{
		volatile int32 Held = 0;
		void Lock() { while (FPlatformAtomics::InterlockedCompareExchange(&Held, 1, 0) != 0) { ::SwitchToThread(); } }
		void Unlock() { FPlatformAtomics::InterlockedExchange(&Held, 0); }
	};
	struct FSpinScope
	{
		FSpin& Spin;
		explicit FSpinScope(FSpin& In) : Spin(In) { Spin.Lock(); }
		~FSpinScope() { Spin.Unlock(); }
	};

	/** The caller's stack, skipping this function and Skip more frames. Never allocates. */
	void CaptureHere(FHeapStack& Out, uint32 Skip)
	{
		void* Frames[StackDepth + 8];
		const uint16 N = ::RtlCaptureStackBackTrace(Skip, StackDepth, Frames, nullptr);   // no fixed skip: inlining moves frames, the reader skips plumbing by module
		Out.Num = 0;
		for (uint16 I = 0; I < N && Out.Num < StackDepth; ++I)
		{
			Out.Frames[Out.Num++] = uint64(Frames[I]);
		}
	}

	/** The stack of the code that was running when an exception was raised, from its saved context. Never allocates. */
	void UnwindFrom(const CONTEXT& In, FHeapStack& Out)
	{
		CONTEXT C = In;
		Out.Num = 0;
		for (int32 Step = 0; Step < 64 && Out.Num < StackDepth && C.Rip; ++Step)
		{
			Out.Frames[Out.Num++] = C.Rip;
			DWORD64 ImageBase = 0;
			PRUNTIME_FUNCTION Fn = ::RtlLookupFunctionEntry(C.Rip, &ImageBase, nullptr);
			if (!Fn)
			{
				// a leaf function: its return address is on top of the stack
				C.Rip = *reinterpret_cast<const DWORD64*>(C.Rsp);
				C.Rsp += 8;
				continue;
			}
			PVOID HandlerData = nullptr;
			DWORD64 Establisher = 0;
			::RtlVirtualUnwind(UNW_FLAG_NHANDLER, ImageBase, C.Rip, Fn, &C, &HandlerData, &Establisher, nullptr);
		}
	}

	struct FBlock
	{
		volatile uint64 Ptr;   // EmptyKey, TombKey, or the block's address
		uint64 Size;
		uint64 Serial;
		uint32 State;
		uint32 Flags;
		int32 GuardSlot;
		FHeapStack Alloc;
		FHeapStack Free;
	};

	/** Sampled blocks by address. Open addressing; Find is lock-free (every free asks it), Insert/Remove run under the
	 *  sampler's lock. A block is only ever removed by its own free, so a lookup never races its own key. */
	class FBlockTable
	{
	public:
		bool Init(uint32 WantSlots)
		{
			Bits = 10;
			while ((1u << Bits) < WantSlots && Bits < 22) { ++Bits; }
			Cap = 1u << Bits;
			Mask = Cap - 1;
			Slots = static_cast<FBlock*>(OsAlloc(sizeof(FBlock) * Cap));
			return Slots != nullptr;
		}
		int32 Find(uint64 Ptr) const
		{
			uint32 I = Hash(Ptr);
			for (uint32 N = 0; N < Cap; ++N, I = (I + 1) & Mask)
			{
				const uint64 K = uint64(FPlatformAtomics::AtomicRead(reinterpret_cast<volatile const int64*>(&Slots[I].Ptr)));
				if (K == Ptr) { return int32(I); }
				if (K == EmptyKey) { return -1; }
			}
			return -1;
		}
		/** A free slot for Ptr (filled by the caller, then Publish), or nullptr when the table is too full. */
		/** bPriority (suspect sizes, guard blocks) may use the last fifth of the table, which ordinary samples leave free. */
		FBlock* Claim(uint64 Ptr, bool bPriority)
		{
			uint32 I = Hash(Ptr);
			for (uint32 N = 0; N < Cap; ++N, I = (I + 1) & Mask)
			{
				const uint64 K = Slots[I].Ptr;
				if (K == TombKey) { return &Slots[I]; }
				if (K == EmptyKey)
				{
					if (Used + 1 > (bPriority ? Cap / 20 * 19 : Cap / 5 * 4)) { return nullptr; }
					++Used;
					return &Slots[I];
				}
			}
			return nullptr;
		}
		void Publish(FBlock& B, uint64 Ptr) { FPlatformAtomics::InterlockedExchange(reinterpret_cast<volatile int64*>(&B.Ptr), int64(Ptr)); }
		void Remove(FBlock& B) { FPlatformAtomics::InterlockedExchange(reinterpret_cast<volatile int64*>(&B.Ptr), int64(TombKey)); }
		FBlock& At(int32 I) { return Slots[I]; }
		uint32 Capacity() const { return Cap; }
	private:
		uint32 Hash(uint64 P) const { return uint32(((P >> 4) * 0x9E3779B97F4A7C15ull) >> (64 - Bits)); }
		FBlock* Slots = nullptr;
		uint32 Cap = 0, Mask = 0, Bits = 0, Used = 0;
	};

	/** Freed sampled blocks, oldest first, held back from reuse so a late write lands in memory nothing else owns. */
	struct FQuarantine
	{
		int32 Index[QuarantineSlots];
		uint64 Ptr[QuarantineSlots];
		int32 Head = 0;
		int32 Count = 0;
		uint64 Bytes = 0;
	};

	/** GWP-ASan style guard slots: [data page][no-access page]. A block sits at the end of its data page, so the first
	 *  byte past it is on the no-access page. Freed slots go to the back of the line, which keeps them unmapped as long as
	 *  possible and turns a late access into a fault. */
	class FGuardPool
	{
	public:
		bool Init()
		{
			Base = static_cast<uint8*>(::VirtualAlloc(nullptr, GuardSlots * 2 * PageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
			for (int32 S = 0; S < GuardSlots; ++S) { Line[S] = S; TableIndex[S] = -1; }
			LineHead = 0;
			LineCount = GuardSlots;
			return Base != nullptr;
		}
		bool Contains(uint64 A) const { return Base && A >= uint64(Base) && A < uint64(Base) + GuardSlots * 2 * PageBytes; }
		int32 SlotOf(uint64 A) const { return int32((A - uint64(Base)) / (2 * PageBytes)); }
		bool OnGuardPage(uint64 A) const { return ((A - uint64(Base)) % (2 * PageBytes)) >= PageBytes; }
		uint8* Data(int32 Slot) const { return Base + uint64(Slot) * 2 * PageBytes; }
		int32 Take()
		{
			if (LineCount == 0) { return -1; }
			const int32 S = Line[LineHead];
			LineHead = (LineHead + 1) % GuardSlots;
			--LineCount;
			return S;
		}
		void Give(int32 Slot)
		{
			Line[(LineHead + LineCount) % GuardSlots] = Slot;
			++LineCount;
		}
		int32 TableIndex[GuardSlots];
	private:
		uint8* Base = nullptr;
		int32 Line[GuardSlots];
		int32 LineHead = 0;
		int32 LineCount = 0;
	};

	struct FEventSlot
	{
		volatile int32 Ready;
		FHeapEvent Event;
	};

	/** Events from any thread. Writers claim a slot with one atomic add, so the fault handler can record without a lock. */
	class FEventRing
	{
	public:
		bool Init() { Slots = static_cast<FEventSlot*>(OsAlloc(sizeof(FEventSlot) * EventSlots)); return Slots != nullptr; }
		FHeapEvent* Claim(int32& OutSlot)
		{
			const int64 N = FPlatformAtomics::InterlockedIncrement(&Written) - 1;
			OutSlot = int32(N % EventSlots);
			if (Slots[OutSlot].Ready)
			{
				FPlatformAtomics::InterlockedIncrement(&Dropped);
				return nullptr;
			}
			FMemory::Memzero(&Slots[OutSlot].Event, sizeof(FHeapEvent));
			return &Slots[OutSlot].Event;
		}
		void Commit(int32 Slot) { FPlatformAtomics::InterlockedExchange(&Slots[Slot].Ready, 1); }
		int32 Drain(TArray<FHeapEvent>& Out)
		{
			int32 Got = 0;
			const int64 Upto = FPlatformAtomics::AtomicRead(&Written);
			while (Read < Upto)
			{
				FEventSlot& S = Slots[Read % EventSlots];
				if (!S.Ready) { break; }
				Out.Add(S.Event);
				FPlatformAtomics::InterlockedExchange(&S.Ready, 0);
				++Read;
				++Got;
			}
			return Got;
		}
		volatile int64 Dropped = 0;
	private:
		FEventSlot* Slots = nullptr;
		volatile int64 Written = 0;
		int64 Read = 0;
	};

	struct FSettings
	{
		uint32 SampleEvery = 256;
		uint32 GuardEvery = 0;
		uint32 TableSlots = 65536;
		uint64 Suspects[MaxSuspects] = {};
		int32 NumSuspects = 0;
		uint64 GuardSizes[MaxSuspects] = {};
		int32 NumGuardSizes = 0;
		int32 RotateMs = 250;
	};

	class FSentinelHeap;
	FSentinelHeap* GSentinel = nullptr;
	FString GNotInstalled = TEXT("not started: run with -MemorySentinel, or set [MemorySentinel] bHeapSampler=True in Engine.ini");
	FString GNoRotor;
	void (*GEmergencyWriter)() = nullptr;
	PVOID GHandler = nullptr;

	/** The data breakpoint rotor: wakes when asked, picks up to four words and loads them into every other thread. */
	class FRotor final : public FRunnable
	{
	public:
		explicit FRotor(FSentinelHeap& InHeap) : Heap(InHeap) {}
		virtual uint32 Run() override;
		virtual void Stop() override { FPlatformAtomics::InterlockedExchange(&bStop, 1); if (Wake) { Wake->Trigger(); } }
		void Program(const uint64* Addr, int32 Num);
		FEvent* Wake = nullptr;
		volatile int32 bStop = 0;
		uint32 SelfId = 0;
	private:
		FSentinelHeap& Heap;
	};

	class FSentinelHeap final : public FMalloc
	{
	public:
		FSentinelHeap(FMalloc* InInner, const FSettings& InSettings) : Inner(InInner), Settings(InSettings) {}

		bool Init()
		{
			return Table.Init(Settings.TableSlots) && Events.Init() && (Settings.GuardEvery == 0 && Settings.NumGuardSizes == 0 ? true : Guards.Init())
				&& (Quarantine = static_cast<FQuarantine*>(OsAlloc(sizeof(FQuarantine)))) != nullptr;
		}
		void SetInner(FMalloc* In) { Inner = In; }

		// ---------------------------------------------------------------- choosing a sample (UC-3.2)
		bool IsSuspect(uint64 Size) const
		{
			const int32 N = FPlatformAtomics::AtomicRead(&NumSuspects);
			for (int32 I = 0; I < N; ++I) { if (Suspects[I] == Size) { return true; } }
			return false;
		}
		bool IsGuardSize(uint64 Size) const
		{
			for (int32 I = 0; I < Settings.NumGuardSizes; ++I) { if (Settings.GuardSizes[I] == Size) { return true; } }
			return false;
		}
		/** 0 no sample, 1 canary, 2 guard page. */
		uint32 Choose(SIZE_T Count, uint32 Alignment, uint64 N) const
		{
			if (Count == 0 || Count > MaxSampleSize) { return 0; }
			if ((Settings.GuardEvery || Settings.NumGuardSizes) && Count <= PageBytes && Alignment <= PageBytes
				&& ((Settings.GuardEvery && N % Settings.GuardEvery == 0) || IsGuardSize(Count)))
			{
				return 2;
			}
			if ((Settings.SampleEvery && N % Settings.SampleEvery == 0) || IsSuspect(Count)) { return 1; }
			return 0;
		}

		void AddSuspect(uint64 Size)
		{
			if (Size == 0 || IsSuspect(Size)) { return; }
			FSpinScope L(SuspectLock);
			if (NumSuspects < MaxSuspects && !IsSuspect(Size))
			{
				Suspects[NumSuspects] = Size;
				FPlatformAtomics::InterlockedIncrement(&NumSuspects);
			}
		}

		// ---------------------------------------------------------------- FMalloc
		virtual void* Malloc(SIZE_T Count, uint32 Alignment) override
		{
			const uint64 N = uint64(FPlatformAtomics::InterlockedIncrement(&Allocations));
			const uint32 Pick = Choose(Count, Alignment, N);
			return Pick ? MallocSampled(Count, Alignment, Pick, N) : Inner->Malloc(Count, Alignment);
		}
		virtual void* MallocZeroed(SIZE_T Count, uint32 Alignment) override
		{
			const uint64 N = uint64(FPlatformAtomics::InterlockedIncrement(&Allocations));
			const uint32 Pick = Choose(Count, Alignment, N);
			if (!Pick) { return Inner->MallocZeroed(Count, Alignment); }
			void* P = MallocSampled(Count, Alignment, Pick, N);
			if (P) { FMemory::Memzero(P, Count); }
			return P;
		}
		virtual void* Realloc(void* Original, SIZE_T Count, uint32 Alignment) override
		{
			if (!Original) { return Malloc(Count, Alignment); }
			const int32 I = Table.Find(uint64(Original));
			if (I < 0)
			{
				if (Count && Count <= MaxSampleSize && IsSuspect(Count))
				{
					SIZE_T Old = 0;
					if (Inner->GetAllocationSize(Original, Old))
					{
						const uint64 N = uint64(FPlatformAtomics::InterlockedIncrement(&Allocations));
						void* P = MallocSampled(Count, Alignment, 1, N);
						if (P)
						{
							FMemory::Memcpy(P, Original, FMath::Min<SIZE_T>(Old, Count));
							Inner->Free(Original);
							return P;
						}
					}
				}
				return Inner->Realloc(Original, Count, Alignment);
			}
			const uint64 OldSize = Table.At(I).Size;
			if (Count == 0)
			{
				FreeSampled(I, Original);
				return nullptr;
			}
			void* P = Malloc(Count, Alignment);
			if (P) { FMemory::Memcpy(P, Original, FMath::Min<uint64>(OldSize, Count)); }
			FreeSampled(I, Original);
			return P;
		}
		virtual void Free(void* Original) override
		{
			if (!Original) { return; }
			const int32 I = Table.Find(uint64(Original));
			if (I < 0) { Inner->Free(Original); return; }
			FreeSampled(I, Original);
		}
		virtual bool GetAllocationSize(void* Original, SIZE_T& SizeOut) override
		{
			const int32 I = Original ? Table.Find(uint64(Original)) : -1;
			if (I >= 0)
			{
				SizeOut = SIZE_T(Table.At(I).Size);
				return true;
			}
			return Inner->GetAllocationSize(Original, SizeOut);
		}
		virtual SIZE_T QuantizeSize(SIZE_T Count, uint32 Alignment) override { return Inner->QuantizeSize(Count, Alignment); }
		virtual void Trim(bool bTrimThreadCaches) override { Inner->Trim(bTrimThreadCaches); }
		virtual void SetupTLSCachesOnCurrentThread() override { Inner->SetupTLSCachesOnCurrentThread(); }
		virtual void MarkTLSCachesAsUsedOnCurrentThread() override { Inner->MarkTLSCachesAsUsedOnCurrentThread(); }
		virtual void MarkTLSCachesAsUnusedOnCurrentThread() override { Inner->MarkTLSCachesAsUnusedOnCurrentThread(); }
		virtual void ClearAndDisableTLSCachesOnCurrentThread() override { Inner->ClearAndDisableTLSCachesOnCurrentThread(); }
		virtual void InitializeStatsMetadata() override { Inner->InitializeStatsMetadata(); }
		virtual void UpdateStats() override { Inner->UpdateStats(); }
		virtual void GetAllocatorStats(FGenericMemoryStats& OutStats) override { Inner->GetAllocatorStats(OutStats); }
		virtual void DumpAllocatorStats(FOutputDevice& Ar) override { Inner->DumpAllocatorStats(Ar); }
		virtual bool IsInternallyThreadSafe() const override { return Inner->IsInternallyThreadSafe(); }
		virtual bool ValidateHeap() override { return Inner->ValidateHeap(); }
		virtual const TCHAR* GetDescriptiveName() override { return TEXT("MemorySentinel"); }
		virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return Inner->Exec(InWorld, Cmd, Ar); }
		virtual void OnMallocInitialized() override { Inner->OnMallocInitialized(); }
		virtual void OnPreFork() override { Inner->OnPreFork(); }
		virtual void OnPostFork() override { Inner->OnPostFork(); }
		virtual uint64 GetImmediatelyFreeableCachedMemorySize() const override { return Inner->GetImmediatelyFreeableCachedMemorySize(); }
		virtual uint64 GetTotalFreeCachedMemorySize() const override { return Inner->GetTotalFreeCachedMemorySize(); }

		// ---------------------------------------------------------------- sampled allocation (UC-3.2, 3.6)
		void* MallocSampled(SIZE_T Count, uint32 Alignment, uint32 Pick, uint64 Serial)
		{
			FHeapStack Stack;
			CaptureHere(Stack, 0);
			if (Pick == 2)
			{
				if (void* P = MallocGuarded(Count, Alignment, Serial, Stack)) { return P; }
			}
			const uint64 Tail = TailBytes(Count);
			uint8* P = static_cast<uint8*>(Inner->Malloc(Count + Tail, Alignment));
			if (!P) { return nullptr; }
			for (uint64 I = 0; I < Tail; ++I) { P[Count + I] = CanaryByte(I); }
			FSpinScope L(Lock);
			FBlock* B = Table.Claim(uint64(P), IsSuspect(Count));
			if (!B)
			{
				++SkippedFull;   // untracked: it frees through the inner allocator like any other block
				return P;
			}
			B->Size = Count;
			B->Serial = Serial;
			B->State = StateLive;
			B->Flags = 0;
			B->GuardSlot = -1;
			B->Alloc = Stack;
			B->Free.Num = 0;
			Table.Publish(*B, uint64(P));
			++Sampled;
			++Live;
			Recent[RecentHead % RecentSlots] = uint64(P);
			++RecentHead;
			if (IsSuspect(Count)) { SuspectLive[SuspectLiveHead++ % SuspectRingSlots] = uint64(P); }
			return P;
		}

		void* MallocGuarded(SIZE_T Count, uint32 Alignment, uint64 Serial, const FHeapStack& Stack)
		{
			const uint64 Align = Alignment ? Alignment : (Count >= 16 ? 16 : 8);
			FSpinScope L(Lock);
			const int32 Slot = Guards.Take();
			if (Slot < 0) { return nullptr; }
			if (Guards.TableIndex[Slot] >= 0)
			{
				FBlock& Old = Table.At(Guards.TableIndex[Slot]);   // the slot's last block, freed long ago
				Table.Remove(Old);
				Guards.TableIndex[Slot] = -1;
			}
			uint8* Page = Guards.Data(Slot);
			DWORD OldProtect = 0;
			::VirtualProtect(Page, PageBytes, PAGE_READWRITE, &OldProtect);
			const uint64 Offset = (PageBytes - Count) & ~(Align - 1);
			uint8* P = Page + Offset;
			for (uint64 I = Count; Offset + I < PageBytes; ++I) { P[I] = CanaryByte(I - Count); }
			FBlock* B = Table.Claim(uint64(P), true);
			if (!B)
			{
				::VirtualProtect(Page, PageBytes, PAGE_NOACCESS, &OldProtect);
				Guards.Give(Slot);
				++SkippedFull;
				return nullptr;
			}
			B->Size = Count;
			B->Serial = Serial;
			B->State = StateGuardLive;
			B->Flags = 0;
			B->GuardSlot = Slot;
			B->Alloc = Stack;
			B->Free.Num = 0;
			Table.Publish(*B, uint64(P));
			Guards.TableIndex[Slot] = int32(B - &Table.At(0));
			++GuardSampled;
			++Live;
			return P;
		}

		// ---------------------------------------------------------------- free, quarantine (UC-3.3)
		void FreeSampled(int32 I, void* Original)
		{
			FHeapStack Stack;
			CaptureHere(Stack, 0);
			FSpinScope L(Lock);
			FBlock& B = Table.At(I);
			if (B.Ptr != uint64(Original))
			{
				Inner->Free(Original);
				return;
			}
			if (B.State == StateQuarantined || B.State == StateGuardFreed)
			{
				int32 Slot;
				if (FHeapEvent* E = BeginEvent(EHeapEvent::DoubleFree, B, Slot))
				{
					E->Address = B.Ptr;
					E->FreeStack = B.Free;
					E->WriteStack = Stack;   // the second free
					Events.Commit(Slot);
				}
				return;   // never free it twice
			}
			B.Free = Stack;
			if (B.State == StateGuardLive)
			{
				CheckCanary(B, uint64(Guards.Data(B.GuardSlot)) + PageBytes - B.Ptr - B.Size);
				DWORD OldProtect = 0;
				::VirtualProtect(Guards.Data(B.GuardSlot), PageBytes, PAGE_NOACCESS, &OldProtect);
				B.State = StateGuardFreed;
				Guards.Give(B.GuardSlot);
				--Live;
				return;
			}
			CheckCanary(B, TailBytes(B.Size));
			--Live;
			if (B.Size > MaxQuarantineBlock && !IsWatchedBlock(B.Ptr))
			{
				Table.Remove(B);
				Inner->Free(Original);
				return;
			}
			uint8* P = static_cast<uint8*>(Original);
			for (uint64 K = 0; K < B.Size; ++K) { P[K] = FreedByte(K); }
			B.State = StateQuarantined;
			FQuarantine& Q = *Quarantine;
			while (Q.Count >= QuarantineSlots || (Q.Count > 0 && Q.Bytes + B.Size > QuarantineBudget))
			{
				if (!Evict())
				{
					B.State = StateLive;   // no room: this block skips quarantine (its canary was checked above)
					Table.Remove(B);
					Inner->Free(Original);
					return;
				}
			}
			const int32 At = (Q.Head + Q.Count) % QuarantineSlots;
			Q.Index[At] = I;
			Q.Ptr[At] = B.Ptr;
			++Q.Count;
			Q.Bytes += B.Size;
			++Quarantined;
			RecentFreed[RecentFreedHead % RecentSlots] = B.Ptr;
			++RecentFreedHead;
			if (IsSuspect(B.Size)) { SuspectFreed[SuspectFreedHead++ % SuspectRingSlots] = B.Ptr; }
		}

		/** Frees the oldest quarantined block nothing may still be watching. A watched block, or one of the newest
		 *  suspect-size frees, goes to the back of the line instead: handed to the engine's allocator, the allocator's own
		 *  free-list write would trip a data breakpoint that some thread still holds, and be reported as the program's.
		 *  Returns false when every block in quarantine is held that way. */
		bool Evict()
		{
			FQuarantine& Q = *Quarantine;
			for (int32 Tries = Q.Count; Tries > 0; --Tries)
			{
				const int32 I = Q.Index[Q.Head];
				const uint64 Ptr = Q.Ptr[Q.Head];
				Q.Head = (Q.Head + 1) % QuarantineSlots;
				--Q.Count;
				FBlock& B = Table.At(I);
				if (B.Ptr != Ptr || B.State != StateQuarantined) { return true; }   // a stale entry: its slot is free now
				if (IsWatchedBlock(Ptr) || (IsSuspect(B.Size) && IsNewestSuspectFree(Ptr)))
				{
					const int32 At = (Q.Head + Q.Count) % QuarantineSlots;
					Q.Index[At] = I;
					Q.Ptr[At] = Ptr;
					++Q.Count;
					continue;
				}
				Q.Bytes -= B.Size;
				CheckFreed(B);
				--Quarantined;
				Table.Remove(B);
				Inner->Free(reinterpret_cast<void*>(Ptr));
				return true;
			}
			return false;
		}

		bool IsWatchedBlock(uint64 Ptr) const
		{
			for (int32 K = 0; K < 8; ++K) { if (WatchHistory[K].Block == Ptr) { return true; } }
			return false;
		}

		bool IsNewestSuspectFree(uint64 Ptr) const
		{
			for (uint64 K = 0; K < SuspectRingSlots; ++K) { if (SuspectFreed[K] == Ptr) { return true; } }
			return false;
		}

		// ---------------------------------------------------------------- checks (UC-3.4, R8)
		/** The canary after a live or just-freed block. Records one Overrun event per block. */
		bool CheckCanary(FBlock& B, uint64 Tail)
		{
			++CanaryChecks;
			const uint8* P = reinterpret_cast<const uint8*>(B.Ptr);
			int64 First = -1;
			uint32 Changed = 0;
			for (uint64 I = 0; I < Tail; ++I)
			{
				if (P[B.Size + I] != CanaryByte(I))
				{
					if (First < 0) { First = int64(I); }
					++Changed;
				}
			}
			if (First < 0 || (B.Flags & FlagReported)) { return First < 0; }
			B.Flags |= FlagReported;
			int32 Slot;
			if (FHeapEvent* E = BeginEvent(EHeapEvent::Overrun, B, Slot))
			{
				E->Address = B.Ptr + B.Size + uint64(First);
				E->Offset = int64(B.Size) + First;
				E->Changed = Changed;
				E->Compared = uint32(Tail);
				for (uint64 K = uint64(First); K < Tail && E->Shown < 16; ++K, ++E->Shown)
				{
					E->Expected[E->Shown] = CanaryByte(K);
					E->Found[E->Shown] = P[B.Size + K];
				}
				E->FreeStack = B.Free;
				Events.Commit(Slot);
			}
			AddSuspect(B.Size);
			return false;
		}

		/** The pattern a quarantined block was filled with, and its canary. */
		bool CheckFreed(FBlock& B)
		{
			++CanaryChecks;
			const uint8* P = reinterpret_cast<const uint8*>(B.Ptr);
			int64 First = -1;
			uint32 Changed = 0;
			for (uint64 I = 0; I < B.Size; ++I)
			{
				if (P[I] != FreedByte(I))
				{
					if (First < 0) { First = int64(I); }
					++Changed;
				}
			}
			const bool bCanary = CheckCanary(B, TailBytes(B.Size));
			if (First < 0 || (B.Flags & FlagReported)) { return First < 0 && bCanary; }
			B.Flags |= FlagReported;
			int32 Slot;
			if (FHeapEvent* E = BeginEvent(EHeapEvent::WriteAfterFree, B, Slot))
			{
				E->Address = B.Ptr + uint64(First);
				E->Offset = First;
				E->Changed = Changed;
				E->Compared = uint32(B.Size);
				for (uint64 K = uint64(First); K < B.Size && E->Shown < 16; ++K, ++E->Shown)
				{
					E->Expected[E->Shown] = FreedByte(K);
					E->Found[E->Shown] = P[K];
				}
				E->FreeStack = B.Free;
				Events.Commit(Slot);
			}
			AddSuspect(B.Size);
			return false;
		}

		void CheckSome(int32 Budget)
		{
			FSpinScope L(Lock);
			const uint32 Cap = Table.Capacity();
			int32 Done = 0;
			for (uint32 Visit = 0; Visit < Cap && Done < Budget; ++Visit)
			{
				Cursor = (Cursor + 1) % Cap;
				FBlock& B = Table.At(int32(Cursor));
				if (B.Ptr <= TombKey || (B.Flags & FlagReported)) { continue; }
				if (B.State == StateLive) { CheckCanary(B, TailBytes(B.Size)); ++Done; }
				else if (B.State == StateQuarantined) { CheckFreed(B); ++Done; }
			}
		}

		FHeapEvent* BeginEvent(EHeapEvent Kind, const FBlock& B, int32& OutSlot)
		{
			FHeapEvent* E = Events.Claim(OutSlot);
			if (!E) { return nullptr; }
			FPlatformAtomics::InterlockedIncrement(&NumEvents);
			E->Kind = Kind;
			E->Block = B.Ptr;
			E->Size = B.Size;
			E->Serial = B.Serial;
			E->ThreadId = FPlatformTLS::GetCurrentThreadId();
			E->AllocStack = B.Alloc;
			E->Register = -1;
			E->bWrite = true;
			return E;
		}

		// ---------------------------------------------------------------- watch words (UC-3.5, R6)
		/** Up to four 8-byte words: suspect canaries, suspect quarantined blocks, then the newest canaries and frees. */
		int32 PickWatches(FHeapWatch Out[4])
		{
			FSpinScope L(Lock);
			int32 N = 0;
			auto TryAdd = [&](uint64 Ptr, bool bFreedRing, bool bWantSuspect) {
				if (N >= 4 || Ptr == 0) { return; }
				const int32 I = Table.Find(Ptr);
				if (I < 0) { return; }
				const FBlock& B = Table.At(I);
				const bool bSuspect = IsSuspect(B.Size);
				if (bWantSuspect != bSuspect) { return; }
				const bool bQ = B.State == StateQuarantined;
				if ((bFreedRing && !bQ) || (!bFreedRing && B.State != StateLive) || (B.Flags & FlagReported)) { return; }
				const uint64 Word = bQ ? (B.Ptr & ~7ull) : Align8(B.Ptr + B.Size);
				for (int32 K = 0; K < N; ++K) { if (Out[K].Address == Word) { return; } }
				Out[N++] = FHeapWatch{ Word, B.Ptr, B.Size, bQ, bSuspect };
			};
			// suspects keep rings of their own: at 1 in 64, the shared ring turns over in milliseconds
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				const bool bWantSuspect = Pass < 2;
				const bool bFreed = (Pass % 2) == 1;
				const uint64 Head = bWantSuspect ? (bFreed ? SuspectFreedHead : SuspectLiveHead) : (bFreed ? RecentFreedHead : RecentHead);
				const uint64* Ring = bWantSuspect ? (bFreed ? SuspectFreed : SuspectLive) : (bFreed ? RecentFreed : Recent);
				const uint64 Slots = bWantSuspect ? SuspectRingSlots : RecentSlots;
				for (uint64 K = 0; K < Slots && K < Head && N < 4; ++K)
				{
					TryAdd(Ring[(Head - 1 - K) % Slots], bFreed, bWantSuspect);
				}
			}
			for (int32 K = 0; K < N; ++K) { WatchHistory[(WatchHistoryHead + K) % 8] = Out[K]; }
			WatchHistoryHead = (WatchHistoryHead + N) % 8;
			NumWatches = N;
			for (int32 K = 0; K < 4; ++K) { Watches[K] = K < N ? Out[K] : FHeapWatch{}; }
			return N;
		}

		/** The watch a fault handler hit, from this rotation or the one before (a thread may still hold old registers). */
		bool FindWatch(uint64 Address, FHeapWatch& Out) const
		{
			for (int32 K = 0; K < 8; ++K)
			{
				if (WatchHistory[K].Address == Address) { Out = WatchHistory[K]; return true; }
			}
			return false;
		}

		// ---------------------------------------------------------------- the fault handler's side (UC-3.5, 3.6)
		void OnWatchedWrite(const CONTEXT& Ctx, int32 Register, uint64 Address)
		{
			FHeapWatch W;
			if (!FindWatch(Address, W)) { return; }
			FPlatformAtomics::InterlockedIncrement(&WatchedWrites);
			FBlock Snapshot = {};
			const int32 I = Table.Find(W.Block);
			if (I >= 0) { Snapshot = Table.At(I); }
			else { Snapshot.Ptr = W.Block; Snapshot.Size = W.Size; }
			int32 Slot;
			if (FHeapEvent* E = BeginEvent(EHeapEvent::WatchedWrite, Snapshot, Slot))
			{
				E->Address = Address;
				E->Offset = int64(Address) - int64(W.Block);
				E->Register = Register;
				E->FreeStack = Snapshot.Free;
				UnwindFrom(Ctx, E->WriteStack);
				Events.Commit(Slot);
			}
		}

		bool OnAccessViolation(const CONTEXT& Ctx, uint64 Address, bool bWrite)
		{
			if (!Guards.Contains(Address)) { return false; }
			const int32 GSlot = Guards.SlotOf(Address);
			const int32 I = Guards.TableIndex[GSlot];
			FBlock Snapshot = {};
			if (I >= 0) { Snapshot = Table.At(I); }
			const bool bGuardPage = Guards.OnGuardPage(Address);
			int32 Slot;
			if (FHeapEvent* E = BeginEvent(bGuardPage && Snapshot.State != StateGuardFreed ? EHeapEvent::GuardOverrun : EHeapEvent::GuardAfterFree, Snapshot, Slot))
			{
				E->Address = Address;
				E->Offset = Snapshot.Ptr ? int64(Address) - int64(Snapshot.Ptr) : 0;
				E->bWrite = bWrite;
				E->FreeStack = Snapshot.Free;
				UnwindFrom(Ctx, E->WriteStack);
				Events.Commit(Slot);
			}
			return true;
		}

		// ---------------------------------------------------------------- state
		FMalloc* Inner;
		FSettings Settings;
		FSpin Lock;
		FSpin SuspectLock;
		FBlockTable Table;
		FQuarantine* Quarantine = nullptr;
		FGuardPool Guards;
		FEventRing Events;
		uint64 Suspects[MaxSuspects] = {};
		volatile int32 NumSuspects = 0;
		uint64 Recent[RecentSlots] = {};
		uint64 RecentHead = 0;
		uint64 RecentFreed[RecentSlots] = {};
		uint64 RecentFreedHead = 0;
		uint64 SuspectLive[SuspectRingSlots] = {};
		uint64 SuspectLiveHead = 0;
		uint64 SuspectFreed[SuspectRingSlots] = {};
		uint64 SuspectFreedHead = 0;
		uint32 Cursor = 0;
		FHeapWatch Watches[4];
		int32 NumWatches = 0;
		FHeapWatch WatchHistory[8];
		int32 WatchHistoryHead = 0;
		volatile int64 Allocations = 0;
		volatile int64 NumEvents = 0;
		volatile int64 WatchedWrites = 0;
		uint64 Sampled = 0, GuardSampled = 0, Live = 0, Quarantined = 0, CanaryChecks = 0, SkippedFull = 0;
		volatile int64 Rotations = 0;
		volatile int64 ThreadsProgrammed = 0;
		FRotor* Rotor = nullptr;
		FRunnableThread* RotorThread = nullptr;
	};

	// -------------------------------------------------------------------- the rotor (UC-3.5)
	uint32 FRotor::Run()
	{
		SelfId = ::GetCurrentThreadId();
		while (!bStop)
		{
			Wake->Wait();
			if (bStop) { break; }
			FHeapWatch W[4];
			const int32 N = Heap.PickWatches(W);
			uint64 Addr[4] = {};
			for (int32 K = 0; K < N; ++K) { Addr[K] = W[K].Address; }
			Program(Addr, N);
			FPlatformAtomics::InterlockedIncrement(&Heap.Rotations);
		}
		const uint64 None[4] = {};
		Program(None, 0);
		return 0;
	}

	/** Loads the words into DR0..DR3 of every other thread of this process: write watches, 8 bytes each. A thread is
	 *  suspended only while its registers change, one at a time, and nothing here allocates or takes a lock while one is
	 *  suspended, so a thread stopped inside the allocator cannot deadlock the rotor. */
	void FRotor::Program(const uint64* Addr, int32 Num)
	{
		DWORD64 Dr7 = 0;
		for (int32 K = 0; K < Num; ++K)
		{
			Dr7 |= DWORD64(1) << (K * 2);            // local enable
			Dr7 |= DWORD64(0x1) << (16 + K * 4);     // RW = 01: data writes
			Dr7 |= DWORD64(0x2) << (18 + K * 4);     // LEN = 10: eight bytes
		}
		const DWORD Pid = ::GetCurrentProcessId();
		HANDLE Snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		if (Snap == INVALID_HANDLE_VALUE) { return; }
		THREADENTRY32 Te;
		Te.dwSize = sizeof(Te);
		for (BOOL Ok = ::Thread32First(Snap, &Te); Ok; Ok = ::Thread32Next(Snap, &Te))
		{
			if (Te.th32OwnerProcessID != Pid || Te.th32ThreadID == SelfId) { continue; }
			HANDLE T = ::OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, 0, Te.th32ThreadID);
			if (!T) { continue; }
			if (::SuspendThread(T) != DWORD(-1))
			{
				CONTEXT C;
				FMemory::Memzero(&C, sizeof(C));
				C.ContextFlags = CONTEXT_DEBUG_REGISTERS;
				if (::GetThreadContext(T, &C))
				{
					C.Dr0 = Num > 0 ? Addr[0] : 0;
					C.Dr1 = Num > 1 ? Addr[1] : 0;
					C.Dr2 = Num > 2 ? Addr[2] : 0;
					C.Dr3 = Num > 3 ? Addr[3] : 0;
					C.Dr6 = 0;
					C.Dr7 = Dr7;
					if (::SetThreadContext(T, &C)) { FPlatformAtomics::InterlockedIncrement(&Heap.ThreadsProgrammed); }
				}
				::ResumeThread(T);
			}
			::CloseHandle(T);
		}
		::CloseHandle(Snap);
	}

	// -------------------------------------------------------------------- the fault handler (UC-3.5, 3.6)
	LONG CALLBACK FaultHandler(PEXCEPTION_POINTERS Info)
	{
		FSentinelHeap* Heap = GSentinel;
		if (!Heap) { return EXCEPTION_CONTINUE_SEARCH; }
		const DWORD Code = Info->ExceptionRecord->ExceptionCode;
		CONTEXT& C = *Info->ContextRecord;
		if (Code == EXCEPTION_SINGLE_STEP)
		{
			const DWORD64 Hit = C.Dr6 & 0xF;
			if (!Hit) { return EXCEPTION_CONTINUE_SEARCH; }
			const DWORD64 Regs[4] = { C.Dr0, C.Dr1, C.Dr2, C.Dr3 };
			bool bOurs = false;
			for (int32 K = 0; K < 4; ++K)
			{
				FHeapWatch W;
				if ((Hit & (DWORD64(1) << K)) && Heap->FindWatch(Regs[K], W))
				{
					Heap->OnWatchedWrite(C, K, Regs[K]);
					C.Dr7 &= ~(DWORD64(3) << (K * 2));   // this thread stops watching it: one report per write site
					bOurs = true;
				}
			}
			if (bOurs)
			{
				C.Dr6 = 0;
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
		if (Code == EXCEPTION_ACCESS_VIOLATION && Info->ExceptionRecord->NumberParameters >= 2)
		{
			const uint64 Address = Info->ExceptionRecord->ExceptionInformation[1];
			const bool bWrite = Info->ExceptionRecord->ExceptionInformation[0] == 1;
			if (Heap->OnAccessViolation(C, Address, bWrite) && GEmergencyWriter)
			{
				GEmergencyWriter();   // write the finding now: the crash that follows ends the process
			}
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}

	void ParseSizes(const FString& Text, uint64* Out, int32& Num)
	{
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& P : Parts)
		{
			const uint64 V = FCString::Strtoui64(*P.TrimStartAndEnd(), nullptr, 10);
			if (V && Num < MaxSuspects) { Out[Num++] = V; }
		}
	}

	void Install()
	{
		const TCHAR* Cmd = FCommandLine::Get();
		bool bEnable = FParse::Param(Cmd, TEXT("MemorySentinel"));
		if (!bEnable && GConfig) { GConfig->GetBool(TEXT("MemorySentinel"), TEXT("bHeapSampler"), bEnable, GEngineIni); }
		if (!bEnable) { return; }
		FSettings S;
		if (GConfig)
		{
			int32 V = 0;
			if (GConfig->GetInt(TEXT("MemorySentinel"), TEXT("SampleEvery"), V, GEngineIni)) { S.SampleEvery = uint32(FMath::Max(V, 0)); }
			if (GConfig->GetInt(TEXT("MemorySentinel"), TEXT("GuardEvery"), V, GEngineIni)) { S.GuardEvery = uint32(FMath::Max(V, 0)); }
		}
		FParse::Value(Cmd, TEXT("MemorySentinelSampleEvery="), S.SampleEvery);
		FParse::Value(Cmd, TEXT("MemorySentinelGuardEvery="), S.GuardEvery);
		FParse::Value(Cmd, TEXT("MemorySentinelTable="), S.TableSlots);
		FParse::Value(Cmd, TEXT("MemorySentinelRotateMs="), S.RotateMs);
		FString Sizes;
		if (FParse::Value(Cmd, TEXT("MemorySentinelSuspectSizes="), Sizes)) { ParseSizes(Sizes, S.Suspects, S.NumSuspects); }
		if (FParse::Value(Cmd, TEXT("MemorySentinelGuardSizes="), Sizes)) { ParseSizes(Sizes, S.GuardSizes, S.NumGuardSizes); }

		FSentinelHeap* Heap = new FSentinelHeap(UE::Private::GMalloc, S);
		if (!Heap->Init())
		{
			GNotInstalled = TEXT("could not reserve its tables from the operating system");
			return;
		}
		for (int32 I = 0; I < S.NumSuspects; ++I) { Heap->AddSuspect(S.Suspects[I]); }
		GSentinel = Heap;
		GHandler = ::AddVectoredExceptionHandler(1, &FaultHandler);
		for (;;)
		{
			FMalloc* Current = UE::Private::GMalloc;
			Heap->SetInner(Current);
			if (FPlatformAtomics::InterlockedCompareExchangePointer(reinterpret_cast<void**>(&UE::Private::GMalloc), Heap, Current) == Current)
			{
				break;
			}
		}
		GNotInstalled.Reset();
		if (::IsDebuggerPresent())
		{
			GNoRotor = TEXT("a debugger is attached and owns the debug registers");
		}
		else
		{
			Heap->Rotor = new FRotor(*Heap);
			Heap->Rotor->Wake = FPlatformProcess::GetSynchEventFromPool(false);
			Heap->RotorThread = FRunnableThread::Create(Heap->Rotor, TEXT("MemorySentinelRotor"), 64 * 1024, TPri_AboveNormal);
			if (!Heap->RotorThread) { GNoRotor = TEXT("the rotor thread could not start"); }
		}
		UE_LOG(LogMemorySentinelHeap, Display, TEXT("Memory Sentinel heap sampler on: 1 in %u allocations sampled, %d suspect size(s), guard pages %s, data breakpoint rotor %s"),
			S.SampleEvery, S.NumSuspects, (S.GuardEvery || S.NumGuardSizes) ? TEXT("on") : TEXT("off"),
			GNoRotor.IsEmpty() ? TEXT("on") : *GNoRotor);
	}
}
#endif // MEMORYSENTINEL_HEAP

bool FHeapSampler::IsInstalled()
{
#if MEMORYSENTINEL_HEAP
	return GSentinel != nullptr;
#else
	return false;
#endif
}

FString FHeapSampler::WhyNotInstalled()
{
#if MEMORYSENTINEL_HEAP
	return GNotInstalled;
#else
	return TEXT("the heap sampler runs on Windows x64 only");
#endif
}

FString FHeapSampler::WhyNoRotor()
{
#if MEMORYSENTINEL_HEAP
	return GSentinel ? GNoRotor : GNotInstalled;
#else
	return TEXT("data breakpoints are programmed on Windows x64 only");
#endif
}

FHeapStats FHeapSampler::Stats()
{
	FHeapStats S;
#if MEMORYSENTINEL_HEAP
	if (FSentinelHeap* H = GSentinel)
	{
		FSpinScope L(H->Lock);
		S.bInstalled = true;
		S.bRotor = H->RotorThread != nullptr;
		S.SampleEvery = H->Settings.SampleEvery;
		S.GuardEvery = H->Settings.GuardEvery;
		S.Allocations = uint64(H->Allocations);
		S.Sampled = H->Sampled;
		S.GuardSampled = H->GuardSampled;
		S.Live = H->Live;
		S.Quarantined = H->Quarantined;
		S.CanaryChecks = H->CanaryChecks;
		S.SkippedFull = H->SkippedFull;
		S.Rotations = uint64(H->Rotations);
		S.ThreadsProgrammed = uint64(H->ThreadsProgrammed);
		S.WatchedWrites = uint64(H->WatchedWrites);
		S.Events = uint64(H->NumEvents);
		S.DroppedEvents = uint64(H->Events.Dropped);
		S.NumWatches = H->NumWatches;
		for (int32 K = 0; K < 4; ++K) { S.Watches[K] = H->Watches[K]; }
		S.NumSuspects = H->NumSuspects;
		for (int32 K = 0; K < S.NumSuspects; ++K) { S.Suspects[K] = H->Suspects[K]; }
	}
#endif
	return S;
}

int32 FHeapSampler::Drain(TArray<FHeapEvent>& Out)
{
#if MEMORYSENTINEL_HEAP
	return GSentinel ? GSentinel->Events.Drain(Out) : 0;
#else
	return 0;
#endif
}

void FHeapSampler::CheckSome(int32 Budget)
{
#if MEMORYSENTINEL_HEAP
	if (GSentinel) { GSentinel->CheckSome(Budget); }
#endif
}

void FHeapSampler::RequestRotation()
{
#if MEMORYSENTINEL_HEAP
	if (GSentinel && GSentinel->Rotor && GSentinel->Rotor->Wake) { GSentinel->Rotor->Wake->Trigger(); }
#endif
}

void FHeapSampler::AddSuspectSize(uint64 Size)
{
#if MEMORYSENTINEL_HEAP
	if (GSentinel) { GSentinel->AddSuspect(Size); }
#endif
}

void FHeapSampler::SetEmergencyWriter(void (*Writer)())
{
#if MEMORYSENTINEL_HEAP
	GEmergencyWriter = Writer;
#endif
}
}

class FMemorySentinelHeapModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if MEMORYSENTINEL_HEAP
		MemorySentinel::Install();
#endif
	}
	virtual void ShutdownModule() override
	{
#if MEMORYSENTINEL_HEAP
		// The proxy stays: blocks it sampled must come back through it. Only the rotor stops, and clears the registers.
		if (MemorySentinel::FSentinelHeap* H = MemorySentinel::GSentinel)
		{
			if (H->RotorThread)
			{
				H->Rotor->Stop();
				H->RotorThread->WaitForCompletion();
			}
		}
#endif
	}
};

IMPLEMENT_MODULE(FMemorySentinelHeapModule, MemorySentinelHeap)
