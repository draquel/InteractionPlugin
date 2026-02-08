#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "Types/CGFCommonEnums.h"
#include "Types/CGFInteractionTypes.h"
#include "InteractionComponent.generated.h"

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

	/** Cancel channeled interaction if the player takes damage */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Interaction|Channeling")
	bool bCancelOnDamage = true;

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

	/** Start a channeled interaction */
	UFUNCTION(BlueprintCallable, Category = "Interaction")
	void StartChanneledInteraction(AActor* Target, FGameplayTag InteractionType, float Duration);

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

	// -----------------------------------------------------------------------
	// Channeled interaction state
	// -----------------------------------------------------------------------

	TWeakObjectPtr<AActor> ChanneledTarget;
	FGameplayTag ChanneledInteractionType;
	float ChanneledDuration = 0.0f;
	float ChanneledElapsed = 0.0f;
	FVector ChanneledStartLocation = FVector::ZeroVector;

	void TickChanneling(float DeltaTime);
	void CompleteChanneledInteraction();

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

	bool ValidateInteraction(AActor* TargetActor, FGameplayTag InteractionType, float RangeTolerance = 1.0f) const;
	UInteractableComponent* GetInteractable(AActor* Actor) const;

	/** Build interaction context for the given target */
	FInteractionContext MakeContext(AActor* TargetActor, FGameplayTag InteractionType) const;
};
