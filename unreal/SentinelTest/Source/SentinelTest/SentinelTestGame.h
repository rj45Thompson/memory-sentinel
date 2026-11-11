// The test game's runtime half. One scenario per run, picked with -SentinelScenario=<name>:
//   leak        an actor in the test map makes a cache object that a game instance subsystem keeps forever (UC-2)
//   leakclean   the same map with nothing planted: the leak test must report no survivor
//   stomp       a 1234-byte block written 16 bytes past its end (UC-3.4, 3.5)
//   stompfree   the same, then freed in the same frame: only the check on free can see it (UC-3.3)
//   uaf         a 1234-byte block written after it was freed (UC-3.3, 3.5)
//   doublefree  a 1234-byte block freed twice
//   guard       a 777-byte guarded block written past its end: the guard page stops the write and the game crashes (UC-3.6)
//   heapclean   nothing planted, the sampler on 1 in 4 with guard pages: the heap file must hold no finding
// Every planted write lives in its own named function (SentinelScenarios.cpp), so a stack that names it is the proof.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/GameModeBase.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SentinelTestGame.generated.h"

/** Made by the leaky actor, once per level. Nothing ever lets go of it: the leak the test must find. */
UCLASS()
class USentinelLevelCache : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<FVector> Points;
	UPROPERTY()
	FString MadeIn;
};

/** Lives as long as the game instance, across every level: a place where per-level objects pile up. */
UCLASS()
class USentinelTestKeeper : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	UPROPERTY()
	TArray<TObjectPtr<USentinelLevelCache>> Caches;
};

UCLASS()
class ASentinelLeakyActor : public AActor
{
	GENERATED_BODY()
public:
	virtual void BeginPlay() override;
};

UCLASS()
class ASentinelTestGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	ASentinelTestGameMode();
private:
	FString Scenario;
	double Started = 0.0;
	int32 Step = 0;
	uint8* Block = nullptr;
};
