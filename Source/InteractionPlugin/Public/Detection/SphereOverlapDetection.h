#pragma once

#include "CoreMinimal.h"
#include "Detection/InteractionDetectionStrategy.h"
#include "SphereOverlapDetection.generated.h"

/**
 * Sphere overlap detection strategy.
 * Performs a sphere overlap query centered on the source actor.
 * Best for third-person games and broad-area interaction.
 */
UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced)
class INTERACTIONPLUGIN_API USphereOverlapDetection : public UInteractionDetectionStrategy
{
	GENERATED_BODY()

public:
	/** Collision channel used for the overlap query */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	TEnumAsByte<ECollisionChannel> CollisionChannel = ECC_WorldDynamic;

	virtual void DetectInteractables_Implementation(AActor* SourceActor, float InteractionRange,
		TArray<AActor*>& OutCandidates) const override;
};
