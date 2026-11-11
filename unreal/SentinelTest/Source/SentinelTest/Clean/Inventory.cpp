// THE CLEAN CONTROL (see Inventory.h).
#include "Inventory.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "TimerManager.h"

void UInventory::Use(AActor* User)
{
	LastUser = User;
	UObject* Local = FindItem(NAME_None);
	if (Local)
	{
		Items.Add(Local);
	}
}

UObject* UInventory::FindItem(FName Name) const
{
	return Items.Num() ? Items[0].Get() : nullptr;
}

void UInventory::Clear()
{
	Items.Reset();
}

void UInventory::Later()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateUObject(this, &UInventory::Clear), 1.0f, false);
		World->GetTimerManager().SetTimer(Timer, FTimerDelegate::CreateWeakLambda(this, [this]() { Clear(); }), 1.0f, false);
	}
	TArray<int32> Numbers = { 3, 1, 2 };
	Numbers.Sort([this](int32 A, int32 B) { return A + Count < B + Count; });
	AsyncTask(ENamedThreads::GameThread, [Weak = TWeakObjectPtr<UInventory>(this)]()
	{
		if (UInventory* Self = Weak.Get())
		{
			Self->Clear();
		}
	});
	const int32 Total = Count;
	AsyncTask(ENamedThreads::AnyThread, [Total]() { UE_LOG(LogTemp, Verbose, TEXT("total %d"), Total); });
	Scratch = &Count;
}
