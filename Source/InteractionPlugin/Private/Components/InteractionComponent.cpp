#include "Components/InteractionComponent.h"
#include "Components/InteractableComponent.h"
#include "Detection/InteractionDetectionStrategy.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"
#include "TimerManager.h"

UInteractionComponent::UInteractionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = false;
	SetIsReplicatedByDefault(true);
}

void UInteractionComponent::BeginPlay()
{
	Super::BeginPlay();

	// Detection only runs on the locally controlled client
	APawn* Pawn = Cast<APawn>(GetOwner());
	if (Pawn && Pawn->IsLocallyControlled() && DetectionStrategy)
	{
		GetWorld()->GetTimerManager().SetTimer(
			DetectionTimerHandle, this, &UInteractionComponent::TickDetection,
			DetectionTickRate, true);

		// Enable tick for channeled interaction updates
		PrimaryComponentTick.SetTickFunctionEnable(true);

	}
}

void UInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetTimerManager().ClearTimer(DetectionTimerHandle);
	Super::EndPlay(EndPlayReason);
}

void UInteractionComponent::TickComponent(float DeltaTime, ELevelTick TickType,
	FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (ChanneledState == EChanneledState::Channeling)
	{
		TickChanneling(DeltaTime);
	}
}

// ===========================================================================
// Detection
// ===========================================================================

void UInteractionComponent::TickDetection()
{
	if (!DetectionStrategy || !GetOwner())
	{
		return;
	}

	// 1. Detect candidates via strategy
	TArray<AActor*> Candidates;
	DetectionStrategy->DetectInteractables(GetOwner(), InteractionRange, Candidates);

	// 2. Filter to actors with enabled InteractableComponent
	NearbyInteractables.Reset();
	for (AActor* Candidate : Candidates)
	{
		UInteractableComponent* Interactable = GetInteractable(Candidate);
		if (Interactable && Interactable->IsEnabled())
		{
			FInteractionContext Context = MakeContext(Candidate, FGameplayTag());
			if (Interactable->CanInteract(GetOwner(), Context))
			{
				NearbyInteractables.Add(Candidate);
			}
		}
	}

	// 3. Score and find best target
	AActor* BestTarget = nullptr;
	float BestScore = -FLT_MAX;

	for (AActor* Candidate : NearbyInteractables)
	{
		float Score = ScoreCandidate(Candidate);
		if (Score > BestScore)
		{
			BestScore = Score;
			BestTarget = Candidate;
		}
	}

	// 4. Fire events on target change
	AActor* PreviousTarget = CurrentBestTarget.Get();
	if (BestTarget != PreviousTarget)
	{
		if (PreviousTarget)
		{
			OnInteractableLost.Broadcast(PreviousTarget);
		}

		CurrentBestTarget = BestTarget;

		if (BestTarget)
		{
			OnInteractableFound.Broadcast(BestTarget);
		}
	}
}

// ===========================================================================
// Scoring
// ===========================================================================

float UInteractionComponent::ScoreCandidate_Implementation(AActor* Candidate) const
{
	if (!Candidate || !GetOwner())
	{
		return -FLT_MAX;
	}

	const FVector OwnerLocation = GetOwner()->GetActorLocation();
	const float Distance = FVector::Dist(OwnerLocation, Candidate->GetActorLocation());

	// Distance score: 0..1, closer = higher
	float DistanceScore = 1.0f - FMath::Clamp(Distance / InteractionRange, 0.0f, 1.0f);

	// Angle score: dot product of camera forward and direction to target (-1..1)
	float AngleScore = 0.0f;
	APawn* Pawn = Cast<APawn>(GetOwner());
	APlayerController* PC = Pawn ? Pawn->GetController<APlayerController>() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		FVector CameraForward = PC->PlayerCameraManager->GetCameraRotation().Vector();
		FVector DirectionToTarget = (Candidate->GetActorLocation() - PC->PlayerCameraManager->GetCameraLocation()).GetSafeNormal();
		AngleScore = FVector::DotProduct(CameraForward, DirectionToTarget);
	}
	else
	{
		FVector Forward = GetOwner()->GetActorForwardVector();
		FVector DirectionToTarget = (Candidate->GetActorLocation() - OwnerLocation).GetSafeNormal();
		AngleScore = FVector::DotProduct(Forward, DirectionToTarget);
	}

	// Priority score: 0..1
	float PriorityScore = 0.0f;
	UInteractableComponent* Interactable = GetInteractable(Candidate);
	if (Interactable)
	{
		PriorityScore = FMath::Clamp(static_cast<float>(Interactable->InteractionPriority) / 10.0f, 0.0f, 1.0f);
	}

	return (DistanceScore * DistanceWeight)
		+ (AngleScore * AngleWeight)
		+ (PriorityScore * PriorityWeight);
}

// ===========================================================================
// Instant Interaction
// ===========================================================================

EInteractionResult UInteractionComponent::TryInteract(FGameplayTag InteractionType)
{
	AActor* Target = CurrentBestTarget.Get();
	if (!Target)
	{
		return EInteractionResult::Failed;
	}
	return TryInteractWith(Target, InteractionType);
}

EInteractionResult UInteractionComponent::TryInteractWith(AActor* TargetActor, FGameplayTag InteractionType)
{
	if (!ValidateInteraction(TargetActor, InteractionType))
	{
		return EInteractionResult::Failed;
	}

	// Client: route through server
	if (!GetOwner()->HasAuthority())
	{
		ServerRPC_RequestInteract(TargetActor, InteractionType);
		return EInteractionResult::InProgress;
	}

	// Server/standalone: execute directly
	UInteractableComponent* Interactable = GetInteractable(TargetActor);
	if (!Interactable)
	{
		return EInteractionResult::Failed;
	}

	FInteractionContext Context = MakeContext(TargetActor, InteractionType);
	OnInteractionStarted.Broadcast(Context);

	EInteractionResult Result = Interactable->Interact(GetOwner(), InteractionType);

	if (Result == EInteractionResult::Success)
	{
		OnInteractionCompleted.Broadcast(Context, Result);
	}
	else
	{
		OnInteractionFailed.Broadcast(Context, Result);
	}

	return Result;
}

// ===========================================================================
// Channeled Interaction
// ===========================================================================

void UInteractionComponent::StartChanneledInteraction(AActor* Target, FGameplayTag InteractionType, float Duration)
{
	if (ChanneledState == EChanneledState::Channeling)
	{
		return; // Already channeling
	}

	if (!ValidateInteraction(Target, InteractionType) || Duration <= 0.0f)
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRPC_StartChanneledInteraction(Target, InteractionType, Duration);
	}

	ChanneledTarget = Target;
	ChanneledInteractionType = InteractionType;
	ChanneledDuration = Duration;
	ChanneledElapsed = 0.0f;
	ChanneledProgress = 0.0f;
	ChanneledStartLocation = GetOwner()->GetActorLocation();
	ChanneledState = EChanneledState::Channeling;

	FInteractionContext Context = MakeContext(Target, InteractionType);
	OnInteractionStarted.Broadcast(Context);
}

void UInteractionComponent::CancelChanneledInteraction()
{
	if (ChanneledState != EChanneledState::Channeling)
	{
		return;
	}

	if (!GetOwner()->HasAuthority())
	{
		ServerRPC_CancelChanneledInteraction();
	}

	ChanneledState = EChanneledState::Cancelled;

	FInteractionContext Context = MakeContext(ChanneledTarget.Get(), ChanneledInteractionType);
	OnInteractionFailed.Broadcast(Context, EInteractionResult::Cancelled);

	ChanneledState = EChanneledState::Idle;
	ChanneledTarget = nullptr;
}

void UInteractionComponent::TickChanneling(float DeltaTime)
{
	// Check cancel conditions
	if (!ChanneledTarget.IsValid())
	{
		CancelChanneledInteraction();
		return;
	}

	UInteractableComponent* Interactable = GetInteractable(ChanneledTarget.Get());
	if (!Interactable || !Interactable->IsEnabled())
	{
		CancelChanneledInteraction();
		return;
	}

	// Movement threshold check
	float MoveDist = FVector::Dist(GetOwner()->GetActorLocation(), ChanneledStartLocation);
	if (MoveDist > CancelMoveThreshold)
	{
		CancelChanneledInteraction();
		return;
	}

	// Range check
	float Dist = FVector::Dist(GetOwner()->GetActorLocation(), ChanneledTarget->GetActorLocation());
	if (Dist > InteractionRange * 1.1f)
	{
		CancelChanneledInteraction();
		return;
	}

	ChanneledElapsed += DeltaTime;
	ChanneledProgress = FMath::Clamp(ChanneledElapsed / ChanneledDuration, 0.0f, 1.0f);
	OnChanneledProgress.Broadcast(ChanneledProgress);

	if (ChanneledElapsed >= ChanneledDuration)
	{
		CompleteChanneledInteraction();
	}
}

void UInteractionComponent::CompleteChanneledInteraction()
{
	AActor* Target = ChanneledTarget.Get();
	UInteractableComponent* Interactable = Target ? GetInteractable(Target) : nullptr;

	ChanneledState = EChanneledState::Completed;

	FInteractionContext Context = MakeContext(Target, ChanneledInteractionType);

	if (Interactable)
	{
		EInteractionResult Result = Interactable->Interact(GetOwner(), ChanneledInteractionType);
		if (Result == EInteractionResult::Success)
		{
			OnInteractionCompleted.Broadcast(Context, Result);
		}
		else
		{
			OnInteractionFailed.Broadcast(Context, Result);
		}
	}

	ChanneledState = EChanneledState::Idle;
	ChanneledTarget = nullptr;
	ChanneledProgress = 0.0f;
}

// ===========================================================================
// Server RPCs
// ===========================================================================

void UInteractionComponent::ServerRPC_RequestInteract_Implementation(AActor* TargetActor,
	FGameplayTag InteractionType)
{
	// 1. Actor validity
	if (!IsValid(TargetActor))
	{
		return;
	}

	// 2. Range check with 10% tolerance for latency
	float Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
	if (Distance > InteractionRange * 1.1f)
	{
		ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::OutOfRange);
		return;
	}

	// 3. Interactable check
	UInteractableComponent* Interactable = GetInteractable(TargetActor);
	if (!Interactable || !Interactable->IsEnabled())
	{
		ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::Failed);
		return;
	}

	// 4. Can interact check
	FInteractionContext Context = MakeContext(TargetActor, InteractionType);
	if (!Interactable->CanInteract(GetOwner(), Context))
	{
		ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::NotAllowed);
		return;
	}

	// 5. Execute
	EInteractionResult Result = Interactable->Interact(GetOwner(), InteractionType);

	// 6. Notify client
	ClientRPC_InteractionResult(TargetActor, InteractionType, Result);
}

void UInteractionComponent::ServerRPC_StartChanneledInteraction_Implementation(AActor* Target,
	FGameplayTag InteractionType, float Duration)
{
	if (!IsValid(Target) || Duration <= 0.0f)
	{
		return;
	}

	float Distance = FVector::Dist(GetOwner()->GetActorLocation(), Target->GetActorLocation());
	if (Distance > InteractionRange * 1.1f)
	{
		return;
	}

	UInteractableComponent* Interactable = GetInteractable(Target);
	if (!Interactable || !Interactable->IsEnabled())
	{
		return;
	}

	// Server starts its own channeling state machine
	ChanneledTarget = Target;
	ChanneledInteractionType = InteractionType;
	ChanneledDuration = Duration;
	ChanneledElapsed = 0.0f;
	ChanneledStartLocation = GetOwner()->GetActorLocation();
	ChanneledState = EChanneledState::Channeling;
	PrimaryComponentTick.SetTickFunctionEnable(true);
}

void UInteractionComponent::ServerRPC_CancelChanneledInteraction_Implementation()
{
	if (ChanneledState == EChanneledState::Channeling)
	{
		ChanneledState = EChanneledState::Cancelled;
		ChanneledState = EChanneledState::Idle;
		ChanneledTarget = nullptr;
	}
}

// ===========================================================================
// Client RPC
// ===========================================================================

void UInteractionComponent::ClientRPC_InteractionResult_Implementation(AActor* TargetActor,
	FGameplayTag InteractionType, EInteractionResult Result)
{
	FInteractionContext Context = MakeContext(TargetActor, InteractionType);

	if (Result == EInteractionResult::Success)
	{
		OnInteractionCompleted.Broadcast(Context, Result);
	}
	else
	{
		OnInteractionFailed.Broadcast(Context, Result);
	}
}

// ===========================================================================
// Validation Helpers
// ===========================================================================

bool UInteractionComponent::ValidateInteraction(AActor* TargetActor, FGameplayTag InteractionType,
	float RangeTolerance) const
{
	if (!IsValid(TargetActor) || !GetOwner())
	{
		return false;
	}

	UInteractableComponent* Interactable = GetInteractable(TargetActor);
	if (!Interactable || !Interactable->IsEnabled())
	{
		return false;
	}

	float Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
	if (Distance > InteractionRange * RangeTolerance)
	{
		return false;
	}

	FInteractionContext Context = MakeContext(TargetActor, InteractionType);
	return Interactable->CanInteract(GetOwner(), Context);
}

UInteractableComponent* UInteractionComponent::GetInteractable(AActor* Actor) const
{
	if (!Actor)
	{
		return nullptr;
	}
	return Actor->FindComponentByClass<UInteractableComponent>();
}

FInteractionContext UInteractionComponent::MakeContext(AActor* TargetActor, FGameplayTag InteractionType) const
{
	FInteractionContext Context;
	Context.Interactor = GetOwner();
	Context.InteractableActor = TargetActor;
	Context.InteractionType = InteractionType;
	if (TargetActor)
	{
		Context.InteractionLocation = TargetActor->GetActorLocation();
		Context.Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
	}
	return Context;
}
