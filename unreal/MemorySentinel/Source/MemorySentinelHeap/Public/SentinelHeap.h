// The heap sampler's public face (UC-3). The sampler is a proxy over the engine's allocator: one allocation in N, and every
// allocation of a suspect size, gets a canary after its end and a recorded allocation stack. Freed sampled blocks are held in
// quarantine, filled with a pattern, instead of being reused. The CPU's four data breakpoints rotate over the likeliest
// blocks, and a sample of blocks can sit against a no-access guard page. What it finds arrives here as FHeapEvent records;
// the MemorySentinel module turns them into findings.
#pragma once

#include "CoreMinimal.h"

namespace MemorySentinel
{
	/** What happened to a sampled block. */
	enum class EHeapEvent : uint8
	{
		Overrun,          // the canary after the block changed: a write past its end
		WriteAfterFree,   // the pattern in a quarantined (freed) block changed
		DoubleFree,       // a block in quarantine was freed again
		WatchedWrite,     // a data breakpoint caught a write to a watched word
		GuardOverrun,     // an access touched the no-access page after a guarded block
		GuardAfterFree,   // an access touched a guarded block after it was freed
	};

	/** A captured call stack: program counters, innermost first. */
	struct FHeapStack
	{
		uint64 Frames[16];
		uint32 Num = 0;
	};

	/** One event, filled in where it happened (the allocator, the per-frame check, or the CPU's exception handler). */
	struct FHeapEvent
	{
		EHeapEvent Kind = EHeapEvent::Overrun;
		uint64 Block = 0;          // the block's address
		uint64 Size = 0;           // the size that was asked for
		uint64 Address = 0;        // the first changed byte, the watched word, or the faulting address
		int64 Offset = 0;          // Address - Block: >= Size means past the end
		uint64 Serial = 0;         // the block's allocation number
		uint32 ThreadId = 0;       // the thread that wrote (watched writes and guard faults) or found it
		uint32 Changed = 0;        // bytes that differ from the pattern
		uint32 Compared = 0;       // bytes compared
		uint32 Shown = 0;          // bytes in Expected/Found
		uint8 Expected[16] = {};
		uint8 Found[16] = {};
		int32 Register = -1;       // DR0..DR3 for a watched write
		bool bWrite = true;        // a guard fault: write (true) or read
		FHeapStack AllocStack;
		FHeapStack FreeStack;
		FHeapStack WriteStack;     // the writing instruction's stack (watched writes, guard faults)
	};

	/** One of the (at most four) words the data breakpoints are watching now. */
	struct FHeapWatch
	{
		uint64 Address = 0;
		uint64 Block = 0;
		uint64 Size = 0;
		bool bQuarantined = false;
		bool bSuspect = false;
	};

	struct FHeapStats
	{
		bool bInstalled = false;
		bool bRotor = false;
		uint32 SampleEvery = 0;
		uint32 GuardEvery = 0;
		uint64 Allocations = 0;    // every allocation that went through the proxy
		uint64 Sampled = 0;        // given a canary
		uint64 GuardSampled = 0;   // put against a guard page
		uint64 Live = 0;           // sampled blocks alive now
		uint64 Quarantined = 0;    // freed sampled blocks held back now
		uint64 CanaryChecks = 0;   // canary and pattern checks done (on free and per frame)
		uint64 SkippedFull = 0;    // samples skipped because the block table was full
		uint64 Rotations = 0;      // times the data breakpoints were reloaded
		uint64 ThreadsProgrammed = 0;
		uint64 WatchedWrites = 0;
		uint64 Events = 0;
		uint64 DroppedEvents = 0;
		int32 NumWatches = 0;
		FHeapWatch Watches[4];
		int32 NumSuspects = 0;
		uint64 Suspects[16] = {};
	};

	class MEMORYSENTINELHEAP_API FHeapSampler
	{
	public:
		/** True once the proxy is the engine's allocator. */
		static bool IsInstalled();
		/** Why it is not installed (empty when it is). */
		static FString WhyNotInstalled();
		/** Why the data breakpoint rotor is off (empty when it is on). */
		static FString WhyNoRotor();
		static FHeapStats Stats();
		/** Move every event recorded since the last call into Out. Game thread. */
		static int32 Drain(TArray<FHeapEvent>& Out);
		/** Check up to Budget sampled blocks (live canaries and quarantine patterns), round robin. */
		static void CheckSome(int32 Budget);
		/** Ask the rotor thread to pick new watch words and load them into every thread's DR0..DR3. */
		static void RequestRotation();
		/** Sample, and watch first, every block of this size from now on. */
		static void AddSuspectSize(uint64 Size);
		/** Called from the fault handler when a guard page is hit, before the crash continues to the engine. */
		static void SetEmergencyWriter(void (*Writer)());
	};
}
