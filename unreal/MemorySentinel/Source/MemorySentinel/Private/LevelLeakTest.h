// The level leak test (UC-2, R4). It records every live object, opens a map, lets it run, opens the return map, collects
// garbage twice, and lists what is still alive that should not be: whatever belonged to the map it left, actors and
// components outside any live world, and objects of the project's own classes made during the level. For each survivor
// the engine's own reference search (FReferenceChainSearch, the code behind `obj refs`) gives the chain that holds it.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/ObjectKey.h"
#include "Engine/EngineBaseTypes.h"

class UWorld;

class FLevelLeakTest : public TSharedFromThis<FLevelLeakTest>
{
public:
	struct FOptions
	{
		FString Map;
		FString ReturnMap = TEXT("/Engine/Maps/Entry");
		float PlaySeconds = 5.f;
		bool bQuit = false;          // end the game when done; the findings file holds the result (ms.py turns it into an exit code)
		int32 MaxChains = 40;
	};

	/** Starts a test, or says why it cannot (another test is running, no game world). */
	static bool Start(const FOptions& Options, FString& OutWhyNot);
	static bool IsRunning();
	/** The findings file of the last finished test, or empty. */
	static FString LastFile();

private:
	enum class EPhase : uint8 { Opening, Playing, Leaving, Collecting, Done };

	explicit FLevelLeakTest(const FOptions& InOptions) : Options(InOptions) {}
	bool Tick(float DeltaTime);
	void OnMapLoaded(UWorld* World);
	void OnTravelFailure(UWorld* World, ETravelFailure::Type Type, const FString& Error);
	void Fail(const FString& Why);
	void Finish();
	void Report();
	void Cleanup();
	static FString PackageOf(const FString& Map);

	FOptions Options;
	EPhase Phase = EPhase::Opening;
	double PhaseStart = 0.0;
	int32 CollectTicks = 0;
	bool bInTick = false;      // the ticker removes itself by returning false, never from inside its own call
	FString TestPackage;
	FString ReturnPackage;
	TWeakObjectPtr<UWorld> TestWorld;
	FObjectKey TestWorldKey;
	TSet<FObjectKey> Baseline;
	TSet<FObjectKey> LevelObjects;
	int32 LevelObjectCount = 0;
	TSet<FString> ProjectScripts;
	FTSTicker::FDelegateHandle TickHandle;
	FDelegateHandle LoadedHandle;
	FDelegateHandle TravelHandle;
	FString WrittenFile;

	static TSharedPtr<FLevelLeakTest> Active;
	static FString Last;
};
