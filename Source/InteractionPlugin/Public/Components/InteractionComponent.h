#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "Types/CGFCommonEnums.h"
#include "Types/CGFInteractionTypes.h"
#include "InteractionComponent.generated.h"

class AController;
class APawn;
class UInteractableComponent;
class UInteractionDetectionStrategy;

/**
 * Channeled interaction state
 */
UENUM(BlueprintType)
enum class EChanneledState : uint8
{
	Idle,
	Channeling,
	Completed,
	Cancelled
};

/**
 * Drives interaction detection, targeting, scoring, and execution.
 * Attach to the player pawn. Detection runs on local client only;
 * interactions are server-authoritative via RPCs.
 */
UCLASS(BlueprintType, ClassGroup = "Interaction", meta = (BlueprintSpawnableComponent))
class INTERACTIONPLUGIN_API UInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UInteractionComponent();

	// -----------------------------------------------------------------------
	// Configuration
	// -----------------------------------------------------------------------

	/** Maximum detection and interaction range */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Config")
	float InteractionRange = 1000.0f;

	/** Seconds between detection ticks */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Config")
	float DetectionTickRate = 0.1f;

	/** Detection strategy — determines how candidates are found */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Instanced, Category = "Interaction|Config")
	TObjectPtr<UInteractionDetectionStrategy> DetectionStrategy;

	/** Scoring weights */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Scoring")
	float DistanceWeight = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Scoring")
	float AngleWeight = 0.4f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Scoring")
	float PriorityWeight = 0.2f;

	/** How far the player can move before a channeled interaction is cancelled */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Channeling")
	float CancelMoveThreshold = 50.0f;

	/**
	 * How long a client holds a finished channel at full progress for the server's result before it gives up and
	 * cancels (the request or the answer was lost: the owner is not this connection's, say)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Channeling", meta = (ClampMin = "0"))
	float ChannelResultTimeout = 2.0f;

	// -----------------------------------------------------------------------
	// State (read-only from Blueprint)
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintReadOnly, Category = "Interaction|State")
	TArray<TObjectPtr<AActor>> NearbyInteractables;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction|State")
	TWeakObjectPtr<AActor> CurrentBestTarget;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction|State")
	EChanneledState ChanneledState = EChanneledState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "Interaction|State")
	float ChanneledProgress = 0.0f;

	// -----------------------------------------------------------------------
	// Interaction API
	// -----------------------------------------------------------------------

	/** Attempt an instant interaction with the current best target */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	EInteractionResult TryInteract(FGameplayTag InteractionType);

	/** Attempt an instant interaction with a specific target */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	EInteractionResult TryInteractWith(AActor* TargetActor, FGameplayTag InteractionType);

	/**
	 * Start a channeled interaction. The server runs the channel and completes it (the interactable's Interact runs there
	 * only); the owning client runs a copy for OnChanneledProgress and finishes when the server's result arrives (G92).
	 * @param Target          The interactable actor
	 * @param InteractionType One of the options it offers (invalid = its first)
	 * @param Duration        Seconds to channel; an option with bRequiresHold is never shorter than its HoldDuration
	 * @return InProgress when the channel started; Failed (already channeling, no such option, no duration), OutOfRange
	 *         or NotAllowed (the interactable refuses) otherwise
	 */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	EInteractionResult StartChanneledInteraction(AActor* Target, FGameplayTag InteractionType, float Duration);

	/** Cancel an in-progress channeled interaction */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	void CancelChanneledInteraction();

	// -----------------------------------------------------------------------
	// Scoring (overridable)
	// -----------------------------------------------------------------------

	/** Score a candidate for targeting priority. Higher = better. */
	UFUNCTION(BlueprintNativeEvent, Category = "Interaction|Scoring")
	float ScoreCandidate(AActor* Candidate) const;

	// -----------------------------------------------------------------------
	// Events
	// -----------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnInteractableFound OnInteractableFound;

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnInteractableLost OnInteractableLost;

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnInteractionStarted OnInteractionStarted;

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnInteractionCompleted OnInteractionCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnInteractionFailed OnInteractionFailed;

	UPROPERTY(BlueprintAssignable, Category = "Interaction|Events")
	FOnChanneledInteractionProgress OnChanneledProgress;

	/**
	 * Re-announce the current target (OnInteractableFound) so prompts rebuild their text. Call when the interactor's
	 * own state changes what the target offers -- e.g. it lit or put out the torch it carries.
	 */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	void RefreshCurrentTarget();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	// -----------------------------------------------------------------------
	// Detection
	// -----------------------------------------------------------------------

	void TickDetection();
	FTimerHandle DetectionTimerHandle;

	/**
	 * Run detection exactly while the owning pawn is locally controlled. Checked at BeginPlay AND on every
	 * controller change: a respawned pawn is spawned (BeginPlay) before the game mode possesses it, so a
	 * BeginPlay-only check left every pawn after the first without detection — no prompt, nothing to interact with.
	 */
	void UpdateDetectionActive();

	UFUNCTION()
	void HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);

	/**
	 * Make Target the current best target: unhook the previous target's options-changed event, hook the new one's,
	 * and broadcast lost / found. Null clears the target.
	 */
	void SetBestTarget(AActor* Target);

	/** The current target's options changed (its text, its enabled state): re-announce it so prompts refresh. */
	UFUNCTION()
	void HandleTargetOptionsChanged(UInteractableComponent* Interactable);

	/** The interactable component whose OnOptionsChanged is hooked (the current target's). */
	TWeakObjectPtr<UInteractableComponent> HookedTargetInteractable;

	// -----------------------------------------------------------------------
	// Channeled interaction state
	// -----------------------------------------------------------------------

	TWeakObjectPtr<AActor> ChanneledTarget;
	FGameplayTag ChanneledInteractionType;
	float ChanneledDuration = 0.0f;
	float ChanneledElapsed = 0.0f;
	FVector ChanneledStartLocation = FVector::ZeroVector;

	void TickChanneling(float DeltaTime);
	/** Authority: run the interactable's Interact, broadcast, and tell a remote owner the result. */
	void CompleteChanneledInteraction();
	/** Enter the Channeling state (both machines) and make sure the component ticks. */
	void BeginChannel(AActor* Target, FGameplayTag InteractionType, float Duration);
	/** Back to Idle with no target and no progress. */
	void EndChannel();

	/**
	 * The channel length for an option: the requested Duration, never shorter than the option's HoldDuration when it
	 * requires a hold, so a client cannot shorten a hold. Both machines use it.
	 * @return The duration, or 0 when the target does not offer the option (or there is no duration)
	 */
	float ResolveChannelDuration(AActor* TargetActor, FGameplayTag InteractionType, float Duration) const;

	/** The owner is a pawn or controller driven from another machine: results go to it by ClientRPC_InteractionResult. */
	bool IsRemotelyControlled() const;

	// -----------------------------------------------------------------------
	// Server RPCs
	// -----------------------------------------------------------------------

	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestInteract(AActor* TargetActor, FGameplayTag InteractionType);

	UFUNCTION(Server, Reliable)
	void ServerRPC_StartChanneledInteraction(AActor* Target, FGameplayTag InteractionType, float Duration);

	UFUNCTION(Server, Reliable)
	void ServerRPC_CancelChanneledInteraction();

	// -----------------------------------------------------------------------
	// Client RPCs
	// -----------------------------------------------------------------------

	UFUNCTION(Client, Reliable)
	void ClientRPC_InteractionResult(AActor* TargetActor, FGameplayTag InteractionType, EInteractionResult Result);

	// -----------------------------------------------------------------------
	// Validation helpers (shared client/server)
	// -----------------------------------------------------------------------

	/**
	 * The one interaction predicate (D-CR16): the client's pre-check (RangeTolerance 1) and both server RPCs (1.1, for
	 * latency) run it. The target is valid, within range, has an enabled interactable, and that interactable's
	 * CanInteract allows the interactor.
	 * @return Success; Failed (no target or interactable, or it is disabled), OutOfRange, NotAllowed
	 */
	EInteractionResult CheckInteraction(AActor* TargetActor, FGameplayTag InteractionType, float RangeTolerance = 1.0f) const;
	UInteractableComponent* GetInteractable(AActor* Actor) const;

	/** Build interaction context for the given target */
	FInteractionContext MakeContext(AActor* TargetActor, FGameplayTag InteractionType) const;
};
