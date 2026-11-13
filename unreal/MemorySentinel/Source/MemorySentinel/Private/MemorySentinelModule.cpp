// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
// The runtime module: console commands, the heap findings writer, and the in-game overlay (UC-2.1, UC-3.7, UC-4.1).
//   ms.LeakTest <map> [-seconds=5] [-return=/Engine/Maps/Entry] [-quit]   the level leak test
//   ms.Heap                      the heap sampler's counters and what it watches now
//   ms.Heap.Write                write the heap findings file now
//   ms.Suspect <bytes>           sample and watch every block of this size from now on
//   ms.Show [rule]               the panel over the game (again to hide); with a rule, select its first finding
//   ms.Shot <file.png>           a screenshot of the viewport with the panel on it
#include "Modules/ModuleManager.h"

#include "HeapFindings.h"
#include "LevelLeakTest.h"
#include "SentinelFindings.h"
#include "SentinelHeap.h"
#include "SSentinelPanel.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "Containers/Ticker.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SWeakWidget.h"

DEFINE_LOG_CATEGORY_STATIC(LogMemorySentinel, Log, All);

namespace
{
	TSharedPtr<SWidget> GOverlay;
	TSharedPtr<SSentinelPanel> GOverlayPanel;

	void LeakTest(const TArray<FString>& Args)
	{
		FLevelLeakTest::FOptions O;
		for (const FString& A : Args)
		{
			if (A == TEXT("-quit")) { O.bQuit = true; }
			else if (A.StartsWith(TEXT("-seconds="))) { O.PlaySeconds = FCString::Atof(*A.Mid(9)); }
			else if (A.StartsWith(TEXT("-return="))) { O.ReturnMap = A.Mid(8); }
			else if (!A.StartsWith(TEXT("-"))) { O.Map = A; }
		}
		FString Why;
		if (!FLevelLeakTest::Start(O, Why))
		{
			UE_LOG(LogMemorySentinel, Error, TEXT("ms.LeakTest: %s"), *Why);
			if (O.bQuit) { FPlatformMisc::RequestExit(false, TEXT("MemorySentinel leak test")); }
		}
	}

	void Heap(const TArray<FString>&)
	{
		using namespace MemorySentinel;
		if (!FHeapSampler::IsInstalled())
		{
			UE_LOG(LogMemorySentinel, Display, TEXT("Heap sampler off: %s"), *FHeapSampler::WhyNotInstalled());
			return;
		}
		const FHeapStats S = FHeapSampler::Stats();
		UE_LOG(LogMemorySentinel, Display, TEXT("Heap sampler: 1 in %u; %llu allocations, %llu sampled, %llu guarded, %llu live, %llu in quarantine, %llu checks, %llu rotations over %llu thread loads, %llu watched writes, %llu events (%llu dropped), %llu skipped (table full)"),
			S.SampleEvery, S.Allocations, S.Sampled, S.GuardSampled, S.Live, S.Quarantined, S.CanaryChecks, S.Rotations, S.ThreadsProgrammed, S.WatchedWrites, S.Events, S.DroppedEvents, S.SkippedFull);
		for (int32 K = 0; K < S.NumWatches; ++K)
		{
			UE_LOG(LogMemorySentinel, Display, TEXT("  DR%d watches 0x%llx (%s of a %llu-byte block at 0x%llx%s)"), K, S.Watches[K].Address,
				S.Watches[K].bQuarantined ? TEXT("first word") : TEXT("canary"), S.Watches[K].Size, S.Watches[K].Block,
				S.Watches[K].bSuspect ? TEXT(", suspect size") : TEXT(""));
		}
		if (!S.bRotor) { UE_LOG(LogMemorySentinel, Display, TEXT("  rotor off: %s"), *FHeapSampler::WhyNoRotor()); }
	}

	void Show(const TArray<FString>& Args)
	{
		if (!GEngine || !GEngine->GameViewport)
		{
			UE_LOG(LogMemorySentinel, Error, TEXT("ms.Show needs a game viewport; in the editor use Tools > Memory Sentinel"));
			return;
		}
		if (GOverlay.IsValid() && Args.Num() == 0)
		{
			GEngine->GameViewport->RemoveViewportWidgetContent(GOverlay.ToSharedRef());
			GOverlay.Reset();
			GOverlayPanel.Reset();
			return;
		}
		if (!GOverlay.IsValid())
		{
			GOverlay = SNew(SBox).Padding(FMargin(40, 30))
			[
				SAssignNew(GOverlayPanel, SSentinelPanel)
			];
			GEngine->GameViewport->AddViewportWidgetContent(GOverlay.ToSharedRef(), 1000);
		}
		if (Args.Num() && GOverlayPanel.IsValid())
		{
			GOverlayPanel->Reload();
			if (!GOverlayPanel->SelectFirst(Args[0]))
			{
				UE_LOG(LogMemorySentinel, Warning, TEXT("ms.Show: no finding of rule %s"), *Args[0]);
			}
		}
	}

	void Shot(const TArray<FString>& Args)
	{
		if (Args.Num() == 0) { UE_LOG(LogMemorySentinel, Error, TEXT("ms.Shot <file.png>")); return; }
		FScreenshotRequest::RequestScreenshot(Args[0], true, false);
		UE_LOG(LogMemorySentinel, Display, TEXT("ms.Shot: requested %s"), *Args[0]);
	}

	FAutoConsoleCommand CmdLeak(TEXT("ms.LeakTest"), TEXT("ms.LeakTest <map> [-seconds=5] [-return=/Engine/Maps/Entry] [-quit]: open the map, leave it, collect garbage, list what survived."), FConsoleCommandWithArgsDelegate::CreateStatic(&LeakTest));
	FAutoConsoleCommand CmdHeap(TEXT("ms.Heap"), TEXT("The heap sampler's counters and the words the data breakpoints watch now."), FConsoleCommandWithArgsDelegate::CreateStatic(&Heap));
	FAutoConsoleCommand CmdHeapWrite(TEXT("ms.Heap.Write"), TEXT("Write the heap findings file now."), FConsoleCommandDelegate::CreateStatic(&FHeapFindings::WriteNow));
	FAutoConsoleCommand CmdSuspect(TEXT("ms.Suspect"), TEXT("ms.Suspect <bytes>: sample, and watch first, every block of this size from now on."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
			for (const FString& A : Args) { MemorySentinel::FHeapSampler::AddSuspectSize(FCString::Strtoui64(*A, nullptr, 10)); }
		}));
	FAutoConsoleCommand CmdShow(TEXT("ms.Show"), TEXT("ms.Show [rule]: the Memory Sentinel panel over the game (again to hide)."), FConsoleCommandWithArgsDelegate::CreateStatic(&Show));
	FAutoConsoleCommand CmdQuit(TEXT("ms.QuitAfter"), TEXT("ms.QuitAfter <seconds>: end the game after this long (headless runs)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
			const float Seconds = Args.Num() ? FCString::Atof(*Args[0]) : 10.f;
			const double At = FPlatformTime::Seconds() + Seconds;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([At](float) {
				if (FPlatformTime::Seconds() < At) { return true; }
				UE_LOG(LogMemorySentinel, Display, TEXT("ms.QuitAfter: time is up"));
				FPlatformMisc::RequestExit(false, TEXT("ms.QuitAfter"));
				return false;
			}));
		}));
	FAutoConsoleCommand CmdAfter(TEXT("ms.After"), TEXT("ms.After <seconds> <command...>: run a console command later (scripted headless runs)."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
			if (Args.Num() < 2) { UE_LOG(LogMemorySentinel, Error, TEXT("ms.After <seconds> <command...>")); return; }
			const double At = FPlatformTime::Seconds() + FCString::Atof(*Args[0]);
			const FString Command = FString::Join(TArrayView<const FString>(Args).RightChop(1), TEXT(" "));
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([At, Command](float) {
				if (FPlatformTime::Seconds() < At) { return true; }
				if (GEngine) { GEngine->Exec(GEngine->GetCurrentPlayWorld(), *Command); }
				return false;
			}));
		}));
	FAutoConsoleCommand CmdShot(TEXT("ms.Shot"), TEXT("ms.Shot <file.png>: a screenshot of the viewport, panel included."), FConsoleCommandWithArgsDelegate::CreateStatic(&Shot));
}

class FMemorySentinelModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FHeapFindings::Start();
		UE_LOG(LogMemorySentinel, Display, TEXT("Memory Sentinel: findings go to %s; heap sampler %s"), *MemorySentinelFiles::Dir(),
			MemorySentinel::FHeapSampler::IsInstalled() ? TEXT("on") : *(TEXT("off (") + MemorySentinel::FHeapSampler::WhyNotInstalled() + TEXT(")")));
	}
	virtual void ShutdownModule() override
	{
		FHeapFindings::Stop();
		GOverlay.Reset();
		GOverlayPanel.Reset();
	}
};

IMPLEMENT_MODULE(FMemorySentinelModule, MemorySentinel)
