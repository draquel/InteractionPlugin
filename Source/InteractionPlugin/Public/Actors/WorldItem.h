#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/StreamableManager.h"
#include "Types/CGFItemTypes.h"
#include "WorldItem.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UInteractableComponent;
class UItemDatabaseSubsystem;
class UItemDefinition;

/**
 * Where a world item rests, as the server settled it. It replicates, and a client places its copy by it: the item's
 * movement is not replicated, so without it a client kept the place and the pose its copy first arrived with.
 */
USTRUCT(BlueprintType)
struct FWorldItemRest
{
	GENERATED_BODY()

	/** The server has placed this item (false while it waits in the pool) */
	UPROPERTY(BlueprintReadOnly, Category = "WorldItem")
	bool bValid = false;

	/** It rests at a water surface (it bobs there on each machine) rather than on the ground */
	UPROPERTY(BlueprintReadOnly, Category = "WorldItem")
	bool bFloating = false;

	/** The actor's resting location */
	UPROPERTY(BlueprintReadOnly, Category = "WorldItem")
	FVector Location = FVector::ZeroVector;

	/** The actor's resting rotation (the resting pose applied) */
	UPROPERTY(BlueprintReadOnly, Category = "WorldItem")
	FRotator Rotation = FRotator::ZeroRotator;

	/** The up the item rests along: world +Z, the direction from the gravity centre, or the water surface's up */
	UPROPERTY(BlueprintReadOnly, Category = "WorldItem")
	FVector_NetQuantizeNormal Up = FVector::UpVector;
};

/**
 * Poolable world item actor. Represents a dropped item in the world.
 * Async-loads its mesh, provides pickup interaction, integrates with the object pool.
 *
 * The server places it: on the ground under it, or at the water's surface when the world's water
 * (UCGFWaterSubsystem, registered by the game layer) says the drop is in or just over water. The place and the pose
 * replicate as Rest; the item itself replicates as ItemInstance, from which each client builds the mesh it shows.
 */
UCLASS(BlueprintType)
class INTERACTIONPLUGIN_API AWorldItem : public AActor
{
	GENERATED_BODY()

public:
	AWorldItem();

	// -----------------------------------------------------------------------
	// Lifecycle
	// -----------------------------------------------------------------------

	/** Initialize this world item from an item instance. Async-loads mesh. */
	UFUNCTION(BlueprintCallable, Category = "WorldItem")
	void InitializeFromItem(const FItemInstance& Item);

	/** Where a pooled item waits unless its pool says otherwise (UWorldItemPoolSubsystem::ParkingLocation) */
	static const FVector DefaultParkingLocation;

	/**
	 * Reset state for return to pool: hidden, no collision, no physics, parked at ParkingLocation.
	 * @param ParkingLocation Where to wait (the pool's parking spot)
	 */
	void ResetForPool(const FVector& ParkingLocation = DefaultParkingLocation);

	/**
	 * Rest the item on what is below it. In or just over water (UCGFWaterSubsystem; bFloatOnWater) it rests at the
	 * water's surface, straight over where it is; otherwise it traces down a short way and moves so the mesh's bounds
	 * sit on the hit. Authority only: the place reaches clients as Rest. Runs at spawn, again once the real mesh has
	 * loaded (its bounds differ from the placeholder's), and when the water under an item changes.
	 * @return True if a surface (water or ground) was found and the item moved onto it
	 */
	UFUNCTION(BlueprintCallable, Category = "WorldItem")
	bool SnapToGround();

	/** Settle onto the ground automatically at spawn and after the mesh loads (drops used to hover ~1 m up). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem")
	bool bSnapToGround = true;

	/** Rest at a water surface instead of the bed under it when settling in or just over water. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating")
	bool bFloatOnWater = true;

	/** Water shallower than this over the ground does not float an item: it rests on the ground (world units). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating")
	float MinFloatWaterDepth = 20.0f;

	/** The part of a floating item's height that lies under the surface (0 = sits on the water, 1 = just under it). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FloatSubmersion = 0.6f;

	/** How far a floating item rises and falls about its resting place (world units; 0 = still). Drawn per machine. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating")
	float FloatBobAmplitude = 2.5f;

	/** Seconds per rise and fall of a floating item. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating", meta = (ClampMin = "0.1"))
	float FloatBobPeriod = 2.8f;

	/** How far a floating item rocks about its resting pose (degrees; 0 = level). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating")
	float FloatRockDegrees = 3.0f;

	/**
	 * A floating item's mesh draws custom depth, so a water material can tell the item from the bed under the water
	 * (one that shades by the water's depth, or draws shore foam where the water is thin, takes an item lying just
	 * under the surface for a shallow bed otherwise). Off again when the item no longer floats.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem|Floating")
	bool bCustomDepthWhileFloating = true;

	/** @return True while the item rests at a water surface */
	UFUNCTION(BlueprintPure, Category = "WorldItem|Floating")
	bool IsFloating() const { return Rest.bValid && Rest.bFloating; }

	/**
	 * A floating item's place at a time: its resting place with the bob (along the rest's up) and the rock applied.
	 * The same function on every machine, each with its own clock: the bob is drawn, not replicated.
	 * @param TimeSeconds The world's time
	 * @param OutLocation The actor's location then (Rest.Location when the item does not float)
	 * @param OutRotation The actor's rotation then (Rest.Rotation when the item does not float)
	 */
	void GetFloatingPose(double TimeSeconds, FVector& OutLocation, FQuat& OutRotation) const;

	/**
	 * Snap along the direction from a gravity centre instead of world Z (a planet; the pool passes its own,
	 * UWorldItemPoolSubsystem::SetGravityCenter).
	 * @param bEnabled Use the centre
	 * @param Center The gravity centre
	 */
	void SetGravityCenter(bool bEnabled, const FVector& Center) { bHasGravityCenter = bEnabled; GravityCenter = Center; }

	/**
	 * Turn the item into a resting pose (the placeholder's zero, or the WorldDisplay fragment's lying rotation). The mesh is
	 * the root, so its relative rotation is the actor's: with a gravity centre the pose is applied on top of the upright
	 * rotation the item was spawned with (InitializeFromItem keeps it), so it lies on the planet's ground; without one it
	 * is the world rotation, as before.
	 * @param Pose The pose, relative to the upright frame (world axes without a gravity centre)
	 */
	void ApplyRestingPose(const FRotator& Pose);

	/** The item this actor represents. A client builds what it shows from it (OnRep_ItemInstance). */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ItemInstance, Category = "WorldItem")
	FItemInstance ItemInstance;

	/** Where the server rested the item; a client places its copy there (OnRep_Rest). */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Rest, Category = "WorldItem")
	FWorldItemRest Rest;

	// -----------------------------------------------------------------------
	// Components
	// -----------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WorldItem")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	/** SnapToGround's gravity centre (SetGravityCenter). */
	bool bHasGravityCenter = false;
	FVector GravityCenter = FVector::ZeroVector;

	/** With a gravity centre: the upright rotation the item was initialised with, the frame of ApplyRestingPose. */
	FQuat UprightRotation = FQuat::Identity;

	/**
	 * Authority: the rotation the item rests with (the one it was initialised with, then each ApplyRestingPose). A
	 * floating item is drawn rocking about it, so a new settle starts from this and never from the drawn rotation.
	 */
	FQuat PoseRotation = FQuat::Identity;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WorldItem")
	TObjectPtr<UInteractableComponent> InteractableComponent;

	/** Placeholder mesh shown until (or instead of) the definition's WorldDisplay mesh — a
	 *  definition without a world mesh must still produce a findable pickup. */
	UPROPERTY()
	TObjectPtr<UStaticMesh> FallbackMesh;

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Client: the item arrived, changed or went back to the pool; build or hide what this copy shows. */
	UFUNCTION()
	void OnRep_ItemInstance();

	/** Client: the server placed the item; move this copy there and start or stop its bob. */
	UFUNCTION()
	void OnRep_Rest();

private:
	/**
	 * Show the item: the placeholder at once, then the definition's world mesh when it has loaded. Every machine
	 * runs it (the server from InitializeFromItem, a client from OnRep_ItemInstance).
	 * @return The item's definition, or null when the database does not know it
	 */
	UItemDefinition* ShowItem();

	/** Hide the item and cancel its mesh load (the pooled state's looks; every machine). */
	void HideItem();

	/** Authority: record where the item now rests (its transform, whether it floats, the up) for the clients. */
	void PublishRest(bool bFloating, const FVector& Up);

	/**
	 * What a floating item needs on a machine with a view: the tick that draws its bob, and custom depth on its mesh.
	 * Both off for an item that does not float, and on a dedicated server.
	 */
	void UpdateFloatTick();

	/** The up an item settles along where there is no water: the direction from the gravity centre, or world +Z. */
	FVector GetSettleUp() const;

	/** The mesh bounds' lowest and highest points along a direction, relative to the actor's location. */
	void GetExtentAlong(const FVector& Up, double& OutLowest, double& OutHighest) const;

	/** Async mesh loading handle */
	TSharedPtr<FStreamableHandle> MeshLoadHandle;

	/** Called when the async mesh load completes */
	void OnMeshLoaded();

	/** Handle a pickup interaction */
	EInteractionResult OnPickupInteraction(AActor* Interactor, FGameplayTag InteractionType);

	UItemDatabaseSubsystem* GetItemDatabase() const;
};
