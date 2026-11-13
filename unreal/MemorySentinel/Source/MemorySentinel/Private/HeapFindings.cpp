// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
#include "HeapFindings.h"

#include "SentinelFindings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"

using namespace MemorySentinel;

DEFINE_LOG_CATEGORY_STATIC(LogMemorySentinelHeapFindings, Log, All);

static TAutoConsoleVariable<int32> CVarCheckBudget(TEXT("ms.Heap.CheckBudget"), 256,
	TEXT("Sampled blocks whose canary (or quarantine pattern) is checked each frame."));
static TAutoConsoleVariable<int32> CVarRotateFrames(TEXT("ms.Heap.RotateFrames"), 15,
	TEXT("Frames between reloads of the four data breakpoints."));

namespace
{
	FHeapFindings* GHeapFindings = nullptr;

	const TCHAR* HeapBlindSpots[] = {
		TEXT("Only sampled blocks are checked: one allocation in N, plus every allocation of a suspect size. A stomp on an unsampled block is not seen."),
		TEXT("A canary catches a write past the end that changes it; a write that skips past the canary, or writes the same bytes, is not caught."),
		TEXT("The quarantine catches writes into freed blocks while they are held (up to 32 MB, 4096 blocks); reads after free are not caught, except on guarded blocks."),
		TEXT("Data breakpoints watch at most four 8-byte words at a time, on threads that existed at the last rotation."),
		TEXT("A block can be watched only from the first rotation after it is allocated (every ms.Heap.RotateFrames frames); a write past its end before that is caught by its canary at the next check, not at the instruction."),
		TEXT("A block allocated before the plugin loaded, or larger than 1 MB, is never sampled."),
		TEXT("Guard pages: an overrun shorter than the gap between the block's end and the page (under the block's alignment) is caught by its canary on free, not at the instruction; an underrun is reported against the block before it."),
	};

	FString Hex(const uint8* Bytes, uint32 Num)
	{
		FString S;
		for (uint32 I = 0; I < Num; ++I) { S += FString::Printf(TEXT("%s%02x"), I ? TEXT(" ") : TEXT(""), Bytes[I]); }
		return S;
	}

	bool IsPlumbing(const FString& Module, const FString& Function)
	{
		const FString M = Module.ToLower();
		if (M.Contains(TEXT("memorysentinelheap")) || M.EndsWith(TEXT("-core")) || M.EndsWith(TEXT("-core.dll"))
			|| M.Contains(TEXT("ntdll")) || M.Contains(TEXT("kernelbase")) || M.Contains(TEXT("kernel32"))
			|| M.Contains(TEXT("ucrtbase")) || M.Contains(TEXT("vcruntime")) || M.Contains(TEXT("msvcp")))
		{
			return true;
		}
		return Function.StartsWith(TEXT("FMemory::")) || Function.Contains(TEXT("operator new")) || Function.Contains(TEXT("operator delete"));
	}

	void EmergencyWrite() { FHeapFindings::WriteNow(); }
}

void FHeapFindings::Start()
{
	if (GHeapFindings || !FHeapSampler::IsInstalled()) { return; }
	GHeapFindings = new FHeapFindings();
	GHeapFindings->Started = FPlatformTime::Seconds();
	GHeapFindings->Path = MemorySentinelFiles::Dir() / FString::Printf(TEXT("heap-%s.json"), *MemorySentinelFiles::Stamp());
	GHeapFindings->TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(GHeapFindings, &FHeapFindings::Tick));
	FPlatformStackWalk::InitStackWalking();
	FHeapSampler::SetEmergencyWriter(&EmergencyWrite);
	FCoreDelegates::OnPreExit.AddStatic(&FHeapFindings::Stop);
	GHeapFindings->Write();   // a file from the start: it says the sampler ran, even when it finds nothing
}

void FHeapFindings::Stop()
{
	static bool bStopped = false;   // OnPreExit and ShutdownModule both call this
	if (!GHeapFindings || bStopped) { return; }
	bStopped = true;
	FHeapSampler::CheckSome(1 << 30);   // every sampled block, once, before the last write
	WriteNow();
	FTSTicker::GetCoreTicker().RemoveTicker(GHeapFindings->TickHandle);
	FHeapSampler::SetEmergencyWriter(nullptr);
	UE_LOG(LogMemorySentinelHeapFindings, Display, TEXT("Memory Sentinel heap findings: %d block(s) reported in %s"),
		GHeapFindings->Order.Num(), *GHeapFindings->Path);
	// GHeapFindings stays: the fault handler may still call WriteNow while the process ends
}

FString FHeapFindings::FilePath() { return GHeapFindings ? GHeapFindings->Path : FString(); }

void FHeapFindings::WriteNow()
{
	if (!GHeapFindings || !GHeapFindings->WriteLock.TryLock()) { return; }
	TArray<FHeapEvent> Events;
	FHeapSampler::Drain(Events);
	GHeapFindings->Absorb(Events);
	GHeapFindings->Write();
	GHeapFindings->WriteLock.Unlock();
}

bool FHeapFindings::Tick(float)
{
	++Frame;
	FHeapSampler::CheckSome(FMath::Max(0, CVarCheckBudget.GetValueOnGameThread()));
	MaxWatches = FMath::Max(MaxWatches, FHeapSampler::Stats().NumWatches);
	const int32 Every = FMath::Max(1, CVarRotateFrames.GetValueOnGameThread());
	if (Frame % Every == 0) { FHeapSampler::RequestRotation(); }
	TArray<FHeapEvent> Events;
	if (FHeapSampler::Drain(Events)) { Absorb(Events); }
	const double Now = FPlatformTime::Seconds();
	if (bDirty && Now - LastWrite > 1.0 && WriteLock.TryLock())
	{
		Write();
		WriteLock.Unlock();
	}
	return true;
}

void FHeapFindings::Absorb(const TArray<FHeapEvent>& Events)
{
	for (const FHeapEvent& E : Events)
	{
		FBlockRecord* R = Blocks.Find(E.Block);
		if (!R)
		{
			R = &Blocks.Add(E.Block);
			Order.Add(E.Block);
			R->First = E;
		}
		if (E.Kind == EHeapEvent::WatchedWrite)
		{
			if (!R->bHasWrite) { R->Write = E; R->bHasWrite = true; }
		}
		else if (!R->bHasCheck)
		{
			R->First = E;
			R->bHasCheck = true;
		}
		bDirty = true;
		UE_LOG(LogMemorySentinelHeapFindings, Warning, TEXT("Memory Sentinel: heap event %d on a %llu-byte block at 0x%llx, offset %lld"),
			int32(E.Kind), E.Size, E.Block, E.Offset);
	}
}

FString FHeapFindings::Symbol(uint64 Pc)
{
	if (const FString* S = Symbols.Find(Pc)) { return *S; }
	FProgramCounterSymbolInfo Info;
	FPlatformStackWalk::ProgramCounterToSymbolInfo(Pc, Info);
	const FString Function = ANSI_TO_TCHAR(Info.FunctionName);
	const FString File = ANSI_TO_TCHAR(Info.Filename);
	FString S = Function.IsEmpty() ? FString::Printf(TEXT("0x%llx"), Pc) : Function;
	if (!File.IsEmpty()) { S += FString::Printf(TEXT(" (%s:%d)"), *FPaths::GetCleanFilename(File), Info.LineNumber); }
	S += FString::Printf(TEXT(" [%s]"), ANSI_TO_TCHAR(Info.ModuleName));
	Symbols.Add(Pc, S);
	return S;
}

TArray<TSharedPtr<FJsonValue>> FHeapFindings::StackJson(const FHeapStack& Stack)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (uint32 I = 0; I < Stack.Num; ++I) { Out.Add(MakeShared<FJsonValueString>(Symbol(Stack.Frames[I]))); }
	return Out;
}

bool FHeapFindings::FirstUserFrame(const FHeapStack& Stack, FString& OutFile, int32& OutLine, FString& OutFunction)
{
	for (uint32 I = 0; I < Stack.Num; ++I)
	{
		FProgramCounterSymbolInfo Info;
		FPlatformStackWalk::ProgramCounterToSymbolInfo(Stack.Frames[I], Info);
		const FString Module = ANSI_TO_TCHAR(Info.ModuleName);
		const FString Function = ANSI_TO_TCHAR(Info.FunctionName);
		if (Function.IsEmpty() || IsPlumbing(Module, Function)) { continue; }
		OutFile = ANSI_TO_TCHAR(Info.Filename);
		OutLine = Info.LineNumber;
		OutFunction = Function;
		return true;
	}
	return false;
}

void FHeapFindings::Write()
{
	const FHeapStats S = FHeapSampler::Stats();
	FSentinelFindingsFile File;
	File.Kind = TEXT("heap");
	File.bOk = S.bInstalled;
	if (!S.bInstalled) { File.Failure = FHeapSampler::WhyNotInstalled(); }
	File.Target->SetStringField(TEXT("project"), FApp::GetProjectName());
	File.Target->SetStringField(TEXT("command_line"), FCommandLine::Get());
	File.Checked->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - Started);
	File.Checked->SetNumberField(TEXT("sample_every"), S.SampleEvery);
	File.Checked->SetNumberField(TEXT("guard_every"), S.GuardEvery);
	File.Checked->SetNumberField(TEXT("allocations"), double(S.Allocations));
	File.Checked->SetNumberField(TEXT("sampled"), double(S.Sampled));
	File.Checked->SetNumberField(TEXT("guard_sampled"), double(S.GuardSampled));
	File.Checked->SetNumberField(TEXT("live"), double(S.Live));
	File.Checked->SetNumberField(TEXT("quarantined"), double(S.Quarantined));
	File.Checked->SetNumberField(TEXT("canary_checks"), double(S.CanaryChecks));
	File.Checked->SetNumberField(TEXT("skipped_table_full"), double(S.SkippedFull));
	File.Checked->SetNumberField(TEXT("rotations"), double(S.Rotations));
	File.Checked->SetNumberField(TEXT("threads_programmed"), double(S.ThreadsProgrammed));
	File.Checked->SetNumberField(TEXT("watched_writes"), double(S.WatchedWrites));
	File.Checked->SetNumberField(TEXT("max_watches"), FMath::Max(MaxWatches, S.NumWatches));
	File.Checked->SetNumberField(TEXT("dropped_events"), double(S.DroppedEvents));
	File.Checked->SetStringField(TEXT("rotor"), S.bRotor ? FString(TEXT("on")) : FHeapSampler::WhyNoRotor());
	TArray<TSharedPtr<FJsonValue>> Suspects;
	for (int32 K = 0; K < S.NumSuspects; ++K) { Suspects.Add(MakeShared<FJsonValueNumber>(double(S.Suspects[K]))); }
	File.Checked->SetArrayField(TEXT("suspect_sizes"), Suspects);
	for (const TCHAR* B : HeapBlindSpots) { File.BlindSpots.Add(B); }

	for (const uint64 Block : Order)
	{
		const FBlockRecord& R = Blocks[Block];
		const FHeapEvent& E = R.bHasCheck ? R.First : R.Write;
		const bool bPastEnd = E.Offset >= int64(E.Size);
		FSentinelFinding F;
		FString Caught;
		FString AllocFile, AllocFn, WriteFile, WriteFn;
		int32 AllocLine = 0, WriteLine = 0;
		FirstUserFrame(E.AllocStack, AllocFile, AllocLine, AllocFn);
		// the double free's stack is its second free; a guard fault's is the faulting access; otherwise a watched write's
		const bool bDouble = E.Kind == EHeapEvent::DoubleFree;
		const bool bWriter = bDouble || R.bHasWrite || E.Kind == EHeapEvent::GuardOverrun || E.Kind == EHeapEvent::GuardAfterFree;
		const FHeapStack& WStack = (bDouble || !R.bHasWrite) ? E.WriteStack : R.Write.WriteStack;
		if (bWriter) { FirstUserFrame(WStack, WriteFile, WriteLine, WriteFn); }
		const FString AllocAt = AllocFn.IsEmpty() ? FString(TEXT("an unknown caller")) : FString::Printf(TEXT("%s (%s:%d)"), *AllocFn, *FPaths::GetCleanFilename(AllocFile), AllocLine);
		const FString WriteAt = WriteFn.IsEmpty() ? FString() : FString::Printf(TEXT("%s (%s:%d)"), *WriteFn, *FPaths::GetCleanFilename(WriteFile), WriteLine);
		const FString Dr = R.bHasWrite ? FString::Printf(TEXT("; the CPU's DR%d data breakpoint fired on thread %u writing 0x%llx"), R.Write.Register, R.Write.ThreadId, R.Write.Address) : FString();
		const int64 Past = E.Offset - int64(E.Size);
		switch (E.Kind)
		{
		case EHeapEvent::Overrun:
		case EHeapEvent::WatchedWrite:
			if (E.Kind == EHeapEvent::Overrun || bPastEnd)
			{
				F.Rule = TEXT("heap-overrun");
				F.Title = FString::Printf(TEXT("Write past the end of a %llu-byte block"), E.Size);
				F.Why = E.Kind == EHeapEvent::Overrun
					? FString::Printf(TEXT("The block was given a canary, known bytes right after its end. %u of the %u canary bytes changed, starting %s, so something wrote past the block."), E.Changed, E.Compared,
						Past == 0 ? TEXT("at the first byte past the end") : *FString::Printf(TEXT("%lld bytes past the end"), Past))
					: FString::Printf(TEXT("A data breakpoint watching the canary %lld bytes past the end of the block caught a write to it."), Past);
				Caught = R.bHasWrite ? (R.bHasCheck ? TEXT("canary and data breakpoint") : TEXT("data breakpoint")) : TEXT("canary");
				F.HowKnown = E.Kind == EHeapEvent::Overrun
					? FString::Printf(TEXT("READ: the canary after the block was read back from memory; %u of %u bytes differ from the pattern%s"), E.Changed, E.Compared, *Dr)
					: FString::Printf(TEXT("READ%s"), *Dr);
				break;
			}
			// a watched write inside a quarantined block, with no pattern check yet
			F.Rule = TEXT("write-after-free");
			F.Title = FString::Printf(TEXT("Write into a %llu-byte block after it was freed"), E.Size);
			F.Why = TEXT("A data breakpoint watching the first word of a freed block (held in quarantine, so nothing else owns it) caught a write to it.");
			Caught = TEXT("data breakpoint");
			F.HowKnown = FString::Printf(TEXT("READ%s"), *Dr);
			break;
		case EHeapEvent::WriteAfterFree:
			F.Rule = TEXT("write-after-free");
			F.Title = FString::Printf(TEXT("Write into a %llu-byte block after it was freed"), E.Size);
			F.Why = FString::Printf(TEXT("The block was freed and held in quarantine, filled with a known pattern, so nothing else owned it. %u of its %u bytes changed, starting at offset %lld: code kept writing through a pointer to it after the free."), E.Changed, E.Compared, E.Offset);
			Caught = R.bHasWrite ? TEXT("quarantine pattern and data breakpoint") : TEXT("quarantine pattern");
			F.HowKnown = FString::Printf(TEXT("READ: the freed block's pattern was read back from memory; %u of %u bytes differ%s"), E.Changed, E.Compared, *Dr);
			break;
		case EHeapEvent::DoubleFree:
			F.Rule = TEXT("double-free");
			F.Title = FString::Printf(TEXT("A %llu-byte block was freed twice"), E.Size);
			F.Why = TEXT("The block was already freed and held in quarantine when it was freed again. Without the quarantine the second free would release memory that may belong to something else by then.");
			Caught = TEXT("quarantine");
			F.HowKnown = TEXT("READ: Free was called on a block that was in the sampler's quarantine");
			break;
		case EHeapEvent::GuardOverrun:
		case EHeapEvent::GuardAfterFree:
			F.Rule = TEXT("guard-fault");
			F.Title = E.Kind == EHeapEvent::GuardOverrun
				? FString::Printf(TEXT("%s past the end of a %llu-byte block hit its guard page"), E.bWrite ? TEXT("A write") : TEXT("A read"), E.Size)
				: FString::Printf(TEXT("%s a %llu-byte block after it was freed"), E.bWrite ? TEXT("Wrote") : TEXT("Read"), E.Size);
			F.Why = E.Kind == EHeapEvent::GuardOverrun
				? FString::Printf(TEXT("The block sat right against a no-access page. The access %lld byte(s) past its end touched that page, and the CPU stopped it at the exact instruction; the game crashes there."), Past)
				: TEXT("The block's page was made no-access when it was freed. The access touched it, and the CPU stopped it at the exact instruction; the game crashes there.");
			Caught = TEXT("guard page");
			F.HowKnown = FString::Printf(TEXT("READ: the CPU raised an access violation at 0x%llx, inside the sampler's guard slot for this block"), E.Address);
			break;
		}
		F.Severity = TEXT("error");
		if (bDouble)
		{
			FString FreeFile, FreeFn;
			int32 FreeLine = 0;
			FirstUserFrame(E.FreeStack, FreeFile, FreeLine, FreeFn);
			const FString FirstAt = FreeFn.IsEmpty() ? FString(TEXT("an unknown caller")) : FString::Printf(TEXT("%s (%s:%d)"), *FreeFn, *FPaths::GetCleanFilename(FreeFile), FreeLine);
			F.Why += FString::Printf(TEXT(" First freed in %s; freed again in %s."), *FirstAt, WriteAt.IsEmpty() ? TEXT("an unknown caller") : *WriteAt);
			F.Fix = FString::Printf(TEXT("Give the block one owner, or clear the pointer after the first free in %s. It was allocated in %s."), *FirstAt, *AllocAt);
		}
		else if (!WriteAt.IsEmpty())
		{
			F.Why += FString::Printf(TEXT(" The write was stopped in %s."), *WriteAt);
			F.Fix = FString::Printf(TEXT("Fix the access in %s: it treats the block as %s. The block was allocated in %s."), *WriteAt,
				F.Rule == TEXT("heap-overrun") || E.Kind == EHeapEvent::GuardOverrun ? TEXT("bigger than it is") : TEXT("still alive"), *AllocAt);
		}
		else
		{
			F.Fix = FString::Printf(TEXT("Run again with -MemorySentinel -MemorySentinelSuspectSizes=%llu: every %llu-byte block is then sampled and watched by a data breakpoint first, which stops the write itself. The block was allocated in %s."), E.Size, E.Size, *AllocAt);
		}
		const bool bWhereWriter = !WriteFile.IsEmpty();
		if (bWhereWriter || !AllocFile.IsEmpty())
		{
			F.Where->SetStringField(TEXT("file"), bWhereWriter ? WriteFile : AllocFile);
			F.Where->SetNumberField(TEXT("line"), bWhereWriter ? WriteLine : AllocLine);
			F.Where->SetStringField(TEXT("code"), bWhereWriter ? WriteFn : AllocFn);
		}
		F.Where->SetStringField(TEXT("object"), FString::Printf(TEXT("block 0x%llx, %llu bytes"), E.Block, E.Size));
		F.Evidence->SetStringField(TEXT("block"), FString::Printf(TEXT("0x%llx"), E.Block));
		F.Evidence->SetNumberField(TEXT("size"), double(E.Size));
		F.Evidence->SetNumberField(TEXT("offset"), double(E.Offset));
		F.Evidence->SetNumberField(TEXT("past_end"), double(Past));
		F.Evidence->SetNumberField(TEXT("changed"), E.Changed);
		F.Evidence->SetNumberField(TEXT("compared"), E.Compared);
		F.Evidence->SetStringField(TEXT("expected"), Hex(E.Expected, E.Shown));
		F.Evidence->SetStringField(TEXT("found"), Hex(E.Found, E.Shown));
		F.Evidence->SetStringField(TEXT("caught_by"), Caught);
		F.Evidence->SetNumberField(TEXT("allocation_number"), double(E.Serial));
		F.Evidence->SetArrayField(TEXT("alloc_stack"), StackJson(E.AllocStack));
		F.Evidence->SetArrayField(TEXT("free_stack"), StackJson(E.FreeStack));
		if (bWriter)
		{
			F.Evidence->SetArrayField(bDouble ? TEXT("second_free_stack") : TEXT("write_stack"), StackJson(WStack));
			F.Evidence->SetNumberField(TEXT("thread"), R.bHasWrite ? R.Write.ThreadId : E.ThreadId);
		}
		if (R.bHasWrite)
		{
			F.Evidence->SetStringField(TEXT("register"), FString::Printf(TEXT("DR%d"), R.Write.Register));
			F.Evidence->SetStringField(TEXT("watched_word"), FString::Printf(TEXT("0x%llx"), R.Write.Address));
		}
		File.Findings.Add(F);
	}
	File.Write(Path, TEXT("HEAP"));
	bDirty = false;
	LastWrite = FPlatformTime::Seconds();
}
