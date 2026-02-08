#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WorldItemPoolSubsystem.generated.h"

class AWorldItem;
struct FItemInstance;

/**
 * World subsystem that manages an object pool of AWorldItem actors.
 * Pre-warms on world begin, expands on demand, and handles despawn timeouts.
 */
UCLASS()
class INTERACTIONPLUGIN_API UWorldItemPoolSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// -----------------------------------------------------------------------
	// Configuration
	// -----------------------------------------------------------------------

	/** Number of AWorldItem actors to pre-spawn on world begin */
	UPROPERTY(EditAnywhere, Category = "Pool")
	int32 InitialPoolSize = 50;

	/** Hard cap on total pool size (active + inactive) */
	UPROPERTY(EditAnywhere, Category = "Pool")
	int32 MaxPoolSize = 200;

	/** Seconds before an uncollected active item despawns (0 = never) */
	UPROPERTY(EditAnywhere, Category = "Pool")
	float DespawnTimeout = 300.0f;

	/** If true, spawn new actors when pool is empty (up to MaxPoolSize) */
	UPROPERTY(EditAnywhere, Category = "Pool")
	bool bExpandPoolOnDemand = true;

	/** Items to spawn per frame during pre-warming (avoids hitch) */
	UPROPERTY(EditAnywhere, Category = "Pool")
	int32 PreWarmBatchSize = 10;

	// -----------------------------------------------------------------------
	// API
	// -----------------------------------------------------------------------

	/** Get an inactive world item from the pool (or spawn new if allowed). Returns nullptr if exhausted. */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	AWorldItem* GetWorldItem();

	/** Spawn a world item initialized with the given item at a location */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	AWorldItem* SpawnWorldItem(const FItemInstance& Item, const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator);

	/** Return a world item to the pool */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	void ReturnWorldItem(AWorldItem* Item);

	/** Return all active world items to the pool */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	void ReturnAllWorldItems();

	/** Current counts */
	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "WorldItemPool")
	int32 GetAvailableCount() const { return AvailablePool.Num(); }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "WorldItemPool")
	int32 GetActiveCount() const { return ActiveItems.Num(); }

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "WorldItemPool")
	int32 GetTotalCount() const { return AvailablePool.Num() + ActiveItems.Num(); }

protected:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;

private:
	UPROPERTY()
	TArray<TObjectPtr<AWorldItem>> AvailablePool;

	UPROPERTY()
	TArray<TObjectPtr<AWorldItem>> ActiveItems;

	/** Spawn a single pooled actor and reset it */
	AWorldItem* SpawnPooledActor();

	/** Pre-warm timer for batched spawning */
	FTimerHandle PreWarmTimerHandle;
	int32 PreWarmRemaining = 0;
	void TickPreWarm();

	/** Despawn timer management */
	TMap<AWorldItem*, FTimerHandle> DespawnTimers;
	void StartDespawnTimer(AWorldItem* Item);
	void ClearDespawnTimer(AWorldItem* Item);
	void OnDespawnTimeout(AWorldItem* Item);
};
