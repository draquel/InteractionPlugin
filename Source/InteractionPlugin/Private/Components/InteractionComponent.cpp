#include "Components/InteractionComponent.h"
#include "InteractionPlugin.h"
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

	// Detection only runs on the locally controlled client. The pawn may not be possessed yet (respawns are
	// spawned first, possessed after), so follow controller changes instead of deciding once here.
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		Pawn->ReceiveControllerChangedDelegate.AddDynamic(this, &UInteractionComponent::HandleControllerChanged);
	}
	UpdateDetectionActive();
}

void UInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		Pawn->ReceiveControllerChangedDelegate.RemoveDynamic(this, &UInteractionComponent::HandleControllerChanged);
	}
	if (UInteractableComponent* Hooked = HookedTargetInteractable.Get())
	{
		Hooked->OnOptionsChanged.RemoveDynamic(this, &UInteractionComponent::HandleTargetOptionsChanged);
	}
	HookedTargetInteractable.Reset();
	// The world can already be gone when an orphaned component ends play (test harnesses tearing down a world).
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DetectionTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void UInteractionComponent::HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController)
{
	UpdateDetectionActive();
}

void UInteractionComponent::UpdateDetectionActive()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const APawn* Pawn = Cast<APawn>(GetOwner());
	const bool bWantDetection = Pawn && Pawn->IsLocallyControlled() && DetectionStrategy;
	FTimerManager& Timers = World->GetTimerManager();
	const bool bDetecting = Timers.IsTimerActive(DetectionTimerHandle);

	if (bWantDetection && !bDetecting)
	{
		Timers.SetTimer(DetectionTimerHandle, this, &UInteractionComponent::TickDetection, DetectionTickRate, true);
		// Enable tick for channeled interaction updates
		PrimaryComponentTick.SetTickFunctionEnable(true);
	}
	else if (!bWantDetection && bDetecting)
	{
		Timers.ClearTimer(DetectionTimerHandle);
		NearbyInteractables.Reset();
		SetBestTarget(nullptr);
		if (ChanneledState != EChanneledState::Channeling)
		{
			PrimaryComponentTick.SetTickFunctionEnable(false);
		}
	}
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
	if (BestTarget != CurrentBestTarget.Get())
	{
		SetBestTarget(BestTarget);
	}
}

void UInteractionComponent::SetBestTarget(AActor* Target)
{
	AActor* PreviousTarget = CurrentBestTarget.Get();
	if (UInteractableComponent* Hooked = HookedTargetInteractable.Get())
	{
		Hooked->OnOptionsChanged.RemoveDynamic(this, &UInteractionComponent::HandleTargetOptionsChanged);
	}
	HookedTargetInteractable.Reset();

	if (PreviousTarget && PreviousTarget != Target)
	{
		OnInteractableLost.Broadcast(PreviousTarget);
	}

	CurrentBestTarget = Target;

	if (Target)
	{
		// Prompt text can depend on the target's state (a lit vs. dark torch): follow it while it is the target.
		if (UInteractableComponent* Interactable = Target->FindComponentByClass<UInteractableComponent>())
		{
			Interactable->OnOptionsChanged.AddUniqueDynamic(this, &UInteractionComponent::HandleTargetOptionsChanged);
			HookedTargetInteractable = Interactable;
		}
		if (Target != PreviousTarget)
		{
			OnInteractableFound.Broadcast(Target);
		}
	}
}

void UInteractionComponent::HandleTargetOptionsChanged(UInteractableComponent* Interactable)
{
	AActor* Target = CurrentBestTarget.Get();
	if (Target && Interactable && Interactable->GetOwner() == Target)
	{
		OnInteractableFound.Broadcast(Target);
	}
}

void UInteractionComponent::RefreshCurrentTarget()
{
	if (AActor* Target = CurrentBestTarget.Get())
	{
		OnInteractableFound.Broadcast(Target);
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
	if (CheckInteraction(TargetActor, InteractionType) != EInteractionResult::Success)
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

EInteractionResult UInteractionComponent::StartChanneledInteraction(AActor* Target, FGameplayTag InteractionType, float Duration)
{
	if (ChanneledState == EChanneledState::Channeling)
	{
		return EInteractionResult::Failed; // Already channeling
	}

	const EInteractionResult Check = CheckInteraction(Target, InteractionType);
	if (Check != EInteractionResult::Success)
	{
		return Check;
	}

	const float ChannelDuration = ResolveChannelDuration(Target, InteractionType, Duration);
	if (ChannelDuration <= 0.0f)
	{
		return EInteractionResult::Failed;
	}

	if (!GetOwner()->HasAuthority())
	{
		// The server checks again and runs the authoritative channel; this copy only shows progress.
		ServerRPC_StartChanneledInteraction(Target, InteractionType, Duration);
	}

	BeginChannel(Target, InteractionType, ChannelDuration);

	FInteractionContext Context = MakeContext(Target, InteractionType);
	OnInteractionStarted.Broadcast(Context);
	return EInteractionResult::InProgress;
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

	AActor* Target = ChanneledTarget.Get();
	FInteractionContext Context = MakeContext(Target, ChanneledInteractionType);
	OnInteractionFailed.Broadcast(Context, EInteractionResult::Cancelled);

	// The server ended a remote player's channel (moved away, target gone): its client stops showing progress.
	if (GetOwner()->HasAuthority() && IsRemotelyControlled())
	{
		ClientRPC_InteractionResult(Target, ChanneledInteractionType, EInteractionResult::Cancelled);
	}

	EndChannel();
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

	const bool bWasFull = ChanneledElapsed >= ChanneledDuration;
	ChanneledElapsed += DeltaTime;
	ChanneledProgress = FMath::Clamp(ChanneledElapsed / ChanneledDuration, 0.0f, 1.0f);
	if (!bWasFull)
	{
		OnChanneledProgress.Broadcast(ChanneledProgress);
	}

	if (ChanneledElapsed < ChanneledDuration)
	{
		return;
	}

	if (GetOwner()->HasAuthority())
	{
		CompleteChanneledInteraction();
	}
	else if (ChanneledElapsed >= ChanneledDuration + ChannelResultTimeout)
	{
		// A client never completes: it holds at full progress for the server's result (G92). None came.
		UE_LOG(LogIAP, Warning, TEXT("Channeled interaction with %s: no result from the server after %.1f s; cancelled."),
			*GetNameSafe(ChanneledTarget.Get()), ChannelResultTimeout);
		CancelChanneledInteraction();
	}
}

void UInteractionComponent::CompleteChanneledInteraction()
{
	// Interact runs on the authority only: a client's channel is a progress display (G92).
	if (!GetOwner()->HasAuthority())
	{
		return;
	}

	AActor* Target = ChanneledTarget.Get();
	UInteractableComponent* Interactable = Target ? GetInteractable(Target) : nullptr;
	const FGameplayTag InteractionType = ChanneledInteractionType;

	ChanneledState = EChanneledState::Completed;

	FInteractionContext Context = MakeContext(Target, InteractionType);

	EInteractionResult Result = EInteractionResult::Failed;
	if (Interactable)
	{
		Result = Interactable->Interact(GetOwner(), InteractionType);
		if (Result == EInteractionResult::Success)
		{
			OnInteractionCompleted.Broadcast(Context, Result);
		}
		else
		{
			OnInteractionFailed.Broadcast(Context, Result);
		}
	}

	if (IsRemotelyControlled())
	{
		ClientRPC_InteractionResult(Target, InteractionType, Result);
	}

	EndChannel();
}

void UInteractionComponent::BeginChannel(AActor* Target, FGameplayTag InteractionType, float Duration)
{
	ChanneledTarget = Target;
	ChanneledInteractionType = InteractionType;
	ChanneledDuration = Duration;
	ChanneledElapsed = 0.0f;
	ChanneledProgress = 0.0f;
	ChanneledStartLocation = GetOwner()->GetActorLocation();
	ChanneledState = EChanneledState::Channeling;
	// Detection enables the tick on the locally controlled pawn only; a server channel for a remote player needs it too.
	PrimaryComponentTick.SetTickFunctionEnable(true);
}

void UInteractionComponent::EndChannel()
{
	ChanneledState = EChanneledState::Idle;
	ChanneledTarget = nullptr;
	ChanneledElapsed = 0.0f;
	ChanneledProgress = 0.0f;
	// The tick stays on while detection runs (UpdateDetectionActive); otherwise it was on for this channel only.
	const UWorld* World = GetWorld();
	if (!World || !World->GetTimerManager().IsTimerActive(DetectionTimerHandle))
	{
		PrimaryComponentTick.SetTickFunctionEnable(false);
	}
}

float UInteractionComponent::ResolveChannelDuration(AActor* TargetActor, FGameplayTag InteractionType, float Duration) const
{
	const UInteractableComponent* Interactable = GetInteractable(TargetActor);
	if (!Interactable)
	{
		return 0.0f;
	}

	// The option as this interactor sees it (an options modifier may change it); no type means the first, as Interact does.
	const TArray<FInteractionOption> Options = Interactable->GetInteractionOptions(GetOwner());
	const FInteractionOption* Option = InteractionType.IsValid()
		? Options.FindByPredicate([&InteractionType](const FInteractionOption& Candidate) { return Candidate.InteractionType == InteractionType; })
		: (Options.Num() > 0 ? &Options[0] : nullptr);
	if (!Option)
	{
		return 0.0f;
	}

	const float HoldFloor = Option->bRequiresHold ? Option->HoldDuration : 0.0f;
	return FMath::Max(Duration, HoldFloor);
}

bool UInteractionComponent::IsRemotelyControlled() const
{
	if (const APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		return Pawn->GetController() && !Pawn->IsLocallyControlled();
	}
	if (const AController* Controller = Cast<AController>(GetOwner()))
	{
		return !Controller->IsLocalController();
	}
	return false;
}

// ===========================================================================
// Server RPCs
// ===========================================================================

void UInteractionComponent::ServerRPC_RequestInteract_Implementation(AActor* TargetActor,
	FGameplayTag InteractionType)
{
	// 1. The client's checks again, with 10% range tolerance for latency
	const EInteractionResult Check = CheckInteraction(TargetActor, InteractionType, 1.1f);
	if (Check != EInteractionResult::Success)
	{
		ClientRPC_InteractionResult(TargetActor, InteractionType, Check);
		return;
	}

	// 2. Execute
	UInteractableComponent* Interactable = GetInteractable(TargetActor);
	EInteractionResult Result = Interactable->Interact(GetOwner(), InteractionType);

	// 3. Notify client
	ClientRPC_InteractionResult(TargetActor, InteractionType, Result);
}

void UInteractionComponent::ServerRPC_StartChanneledInteraction_Implementation(AActor* Target,
	FGameplayTag InteractionType, float Duration)
{
	// A refusal answers, so the client stops the progress it already shows.
	if (ChanneledState == EChanneledState::Channeling)
	{
		ClientRPC_InteractionResult(Target, InteractionType, EInteractionResult::Failed);
		return;
	}

	// The same checks as the instant path, CanInteract included (G92)
	const EInteractionResult Check = CheckInteraction(Target, InteractionType, 1.1f);
	if (Check != EInteractionResult::Success)
	{
		ClientRPC_InteractionResult(Target, InteractionType, Check);
		return;
	}

	// The option must be offered, and its hold is the server's length, whatever the client asked
	const float ChannelDuration = ResolveChannelDuration(Target, InteractionType, Duration);
	if (ChannelDuration <= 0.0f)
	{
		ClientRPC_InteractionResult(Target, InteractionType, EInteractionResult::Failed);
		return;
	}

	// Server starts its own channeling state machine; it alone completes it
	BeginChannel(Target, InteractionType, ChannelDuration);
}

void UInteractionComponent::ServerRPC_CancelChanneledInteraction_Implementation()
{
	// The client cancelled and already reported it; no answer.
	if (ChanneledState == EChanneledState::Channeling)
	{
		EndChannel();
	}
}

// ===========================================================================
// Client RPC
// ===========================================================================

void UInteractionComponent::ClientRPC_InteractionResult_Implementation(AActor* TargetActor,
	FGameplayTag InteractionType, EInteractionResult Result)
{
	// The server's word ends this client's copy of a channel with that target (completed, refused or cancelled there).
	const bool bEndsChannel = ChanneledState == EChanneledState::Channeling && ChanneledTarget.Get() == TargetActor;
	if (bEndsChannel)
	{
		EndChannel();
	}
	else if (Result == EInteractionResult::Cancelled)
	{
		// The client ended this channel itself and has reported it.
		return;
	}

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

EInteractionResult UInteractionComponent::CheckInteraction(AActor* TargetActor, FGameplayTag InteractionType,
	float RangeTolerance) const
{
	if (!IsValid(TargetActor) || !GetOwner())
	{
		return EInteractionResult::Failed;
	}

	float Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
	if (Distance > InteractionRange * RangeTolerance)
	{
		return EInteractionResult::OutOfRange;
	}

	UInteractableComponent* Interactable = GetInteractable(TargetActor);
	if (!Interactable || !Interactable->IsEnabled())
	{
		return EInteractionResult::Failed;
	}

	FInteractionContext Context = MakeContext(TargetActor, InteractionType);
	if (!Interactable->CanInteract(GetOwner(), Context))
	{
		return EInteractionResult::NotAllowed;
	}

	return EInteractionResult::Success;
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
