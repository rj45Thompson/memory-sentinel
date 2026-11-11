// THE CLEAN CONTROL. The same patterns as Planted/, written the way Unreal wants them. The static pass must report nothing
// in this folder: a finding here is a false alarm (tests/test_scanner.py).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/Texture2D.h"
#include "UObject/GCObject.h"
#include "Templates/SharedPointer.h"
#include "Inventory.generated.h"

UCLASS()
class UInventory : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Items")
	TArray<TObjectPtr<UObject>> Items;

	UPROPERTY()
	TMap<FName, TObjectPtr<UTexture2D>> Icons;

	UPROPERTY()
	TSubclassOf<AActor> SpawnClass;

	TWeakObjectPtr<AActor> LastUser;
	TSoftObjectPtr<UTexture2D> Lazy;
	FString Label;
	int32 Count = 0;
	FTimerHandle Timer;

	void Use(AActor* User);
	UObject* FindItem(FName Name) const;
	UFUNCTION(BlueprintCallable, Category = "Items")
	void Clear();
	void Later();
	void* Scratch = nullptr;
};

USTRUCT()
struct FSlot
{
	GENERATED_BODY()
	UPROPERTY()
	TObjectPtr<UObject> Item;
	UPROPERTY()
	int32 Stack = 0;
};

class FWatcher : public FGCObject
{
public:
	TObjectPtr<UObject> Watched;
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override { Collector.AddReferencedObject(Watched); }
	virtual FString GetReferencerName() const override { return TEXT("FWatcher"); }
};

struct FInventoryLeaf;

struct FInventoryTree
{
	TArray<TSharedPtr<FInventoryLeaf>> Leaves;
};

struct FInventoryLeaf
{
	TWeakPtr<FInventoryTree> Tree;
	int32 Value = 0;
};

class FSaver : public TSharedFromThis<FSaver>
{
public:
	FSimpleDelegate OnSaved;
	void Start()
	{
		OnSaved.BindSP(this, &FSaver::Done);
		OnSaved.BindLambda([Weak = AsWeak()]() { if (TSharedPtr<FSaver> Self = Weak.Pin()) { Self->Done(); } });
	}
	void Done() {}
};
