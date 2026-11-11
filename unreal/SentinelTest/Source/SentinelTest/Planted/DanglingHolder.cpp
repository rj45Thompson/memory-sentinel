// PLANTED BUGS (see DanglingHolder.h). Also the runtime half of the dangling pointer: BeginPlay makes an object that only
// the unseen raw pointer Target holds, so the next garbage collection deletes it and Target dangles.
#include "DanglingHolder.h"
#include "Engine/World.h"
#include "TimerManager.h"

UObject* ADanglingHolder::LastHolder = nullptr;

ADanglingHolder::ADanglingHolder()
{
	PrimaryActorTick.bCanEverTick = false;
}

void ADanglingHolder::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	ADanglingHolder* This = CastChecked<ADanglingHolder>(InThis);
	Collector.AddReferencedObject(This->Reported);
	Super::AddReferencedObjects(InThis, Collector);
}

void ADanglingHolder::BeginPlay()
{
	Super::BeginPlay();
	Target = NewObject<UObject>(this);
	UserData = this;                                // PLANTED: void-hides-uobject
	StartTimers();
	Hide();
}

void ADanglingHolder::StartTimers()
{
	GetWorldTimerManager().SetTimer(Timer, [this]() { Target = nullptr; }, 1.0f, false);   // PLANTED: lambda-captures-uobject
	GetWorldTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(this, [this]() { Target = nullptr; }), 1.0f, false);
}

void ADanglingHolder::Hide()
{
	const void* Key = static_cast<const void*>(Target);   // PLANTED: void-hides-uobject
	UE_LOG(LogTemp, Verbose, TEXT("hidden key %p"), Key);
}
