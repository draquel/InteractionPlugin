#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Types/CGFInteractionTypes.h"
#include "Types/CGFCommonEnums.h"
#include "Interfaces/CGFInteractableInterface.h"
#include "InteractableComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnInteractableStatusChanged, bool, bEnabled);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnInteractionTriggered, AActor*, Interactor, FGameplayTag, InteractionType);

/**
 * Makes the owning actor interactable by the interaction system.
 * Provides interaction options, enable/disable control, and fires
 * delegates when interactions occur.
 */
UCLASS(BlueprintType, ClassGroup = "Interaction", meta = (BlueprintSpawnableComponent))
class INTERACTIONPLUGIN_API UInteractableComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInteractableComponent();

	// -----------------------------------------------------------------------
	// Configuration
	// -----------------------------------------------------------------------

	/** Whether this interactable is active and visible to detection */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Replicated, Category = "Interactable|Config")
	bool bIsEnabled = true;

	/** Available interaction options for this interactable */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interactable|Config")
	TArray<FInteractionOption> InteractionOptions;

	/** Priority for scoring — higher values are preferred by the detection system */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interactable|Config")
	int32 InteractionPriority = 0;

	// -----------------------------------------------------------------------
	// API
	// -----------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "Interactable")
	TArray<FInteractionOption> GetInteractionOptions(AActor* Interactor) const;

	UFUNCTION(BlueprintCallable, Category = "Interactable")
	bool CanInteract(AActor* Interactor, const FInteractionContext& Context) const;

	UFUNCTION(BlueprintCallable, Category = "Interactable")
	EInteractionResult Interact(AActor* Interactor, FGameplayTag InteractionType);

	UFUNCTION(BlueprintCallable, Category = "Interactable")
	void Enable();

	UFUNCTION(BlueprintCallable, Category = "Interactable")
	void Disable();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Interactable")
	bool IsEnabled() const { return bIsEnabled; }

	// -----------------------------------------------------------------------
	// Events
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Interactable|Events")
	FOnInteractableStatusChanged OnStatusChanged;

	UPROPERTY(BlueprintAssignable, Category = "Interactable|Events")
	FOnInteractionTriggered OnInteractionTriggered;

	// -----------------------------------------------------------------------
	// Interaction handler — set by owning code (e.g. AWorldItem)
	// -----------------------------------------------------------------------

	/** Optional C++ callback invoked when Interact() succeeds. Set by the owning actor. */
	TFunction<EInteractionResult(AActor* Interactor, FGameplayTag InteractionType)> InteractionHandler;

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
