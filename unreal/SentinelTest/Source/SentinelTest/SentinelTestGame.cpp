#include "SentinelTestGame.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, SentinelTest, "SentinelTest");

DEFINE_LOG_CATEGORY_STATIC(LogSentinelTest, Log, All);

// the planted heap bugs (SentinelScenarios.cpp)
void SentinelPlant_HeapOverrun(uint8* Block, int32 Size);
void SentinelPlant_WriteAfterFree(uint8* Block);
void SentinelPlant_DoubleFree(uint8* Block);
void SentinelPlant_GuardOverrun(uint8* Block, int32 Size);

void ASentinelLeakyActor::BeginPlay()
{
	Super::BeginPlay();
	// PLANT (runtime): a per-level cache outered to the transient package, kept by a subsystem that outlives the level
	USentinelLevelCache* Cache = NewObject<USentinelLevelCache>(GetTransientPackage(), TEXT("LevelCache"));
	Cache->MadeIn = GetWorld()->GetOutermost()->GetName();
	Cache->Points.Add(GetActorLocation());
	GetGameInstance()->GetSubsystem<USentinelTestKeeper>()->Caches.Add(Cache);
	UE_LOG(LogSentinelTest, Display, TEXT("SentinelTest: planted a level cache kept by USentinelTestKeeper::Caches"));
}

ASentinelTestGameMode::ASentinelTestGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ASentinelTestGameMode::BeginPlay()
{
	Super::BeginPlay();
	FParse::Value(FCommandLine::Get(), TEXT("SentinelScenario="), Scenario);
	Started = FPlatformTime::Seconds();
	const FString Map = GetWorld()->GetOutermost()->GetName();
	UE_LOG(LogSentinelTest, Display, TEXT("SentinelTest: scenario '%s' in %s"), *Scenario, *Map);
	if (Scenario == TEXT("leak") && Map == TEXT("/Engine/Maps/Templates/Template_Default"))
	{
		GetWorld()->SpawnActor<ASentinelLeakyActor>();
	}
}

void ASentinelTestGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double T = FPlatformTime::Seconds() - Started;
	if (Scenario == TEXT("heapclean") && T > 3.0 && Step == 0)
	{
		// the clean control travels too: Entry -> Template_Default -> Entry, so world teardown runs under the sampler
		static int32 Travels = 0;
		Step = 1;
		if (Travels < 2)
		{
			++Travels;
			GEngine->Exec(GetWorld(), Travels == 1 ? TEXT("open /Engine/Maps/Templates/Template_Default") : TEXT("open /Engine/Maps/Entry"));
		}
		return;
	}
	const bool bHeap = Scenario == TEXT("stomp") || Scenario == TEXT("stompfree") || Scenario == TEXT("uaf") || Scenario == TEXT("doublefree") || Scenario == TEXT("guard");
	if (!bHeap) { return; }
	const int32 Size = Scenario == TEXT("guard") ? 777 : 1234;
	if (Step == 0 && T > 1.0)
	{
		Block = static_cast<uint8*>(FMemory::Malloc(Size));
		FMemory::Memzero(Block, Size);
		if (Scenario == TEXT("uaf")) { FMemory::Free(Block); }
		UE_LOG(LogSentinelTest, Display, TEXT("SentinelTest: allocated a %d-byte block at %p%s"), Size, Block, Scenario == TEXT("uaf") ? TEXT(" and freed it") : TEXT(""));
		Step = 1;
	}
	else if (Step == 1 && T > 2.5)
	{
		UE_LOG(LogSentinelTest, Display, TEXT("SentinelTest: planting '%s' now"), *Scenario);
		if (Scenario == TEXT("stomp")) { SentinelPlant_HeapOverrun(Block, Size); }
		else if (Scenario == TEXT("stompfree")) { SentinelPlant_HeapOverrun(Block, Size); FMemory::Free(Block); }
		else if (Scenario == TEXT("uaf")) { SentinelPlant_WriteAfterFree(Block); }
		else if (Scenario == TEXT("doublefree")) { SentinelPlant_DoubleFree(Block); }
		else if (Scenario == TEXT("guard")) { SentinelPlant_GuardOverrun(Block, Size); }
		Step = 2;
	}
}
