#include "Misc/AutomationTest.h"
#include "Components/InteractableComponent.h"
#include "Components/InteractionComponent.h"
#include "Detection/LineTraceDetection.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Tags/CGFGameplayTags.h"
#include "Types/CGFInteractionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// The interaction flow on a test world with the line-trace strategy (code review CR2, D-CR23 / FW-34): an interactor actor
// looking at a blocking cube with an interactable component detects it along its forward vector (no player controller: the
// strategy traces from the actor), TryInteractWith runs the interactable's handler and returns its result (standalone: the
// owner has authority), and a disabled or an out-of-range interactable fails. The world is the gravity test's: created and
// given a context, no play begun; collision queries work there.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionFlowLineTraceTest,
	"Interaction.Flow.LineTraceAndInteract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInteractionFlowLineTraceTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("The engine's cube mesh"), Cube))
	{
		return true;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("InteractionFlowTest"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	// The interactor: a plain actor facing +X with an interaction component on the line-trace strategy.
	AActor* Interactor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	USceneComponent* Root = NewObject<USceneComponent>(Interactor, TEXT("Root"));
	Interactor->SetRootComponent(Root);
	Root->RegisterComponent();
	UInteractionComponent* Interaction = NewObject<UInteractionComponent>(Interactor, TEXT("Interaction"));
	ULineTraceDetection* Strategy = NewObject<ULineTraceDetection>(Interaction);
	Interaction->DetectionStrategy = Strategy;
	Interaction->RegisterComponent();

	// The target: a blocking cube 300 uu ahead with an interactable whose one option is Use; its handler records the caller.
	auto SpawnInteractable = [World, Cube](const FVector& Location, bool bEnabled, AActor*& OutActor, UInteractableComponent*& OutComponent, TArray<AActor*>& Callers)
	{
		AStaticMeshActor* Target = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Target->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Target->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Target->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
		UInteractableComponent* Interactable = NewObject<UInteractableComponent>(Target, TEXT("Interactable"));
		Interactable->bIsEnabled = bEnabled;
		FInteractionOption Use;
		Use.InteractionType = CGFGameplayTags::Interaction_Type_Use;
		Use.DisplayText = FText::FromString(TEXT("Use"));
		Interactable->InteractionOptions.Add(Use);
		Interactable->InteractionHandler = [&Callers](AActor* Caller, FGameplayTag) { Callers.Add(Caller); return EInteractionResult::Success; };
		Interactable->RegisterComponent();
		OutActor = Target;
		OutComponent = Interactable;
	};
	TArray<AActor*> Callers;
	AActor* Target = nullptr;
	UInteractableComponent* Interactable = nullptr;
	SpawnInteractable(FVector(300.0, 0.0, 0.0), true, Target, Interactable, Callers);

	// Detection: the strategy's trace from the actor along its forward vector hits the cube.
	TArray<AActor*> Candidates;
	Strategy->DetectInteractables(Interactor, Interaction->InteractionRange, Candidates);
	TestTrue(TEXT("The line trace finds the cube ahead"), Candidates.Contains(Target));

	// The flow: the handler runs with the interactor and its result comes back.
	const EInteractionResult Used = Interaction->TryInteractWith(Target, CGFGameplayTags::Interaction_Type_Use);
	TestEqual(TEXT("Using the interactable succeeds"), static_cast<int32>(Used), static_cast<int32>(EInteractionResult::Success));
	TestEqual(TEXT("The handler ran once"), Callers.Num(), 1);
	TestTrue(TEXT("The handler saw the interactor"), Callers.Num() == 1 && Callers[0] == Interactor);

	// An option the interactable does not offer fails in the interactable; a disabled one and one out of range fail before it.
	TestEqual(TEXT("An option the interactable lacks is Failed"), static_cast<int32>(Interaction->TryInteractWith(Target, CGFGameplayTags::Interaction_Type_Pickup)),
		static_cast<int32>(EInteractionResult::Failed));
	Interactable->bIsEnabled = false;
	TestEqual(TEXT("A disabled interactable is Failed"), static_cast<int32>(Interaction->TryInteractWith(Target, CGFGameplayTags::Interaction_Type_Use)),
		static_cast<int32>(EInteractionResult::Failed));
	Interactable->bIsEnabled = true;
	AActor* Far = nullptr;
	UInteractableComponent* FarInteractable = nullptr;
	SpawnInteractable(FVector(Interaction->InteractionRange * 3.0, 0.0, 0.0), true, Far, FarInteractable, Callers);
	TestEqual(TEXT("An interactable out of range is Failed"), static_cast<int32>(Interaction->TryInteractWith(Far, CGFGameplayTags::Interaction_Type_Use)),
		static_cast<int32>(EInteractionResult::Failed));
	TestEqual(TEXT("No handler ran for the refusals"), Callers.Num(), 1);

	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
