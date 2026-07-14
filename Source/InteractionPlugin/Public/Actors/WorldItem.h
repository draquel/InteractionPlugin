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
