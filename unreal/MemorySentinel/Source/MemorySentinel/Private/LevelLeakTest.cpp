#include "LevelLeakTest.h"

#include "SentinelFindings.h"
#include "Components/ActorComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/PackageName.h"
#include "ProjectDescriptor.h"
#include "UObject/GCObjectInfo.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogMemorySentinelLeak, Log, All);

TSharedPtr<FLevelLeakTest> FLevelLeakTest::Active;
FString FLevelLeakTest::Last;

namespace
{
	const TCHAR* BlindSpots[] = {
		TEXT("Only what this run played is tested: code the level did not reach during the run is not exercised."),
		TEXT("A dangling raw pointer keeps nothing alive, so this test cannot see one. The static pass and the heap sampler can."),
		TEXT("Objects of engine classes made during the level outside the map's own world are not counted; actors, components and the project's own classes are."),
		TEXT("An object made before the map was opened, and kept, is not counted unless it belongs to the map."),
		TEXT("The reference search reports the shortest chain to a root. Other chains may hold the same object; fixing one link can leave another."),
	};

	UWorld* GameWorld()
	{
		if (!GEngine) { return nullptr; }
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			if ((C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE) && C.World()) { return C.World(); }
		}
		return nullptr;
	}

	FString Via(const FReferenceChainSearch::FNodeReferenceInfo* Info)
	{
		if (!Info) { return TEXT("root"); }
		switch (Info->Type)
		{
		case FReferenceChainSearch::EReferenceType::Property:
			return FString::Printf(TEXT("property %s"), *Info->ReferencerName);
		case FReferenceChainSearch::EReferenceType::AddReferencedObjects:
			return Info->ReferencerName.IsEmpty() ? FString(TEXT("AddReferencedObjects"))
				: FString::Printf(TEXT("AddReferencedObjects (%s)"), *Info->ReferencerName);
		case FReferenceChainSearch::EReferenceType::OuterChain:
			return TEXT("outer");
		default:
			return TEXT("an unnamed reference");
		}
	}
}

bool FLevelLeakTest::IsRunning() { return Active.IsValid(); }
FString FLevelLeakTest::LastFile() { return Last; }

FString FLevelLeakTest::PackageOf(const FString& Map)
{
	FString P = Map;
	int32 Dot;
	if (P.FindChar(TEXT('.'), Dot)) { P.LeftInline(Dot); }
	return P;
}

bool FLevelLeakTest::Start(const FOptions& Options, FString& OutWhyNot)
{
	if (Active.IsValid()) { OutWhyNot = TEXT("a leak test is already running"); return false; }
	UWorld* World = GameWorld();
	if (!World) { OutWhyNot = TEXT("the leak test needs a running game: start it with -game, or in Play In Editor"); return false; }
	if (Options.Map.IsEmpty()) { OutWhyNot = TEXT("no map given: ms.LeakTest /Game/Maps/YourMap"); return false; }
	if (!FPackageName::DoesPackageExist(PackageOf(Options.Map)))
	{
		OutWhyNot = FString::Printf(TEXT("there is no map package %s"), *PackageOf(Options.Map));
		return false;
	}
	TSharedPtr<FLevelLeakTest> T = MakeShareable(new FLevelLeakTest(Options));
	T->TestPackage = PackageOf(Options.Map);
	T->ReturnPackage = PackageOf(Options.ReturnMap);

	// the project's own modules: its own and its plugins', as /Script/<Module> packages
	if (const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject())
	{
		for (const FModuleDescriptor& M : Project->Modules) { T->ProjectScripts.Add(TEXT("/Script/") + M.Name.ToString()); }
	}
	for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
	{
		if (Plugin->GetLoadedFrom() == EPluginLoadedFrom::Project)
		{
			for (const FModuleDescriptor& M : Plugin->GetDescriptor().Modules) { T->ProjectScripts.Add(TEXT("/Script/") + M.Name.ToString()); }
		}
	}

	// step 1: what is alive before the map opens
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	for (FThreadSafeObjectIterator It; It; ++It) { T->Baseline.Add(FObjectKey(*It)); }

	T->LoadedHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddSP(T.ToSharedRef(), &FLevelLeakTest::OnMapLoaded);
	T->TravelHandle = GEngine->OnTravelFailure().AddSP(T.ToSharedRef(), &FLevelLeakTest::OnTravelFailure);
	T->TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(T.ToSharedRef(), &FLevelLeakTest::Tick));
	T->Phase = EPhase::Opening;
	T->PhaseStart = FPlatformTime::Seconds();
	Active = T;
	UE_LOG(LogMemorySentinelLeak, Display, TEXT("Memory Sentinel leak test: %d objects alive before; opening %s"), T->Baseline.Num(), *T->TestPackage);
	GEngine->Exec(World, *FString::Printf(TEXT("open %s"), *T->TestPackage));
	return true;
}

void FLevelLeakTest::OnMapLoaded(UWorld* World)
{
	if (!World) { return; }
	const FString Pkg = World->GetOutermost()->GetName();
	if (Phase == EPhase::Opening && Pkg == TestPackage)
	{
		TestWorld = World;
		TestWorldKey = FObjectKey(World);
		Phase = EPhase::Playing;
		PhaseStart = FPlatformTime::Seconds();
		UE_LOG(LogMemorySentinelLeak, Display, TEXT("Memory Sentinel leak test: %s loaded, playing %.1f s"), *Pkg, Options.PlaySeconds);
	}
	else if (Phase == EPhase::Leaving && Pkg == ReturnPackage)
	{
		Phase = EPhase::Collecting;
		CollectTicks = 0;
		PhaseStart = FPlatformTime::Seconds();
	}
}

void FLevelLeakTest::OnTravelFailure(UWorld*, ETravelFailure::Type, const FString& Error)
{
	Fail(FString::Printf(TEXT("the engine could not open the map: %s"), *Error));
}

bool FLevelLeakTest::Tick(float)
{
	TGuardValue<bool> InTick(bInTick, true);
	const double Now = FPlatformTime::Seconds();
	switch (Phase)
	{
	case EPhase::Opening:
		if (Now - PhaseStart > 60.0) { Fail(FString::Printf(TEXT("%s did not load within 60 s"), *TestPackage)); }
		break;
	case EPhase::Playing:
		if (Now - PhaseStart >= Options.PlaySeconds)
		{
			// everything that belongs to the level now: its package, or its world
			UWorld* W = TestWorld.Get();
			for (FThreadSafeObjectIterator It; It; ++It)
			{
				UObject* O = *It;
				if (O->GetOutermost()->GetName() == TestPackage || (W && (O == W || O->GetTypedOuter<UWorld>() == W)))
				{
					LevelObjects.Add(FObjectKey(O));
				}
			}
			LevelObjectCount = LevelObjects.Num();
			Phase = EPhase::Leaving;
			PhaseStart = Now;
			UE_LOG(LogMemorySentinelLeak, Display, TEXT("Memory Sentinel leak test: the level holds %d objects; opening %s"), LevelObjectCount, *ReturnPackage);
			if (UWorld* G = GameWorld()) { GEngine->Exec(G, *FString::Printf(TEXT("open %s"), *ReturnPackage)); }
		}
		break;
	case EPhase::Leaving:
		if (Now - PhaseStart > 60.0) { Fail(FString::Printf(TEXT("%s did not load within 60 s"), *ReturnPackage)); }
		break;
	case EPhase::Collecting:
		// two full collections, a tick apart, so anything only pending destruction after the travel is gone
		if (IsAsyncLoading()) { break; }
		if (CollectTicks < 2)
		{
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
			++CollectTicks;
			break;
		}
		Report();
		Finish();
		return false;
	case EPhase::Done:
		return false;
	}
	return Phase != EPhase::Done;
}

void FLevelLeakTest::Report()
{
	UWorld* Current = GameWorld();
	UPackage* CurrentPackage = Current ? Current->GetOutermost() : nullptr;
	TMap<UObject*, FString> Survivors;
	int32 After = 0;
	for (FThreadSafeObjectIterator It; It; ++It)
	{
		UObject* O = *It;
		++After;
		if (O->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) { continue; }
		const FObjectKey Key(O);
		if (LevelObjects.Contains(Key))
		{
			Survivors.Add(O, FString::Printf(TEXT("it belonged to %s, the map that was left"), *TestPackage));
			continue;
		}
		if (Baseline.Contains(Key)) { continue; }
		if (Current && (O == Current || O->GetTypedOuter<UWorld>() == Current || O->GetOutermost() == CurrentPackage)) { continue; }
		if ((O->IsA<AActor>() || O->IsA<UActorComponent>()) && !O->IsTemplate() && O->GetWorld() != Current)
		{
			Survivors.Add(O, TEXT("an actor or component made during the level, and in no live world now"));
			continue;
		}
		const FString ClassPackage = O->GetClass()->GetOutermost()->GetName();
		if (ProjectScripts.Contains(ClassPackage) || ClassPackage.StartsWith(TEXT("/Game/")))
		{
			Survivors.Add(O, FString::Printf(TEXT("an object of the project's own class %s made during the level"), *O->GetClass()->GetName()));
		}
	}

	// group inner objects under the survivor that owns them: fix the owner, the inners go with it
	TArray<UObject*> Tops;
	TMap<UObject*, TMap<FString, int32>> Inners;
	for (const TPair<UObject*, FString>& S : Survivors)
	{
		UObject* Top = nullptr;
		for (UObject* Outer = S.Key->GetOuter(); Outer; Outer = Outer->GetOuter())
		{
			if (Survivors.Contains(Outer)) { Top = Outer; }
		}
		if (Top) { Inners.FindOrAdd(Top).FindOrAdd(S.Key->GetClass()->GetName())++; }
		else { Tops.Add(S.Key); }
	}
	Tops.Sort([](const UObject& A, const UObject& B) { return A.GetPathName() < B.GetPathName(); });

	TArray<UObject*> Search;
	for (int32 I = 0; I < Tops.Num() && I < Options.MaxChains; ++I) { Search.Add(Tops[I]); }
	TMap<FString, TArray<TSharedPtr<FJsonValue>>> Chains;
	TMap<FString, FString> Holders;
	if (Search.Num())
	{
		// Shortest only. ExternalOnly would be the natural flag, but in 5.7 its non-direct branch tests every chain with a
		// placeholder that always answers false and removes them all (ReferenceChainSearch.cpp, PerformSearch).
		FReferenceChainSearch Refs(Search, EReferenceChainSearchMode::Shortest);
		for (const FReferenceChainSearch::FReferenceChain* Chain : Refs.GetReferenceChains())
		{
			if (!Chain || Chain->Num() == 0) { continue; }
			const FString TargetPath = Chain->GetNode(0)->ObjectInfo->GetPathName();
			if (Chains.Contains(TargetPath)) { continue; }
			TArray<TSharedPtr<FJsonValue>> Links;
			for (int32 N = Chain->Num() - 1; N >= 0; --N)
			{
				FGCObjectInfo* Info = Chain->GetNode(N)->ObjectInfo;
				TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
				L->SetStringField(TEXT("object"), Info->GetPathName());
				L->SetStringField(TEXT("class"), Info->GetClassName());
				L->SetBoolField(TEXT("rooted"), Info->IsRooted());
				// how this node is reached from the one before it (the root has none)
				L->SetStringField(TEXT("via"), N == Chain->Num() - 1 ? FString(Info->IsRooted() ? TEXT("root set") : TEXT("root"))
					: Via(Chain->GetReferenceInfo(N + 1)));
				Links.Add(MakeShared<FJsonValueObject>(L));
			}
			if (Chain->Num() >= 2)
			{
				const FString Holder = Chain->GetNode(1)->ObjectInfo->GetClassName();
				Holders.Add(TargetPath, FString::Printf(TEXT("%s, through %s"), *Holder, *Via(Chain->GetReferenceInfo(1))));
			}
			Chains.Add(TargetPath, MoveTemp(Links));
		}
	}

	FSentinelFindingsFile File;
	File.Kind = TEXT("leaktest");
	File.Target->SetStringField(TEXT("project"), FApp::GetProjectName());
	File.Target->SetStringField(TEXT("map"), TestPackage);
	File.Target->SetStringField(TEXT("return_map"), ReturnPackage);
	File.Checked->SetNumberField(TEXT("objects_before"), Baseline.Num());
	File.Checked->SetNumberField(TEXT("level_objects"), LevelObjectCount);
	File.Checked->SetNumberField(TEXT("objects_after"), After);
	File.Checked->SetNumberField(TEXT("survivors"), Survivors.Num());
	File.Checked->SetNumberField(TEXT("top_survivors"), Tops.Num());
	File.Checked->SetNumberField(TEXT("chains_searched"), Search.Num());
	File.Checked->SetNumberField(TEXT("chains_found"), Chains.Num());
	File.Checked->SetNumberField(TEXT("play_seconds"), Options.PlaySeconds);
	File.Checked->SetNumberField(TEXT("gc_passes"), 2);
	for (const TCHAR* B : BlindSpots) { File.BlindSpots.Add(B); }

	for (UObject* Top : Tops)
	{
		const FString Path = Top->GetPathName();
		FSentinelFinding F;
		F.Rule = TEXT("level-survivor");
		F.Severity = TEXT("error");
		F.Title = FString::Printf(TEXT("%s %s survived the level unload"), *Top->GetClass()->GetName(), *Top->GetName());
		F.Where->SetStringField(TEXT("object"), Path);
		F.Where->SetStringField(TEXT("class"), Top->GetClass()->GetName());
		const TMap<FString, int32>* In = Inners.Find(Top);
		int32 NumInner = 0;
		TSharedRef<FJsonObject> InnerClasses = MakeShared<FJsonObject>();
		if (In)
		{
			for (const TPair<FString, int32>& P : *In) { NumInner += P.Value; InnerClasses->SetNumberField(P.Key, P.Value); }
		}
		F.Why = FString::Printf(TEXT("%s was left and garbage was collected twice, but this object is still alive: %s.%s"),
			*TestPackage, *Survivors[Top], NumInner ? *FString::Printf(TEXT(" %d objects inside it stay alive with it."), NumInner) : TEXT(""));
		const TArray<TSharedPtr<FJsonValue>>* Chain = Chains.Find(Path);
		if (const FString* Holder = Holders.Find(Path))
		{
			F.Fix = FString::Printf(TEXT("%s still holds it. Let go of it when the level ends (EndPlay, or FWorldDelegates::OnWorldCleanup), or hold it with a TWeakObjectPtr."), **Holder);
			F.HowKnown = TEXT("READ: alive after two full garbage collections; FReferenceChainSearch (the search behind obj refs) gave the chain from its root");
		}
		else if (Top->IsRooted())
		{
			F.Fix = TEXT("It is in the root set (AddToRoot or RF_MarkAsRootSet). Call RemoveFromRoot when the level ends.");
			F.HowKnown = TEXT("READ: alive after two full garbage collections; it is in the root set, so no chain leads to it");
		}
		else
		{
			F.Fix = TEXT("Run `obj refs name=<path>` on it while the game runs: something outside the reflected graph keeps it.");
			F.HowKnown = Search.Contains(Top) ? TEXT("READ: alive after two full garbage collections; the reference search found no chain")
				: TEXT("READ: alive after two full garbage collections; not searched (more survivors than the chain limit)");
		}
		F.Evidence->SetStringField(TEXT("object"), Path);
		F.Evidence->SetStringField(TEXT("class"), Top->GetClass()->GetName());
		F.Evidence->SetStringField(TEXT("reason"), Survivors[Top]);
		F.Evidence->SetNumberField(TEXT("inner_objects"), NumInner);
		F.Evidence->SetObjectField(TEXT("inner_classes"), InnerClasses);
		F.Evidence->SetBoolField(TEXT("rooted"), Top->IsRooted());
		F.Evidence->SetBoolField(TEXT("chain_found"), Chain != nullptr);
		F.Evidence->SetArrayField(TEXT("chain"), Chain ? *Chain : TArray<TSharedPtr<FJsonValue>>());
		File.Findings.Add(F);
	}

	const FString Short = FPackageName::GetShortName(TestPackage);
	WrittenFile = MemorySentinelFiles::Dir() / FString::Printf(TEXT("leaktest-%s-%s.json"), *Short, *MemorySentinelFiles::Stamp());
	File.Write(WrittenFile, TEXT("LEAK"));
	Last = WrittenFile;
	UE_LOG(LogMemorySentinelLeak, Display, TEXT("Memory Sentinel leak test: %d objects before, %d in the level, %d after; %d survived (%d owners), %d chains. Wrote %s"),
		Baseline.Num(), LevelObjectCount, After, Survivors.Num(), Tops.Num(), Chains.Num(), *WrittenFile);
	for (const FSentinelFinding& F : File.Findings) { UE_LOG(LogMemorySentinelLeak, Display, TEXT("  survivor: %s"), *F.Title); }
	if (Options.bQuit) { FPlatformMisc::RequestExit(false, TEXT("MemorySentinel leak test")); }
}

void FLevelLeakTest::Fail(const FString& Why)
{
	if (Phase == EPhase::Done) { return; }
	UE_LOG(LogMemorySentinelLeak, Error, TEXT("Memory Sentinel leak test FAILED: %s"), *Why);
	FSentinelFindingsFile File;
	File.Kind = TEXT("leaktest");
	File.bOk = false;
	File.Failure = Why;
	File.Target->SetStringField(TEXT("map"), TestPackage);
	File.Checked->SetNumberField(TEXT("objects_before"), Baseline.Num());
	for (const TCHAR* B : BlindSpots) { File.BlindSpots.Add(B); }
	WrittenFile = MemorySentinelFiles::Dir() / FString::Printf(TEXT("leaktest-%s-%s.json"), *FPackageName::GetShortName(TestPackage), *MemorySentinelFiles::Stamp());
	File.Write(WrittenFile, TEXT("LEAK"));
	Last = WrittenFile;
	if (Options.bQuit) { FPlatformMisc::RequestExit(false, TEXT("MemorySentinel leak test")); }
	Finish();
}

void FLevelLeakTest::Finish()
{
	Phase = EPhase::Done;
	Cleanup();
	Active.Reset();
}

void FLevelLeakTest::Cleanup()
{
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(LoadedHandle);
	if (GEngine) { GEngine->OnTravelFailure().Remove(TravelHandle); }
	if (TickHandle.IsValid() && !bInTick) { FTSTicker::GetCoreTicker().RemoveTicker(TickHandle); }
	TickHandle.Reset();
}
