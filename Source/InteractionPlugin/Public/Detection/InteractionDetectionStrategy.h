#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "InteractionDetectionStrategy.generated.h"

/**
 * Abstract base for interaction detection strategies.
 * Subclass and override DetectInteractables to customize how nearby
 * interactable actors are discovered (overlap, trace, custom).
 */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class INTERACTIONPLUGIN_API UInteractionDetectionStrategy : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Detect interactable candidates near the source actor.
	 * @param SourceActor       The actor performing detection (typically the player pawn)
	 * @param InteractionRange  Maximum detection distance
	 * @param OutCandidates     Populated with detected actor candidates
	 */
	UFUNCTION(BlueprintNativeEvent, Category = "Interaction|Detection")
	void DetectInteractables(AActor* SourceActor, float InteractionRange, TArray<AActor*>& OutCandidates) const;

	virtual void DetectInteractables_Implementation(AActor* SourceActor, float InteractionRange,
		TArray<AActor*>& OutCandidates) const
	{
	}
};
