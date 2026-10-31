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
	TArray<FInteractionOption> Options = InteractionOptions;
	if (OptionsModifier)
	{
		OptionsModifier(Interactor, Options);
	}
	return Options;
}

void UInteractableComponent::NotifyOptionsChanged()
{
	OnOptionsChanged.Broadcast(this);
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

	// Resolve interaction type: if none specified, use the first available option
	FGameplayTag ResolvedType = InteractionType;
	if (!ResolvedType.IsValid() && InteractionOptions.Num() > 0)
	{
		ResolvedType = InteractionOptions[0].InteractionType;
	}

	// Verify the resolved interaction type is one of our options
	bool bValidOption = false;
	for (const FInteractionOption& Option : InteractionOptions)
	{
		if (Option.InteractionType == ResolvedType)
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
		Result = InteractionHandler(Interactor, ResolvedType);
	}

	if (Result == EInteractionResult::Success)
	{
		OnInteractionTriggered.Broadcast(Interactor, ResolvedType);
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

void UInteractableComponent::SetEnabled(bool bEnabled)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}
	if (bEnabled)
	{
		Enable();
	}
	else
	{
		Disable();
	}
}
