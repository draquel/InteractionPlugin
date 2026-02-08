#include "Subsystems/WorldItemPoolSubsystem.h"
#include "Actors/WorldItem.h"
#include "TimerManager.h"
#include "Engine/World.h"

void UWorldItemPoolSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	// Batched pre-warming to avoid hitch
	PreWarmRemaining = InitialPoolSize;
	if (PreWarmRemaining > 0)
	{
		InWorld.GetTimerManager().SetTimer(
			PreWarmTimerHandle, this, &UWorldItemPoolSubsystem::TickPreWarm,
			0.0f, true); // Every frame
	}
}

void UWorldItemPoolSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(PreWarmTimerHandle);

		// Clear all despawn timers
		for (auto& Pair : DespawnTimers)
		{
			World->GetTimerManager().ClearTimer(Pair.Value);
		}
	}
	DespawnTimers.Empty();

	Super::Deinitialize();
}

void UWorldItemPoolSubsystem::TickPreWarm()
{
	int32 ToSpawn = FMath::Min(PreWarmBatchSize, PreWarmRemaining);
	for (int32 i = 0; i < ToSpawn; ++i)
	{
		AWorldItem* Item = SpawnPooledActor();
		if (Item)
		{
			AvailablePool.Add(Item);
		}
	}

	PreWarmRemaining -= ToSpawn;
	if (PreWarmRemaining <= 0)
	{
		GetWorld()->GetTimerManager().ClearTimer(PreWarmTimerHandle);
	}
}

// ===========================================================================
// Pool API
// ===========================================================================

AWorldItem* UWorldItemPoolSubsystem::GetWorldItem()
{
	AWorldItem* Item = nullptr;

	if (AvailablePool.Num() > 0)
	{
		Item = AvailablePool.Pop();
	}
	else if (bExpandPoolOnDemand && GetTotalCount() < MaxPoolSize)
	{
		Item = SpawnPooledActor();
	}

	if (Item)
	{
		ActiveItems.Add(Item);

		if (DespawnTimeout > 0.0f)
		{
			StartDespawnTimer(Item);
		}
	}

	return Item;
}

AWorldItem* UWorldItemPoolSubsystem::SpawnWorldItem(const FItemInstance& Item, const FVector& Location,
	const FRotator& Rotation)
{
	AWorldItem* WorldItem = GetWorldItem();
	if (!WorldItem)
	{
		return nullptr;
	}

	WorldItem->SetActorLocationAndRotation(Location, Rotation);
	WorldItem->InitializeFromItem(Item);
	return WorldItem;
}

void UWorldItemPoolSubsystem::ReturnWorldItem(AWorldItem* Item)
{
	if (!Item)
	{
		return;
	}

	ClearDespawnTimer(Item);
	ActiveItems.Remove(Item);

	Item->ResetForPool();
	AvailablePool.Add(Item);
}

void UWorldItemPoolSubsystem::ReturnAllWorldItems()
{
	TArray<TObjectPtr<AWorldItem>> ItemsCopy = ActiveItems;
	for (AWorldItem* Item : ItemsCopy)
	{
		ReturnWorldItem(Item);
	}
}

// ===========================================================================
// Spawning
// ===========================================================================

AWorldItem* UWorldItemPoolSubsystem::SpawnPooledActor()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AWorldItem* Item = World->SpawnActor<AWorldItem>(AWorldItem::StaticClass(),
		FVector(0.0f, 0.0f, -100000.0f), FRotator::ZeroRotator, SpawnParams);

	if (Item)
	{
		Item->ResetForPool();
	}

	return Item;
}

// ===========================================================================
// Despawn Timers
// ===========================================================================

void UWorldItemPoolSubsystem::StartDespawnTimer(AWorldItem* Item)
{
	if (!Item || DespawnTimeout <= 0.0f)
	{
		return;
	}

	ClearDespawnTimer(Item);

	FTimerHandle& Handle = DespawnTimers.Add(Item);
	GetWorld()->GetTimerManager().SetTimer(
		Handle,
		FTimerDelegate::CreateUObject(this, &UWorldItemPoolSubsystem::OnDespawnTimeout, Item),
		DespawnTimeout, false);
}

void UWorldItemPoolSubsystem::ClearDespawnTimer(AWorldItem* Item)
{
	if (FTimerHandle* Handle = DespawnTimers.Find(Item))
	{
		GetWorld()->GetTimerManager().ClearTimer(*Handle);
		DespawnTimers.Remove(Item);
	}
}

void UWorldItemPoolSubsystem::OnDespawnTimeout(AWorldItem* Item)
{
	if (Item && ActiveItems.Contains(Item))
	{
		ReturnWorldItem(Item);
	}
}
