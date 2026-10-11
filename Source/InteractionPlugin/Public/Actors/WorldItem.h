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

/**
 * Poolable world item actor. Represents a dropped item in the world.
 * Async-loads its mesh, provides pickup interaction, integrates with the object pool.
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

	/** Reset state for return to pool */
	void ResetForPool();

	/**
	 * Rest the item on the surface below it: trace down a short way and move the actor so the mesh's bounds sit on
	 * the hit. Authority only (the location replicates). Runs at spawn and again once the real mesh has loaded,
	 * because its bounds differ from the placeholder's.
	 * @return True if a surface was found and the item moved onto it
	 */
	UFUNCTION(BlueprintCallable, Category = "WorldItem")
	bool SnapToGround();

	/** Settle onto the ground automatically at spawn and after the mesh loads (drops used to hover ~1 m up). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "WorldItem")
	bool bSnapToGround = true;

	/** The item this actor represents */
	UPROPERTY(BlueprintReadOnly, Replicated, Category = "WorldItem")
	FItemInstance ItemInstance;

	// -----------------------------------------------------------------------
	// Components
	// -----------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WorldItem")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "WorldItem")
	TObjectPtr<UInteractableComponent> InteractableComponent;

	/** Placeholder mesh shown until (or instead of) the definition's WorldDisplay mesh — a
	 *  definition without a world mesh must still produce a findable pickup. */
	UPROPERTY()
	TObjectPtr<UStaticMesh> FallbackMesh;

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	/** Async mesh loading handle */
	TSharedPtr<FStreamableHandle> MeshLoadHandle;

	/** Called when the async mesh load completes */
	void OnMeshLoaded();

	/** Handle a pickup interaction */
	EInteractionResult OnPickupInteraction(AActor* Interactor, FGameplayTag InteractionType);

	UItemDatabaseSubsystem* GetItemDatabase() const;
};
