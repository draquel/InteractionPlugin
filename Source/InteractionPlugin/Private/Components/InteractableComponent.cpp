#include "Components/InteractableComponent.h"
#include "Net/UnrealNetwork.h"

UInteractableComponent::UInteractableComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UInteractableComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UInteractableComponent, bIsEnabled);
}

TArray<FInteractionOption> UInteractableComponent::GetInteractionOptions(AActor* Interactor) const
{
	if (!bIsEnabled)
	{
		return TArray<FInteractionOption>();
	}
	return InteractionOptions;
}

bool UInteractableComponent::CanInteract(AActor* Interactor, const FInteractionContext& Context) const
{
	return bIsEnabled && Interactor != nullptr;
}

EInteractionResult UInteractableComponent::Interact(AActor* Interactor, FGameplayTag InteractionType)
{
	if (!bIsEnabled || !Interactor)
	{
		return EInteractionResult::NotAllowed;
	}

	// Verify the requested interaction type is one of our options
	bool bValidOption = false;
	for (const FInteractionOption& Option : InteractionOptions)
	{
		if (Option.InteractionType == InteractionType)
		{
			bValidOption = true;
			break;
		}
	}

	if (!bValidOption)
	{
		return EInteractionResult::Failed;
	}

	// Use handler if set, otherwise succeed
	EInteractionResult Result = EInteractionResult::Success;
	if (InteractionHandler)
	{
		Result = InteractionHandler(Interactor, InteractionType);
	}

	if (Result == EInteractionResult::Success)
	{
		OnInteractionTriggered.Broadcast(Interactor, InteractionType);
	}

	return Result;
}

void UInteractableComponent::Enable()
{
	if (!bIsEnabled)
	{
		bIsEnabled = true;
		OnStatusChanged.Broadcast(true);
	}
}

void UInteractableComponent::Disable()
{
	if (bIsEnabled)
	{
		bIsEnabled = false;
		OnStatusChanged.Broadcast(false);
	}
}
