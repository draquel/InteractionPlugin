#pragma once

#include "CoreMinimal.h"
#include "Detection/InteractionDetectionStrategy.h"
#include "LineTraceDetection.generated.h"

/**
 * Line trace detection strategy.
 * Fires a line trace from the camera location along camera forward.
 * Best for first-person games and precision aiming.
 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class INTERACTIONPLUGIN_API ULineTraceDetection : public UInteractionDetectionStrategy
{
	GENERATED_BODY()

public:
	/** Collision channel used for the trace */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	TEnumAsByte<ECollisionChannel> CollisionChannel = ECC_Visibility;

	/** If true, returns all hit actors along the trace. If false, only the first. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	bool bMultiTrace = false;

	virtual void DetectInteractables_Implementation(AActor* SourceActor, float InteractionRange,
		TArray<AActor*>& OutCandidates) const override;
};
