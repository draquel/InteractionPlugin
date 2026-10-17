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

	/**
	 * Where pooled items wait while hidden (no collision, no physics). The default is far under a flat world's ground; a
	 * world whose ground passes there (a planet's south pole, VoxelWorlds SPHERICAL_PLANET_REQUIREMENTS.md SP-F5) moves
	 * it with SetParkingLocation.
	 */
	UPROPERTY(EditAnywhere, Category = "Pool")
	FVector ParkingLocation = FVector(0.0, 0.0, -100000.0);

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

	/**
	 * Move the parking spot (ParkingLocation) and every item waiting in the pool to it; returned items park there from
	 * now on. Game code calls it for worlds whose ground passes through the default spot.
	 * @param NewLocation Where pooled items wait
	 */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	void SetParkingLocation(const FVector& NewLocation);

	/** @return Where pooled items wait */
	UFUNCTION(BlueprintPure, Category = "WorldItemPool")
	FVector GetParkingLocation() const { return ParkingLocation; }

	/**
	 * Give spawned items a gravity centre (a planet: VoxelWorlds SPHERICAL_PLANET_REQUIREMENTS.md SP-F7, spherical Phase 4):
	 * they then stand upright on, and snap to the ground along, the direction from it instead of world Z. Game code sets
	 * it with the parking spot.
	 * @param Center The gravity centre
	 */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	void SetGravityCenter(const FVector& Center);

	/** Back to world Z for items spawned from now on. */
	UFUNCTION(BlueprintCallable, Category = "WorldItemPool")
	void ClearGravityCenter();

	UFUNCTION(BlueprintPure, Category = "WorldItemPool")
	bool HasGravityCenter() const { return bHasGravityCenter; }

	UFUNCTION(BlueprintPure, Category = "WorldItemPool")
	FVector GetGravityCenter() const { return GravityCenter; }

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

	/** The gravity centre spawned items use (SetGravityCenter). */
	bool bHasGravityCenter = false;
	FVector GravityCenter = FVector::ZeroVector;

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
