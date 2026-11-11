// PLANTED BUGS: UObject pointers in plain C++ types, and an FGCObject that forgets one member.
#pragma once

#include "CoreMinimal.h"
#include "Tickable.h"
#include "UObject/GCObject.h"

class UTexture2D;

struct FIconCache
{
	UTexture2D* Icon = nullptr;                     // PLANTED: uobject-in-plain-type
	TArray<UObject*> Recent;                        // PLANTED: uobject-in-plain-type
	int32 Hits = 0;
};

class FIconTicker : public FTickableGameObject
{
public:
	UObject* Watched = nullptr;                     // PLANTED: uobject-in-plain-type
	virtual void Tick(float DeltaTime) override {}
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FIconTicker, STATGROUP_Tickables); }
};

class FHalfReported : public FGCObject
{
public:
	TObjectPtr<UObject> Kept;
	TObjectPtr<UObject> Forgotten;                  // PLANTED: fgcobject-missing-member
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObject(Kept); }
	virtual FString GetReferencerName() const override { return TEXT("FHalfReported"); }
};
