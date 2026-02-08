#include "Detection/LineTraceDetection.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Camera/PlayerCameraManager.h"

void ULineTraceDetection::DetectInteractables_Implementation(AActor* SourceActor, float InteractionRange,
	TArray<AActor*>& OutCandidates) const
{
	OutCandidates.Reset();

	if (!SourceActor || !SourceActor->GetWorld())
	{
		return;
	}

	// Get camera viewpoint for trace origin and direction
	FVector TraceStart;
	FVector TraceDir;

	APawn* Pawn = Cast<APawn>(SourceActor);
	APlayerController* PC = Pawn ? Pawn->GetController<APlayerController>() : nullptr;
	if (PC && PC->PlayerCameraManager)
	{
		TraceStart = PC->PlayerCameraManager->GetCameraLocation();
		TraceDir = PC->PlayerCameraManager->GetCameraRotation().Vector();
	}
	else
	{
		TraceStart = SourceActor->GetActorLocation();
		TraceDir = SourceActor->GetActorForwardVector();
	}

	const FVector TraceEnd = TraceStart + TraceDir * InteractionRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(InteractionTrace), false, SourceActor);

	if (bMultiTrace)
	{
		TArray<FHitResult> Hits;
		SourceActor->GetWorld()->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd, CollisionChannel, Params);

		for (const FHitResult& Hit : Hits)
		{
			AActor* Actor = Hit.GetActor();
			if (Actor && Actor != SourceActor)
			{
				OutCandidates.AddUnique(Actor);
			}
		}
	}
	else
	{
		FHitResult Hit;
		if (SourceActor->GetWorld()->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, CollisionChannel, Params))
		{
			AActor* Actor = Hit.GetActor();
			if (Actor && Actor != SourceActor)
			{
				OutCandidates.Add(Actor);
			}
		}
	}
}
