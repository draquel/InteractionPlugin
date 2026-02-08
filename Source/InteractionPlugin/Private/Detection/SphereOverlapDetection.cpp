#include "Detection/SphereOverlapDetection.h"
#include "CollisionQueryParams.h"
#include "Engine/OverlapResult.h"

void USphereOverlapDetection::DetectInteractables_Implementation(AActor* SourceActor, float InteractionRange,
	TArray<AActor*>& OutCandidates) const
{
	OutCandidates.Reset();

	if (!SourceActor || !SourceActor->GetWorld())
	{
		return;
	}

	const FVector Origin = SourceActor->GetActorLocation();

	FCollisionShape Sphere = FCollisionShape::MakeSphere(InteractionRange);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(InteractionOverlap), false, SourceActor);

	TArray<FOverlapResult> Overlaps;
	SourceActor->GetWorld()->OverlapMultiByChannel(
		Overlaps,
		Origin,
		FQuat::Identity,
		CollisionChannel,
		Sphere,
		Params
	);

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* Actor = Overlap.GetActor();
		if (Actor && Actor != SourceActor)
		{
			OutCandidates.AddUnique(Actor);
		}
	}
}
