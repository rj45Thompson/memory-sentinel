// PLANTED BUGS. Every line marked "PLANTED: <rule>" is a bug the static pass must report with that rule, at that line, once.
// Nothing else in this file may be reported. tests/test_scanner.py holds the scan to that, both ways.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/Texture2D.h"
#include "DanglingHolder.generated.h"

UCLASS()
class ADanglingHolder : public AActor
{
	GENERATED_BODY()
public:
	ADanglingHolder();
	virtual void BeginPlay() override;
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	UObject* Target = nullptr;                      // PLANTED: uobject-member-unseen
	TObjectPtr<UTexture2D> Icon;                    // PLANTED: uobject-member-unseen
	TArray<UObject*> Spawned;                       // PLANTED: uobject-member-unseen
	TMap<FName, TObjectPtr<UObject>> ByName;        // PLANTED: uobject-member-unseen
	TSubclassOf<AActor> SpawnClass;                 // PLANTED: subclassof-unseen
	static UObject* LastHolder;                     // PLANTED: static-uobject-pointer

	UPROPERTY()
	TObjectPtr<UObject> Seen;                       // fine: UPROPERTY on the line above
	UPROPERTY(EditAnywhere, Category = "Test") TArray<TObjectPtr<UObject>> SeenList;
	TWeakObjectPtr<UObject> Weak;                   // fine: a weak pointer
	TObjectPtr<UObject> Reported;                   // fine: AddReferencedObjects reports it
	UObject* Excused = nullptr;                     // memsentinel: ignore owned and kept alive by the subsystem

	void* UserData = nullptr;
	FTimerHandle Timer;

	void StartTimers();
	void Hide();
};
