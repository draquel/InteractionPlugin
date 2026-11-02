#include "Misc/AutomationTest.h"
#include "Components/InteractableComponent.h"
#include "Components/InteractionComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Tags/CGFGameplayTags.h"
#include "Testing/CGFTestGameInstance.h"
#include "Types/CGFInteractionTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Channeled interactions complete on the server only (G92), simulated server/client in one standalone world: the server's
// interactor has authority, the client's is an actor with role AutonomousProxy at the same spot. The client's Server RPCs
// are absorbed (standalone), so the test delivers them to the server's component with CGFTesting::FRPCCall, and delivers
// the server's answer (ClientRPC_InteractionResult) to the client's the same way. Components are registered (a tick
// needs it) but play has not begun, so detection never runs; the test ticks each component itself.
// ---------------------------------------------------------------------------
namespace InteractionChanneledTestHelpers
{
	/** An actor with a root at Location, and the component T registered on it. */
	template <typename T>
	T* SpawnWith(UWorld* World, const FVector& Location, bool bRemote, AActor*& OutActor)
	{
		AActor* Actor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		Actor->SetActorLocation(Location);
		if (bRemote)
		{
			Actor->SetRole(ROLE_AutonomousProxy);
		}
		T* Component = NewObject<T>(Actor);
		Component->RegisterComponent();
		OutActor = Actor;
		return Component;
	}

	void Tick(UInteractionComponent* Component, float Seconds)
	{
		static_cast<UActorComponent*>(Component)->TickComponent(Seconds, LEVELTICK_All, nullptr);
	}

	bool StartOnServer(UInteractionComponent* Server, AActor* Target, FGameplayTag Type, float Duration)
	{
		return CGFTesting::FRPCCall(Server, TEXT("ServerRPC_StartChanneledInteraction"))
			.Set(TEXT("Target"), Target).Set(TEXT("InteractionType"), Type).Set(TEXT("Duration"), Duration).Send();
	}

	bool AnswerClient(UInteractionComponent* Client, AActor* Target, FGameplayTag Type, EInteractionResult Result)
	{
		return CGFTesting::FRPCCall(Client, TEXT("ClientRPC_InteractionResult"))
			.Set(TEXT("TargetActor"), Target).Set(TEXT("InteractionType"), Type).Set(TEXT("Result"), Result).Send();
	}

	int32 State(const UInteractionComponent* Component)
	{
		return static_cast<int32>(Component->ChanneledState);
	}

	struct FWorldScope
	{
		UWorld* World = nullptr;
		explicit FWorldScope(const TCHAR* Name)
		{
			World = UWorld::CreateWorld(EWorldType::Game, false, Name);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		}
		~FWorldScope()
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionChanneledServerCompletesTest,
	"Interaction.Channeled.ServerCompletes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInteractionChanneledServerCompletesTest::RunTest(const FString& Parameters)
{
	using namespace InteractionChanneledTestHelpers;
	FWorldScope Scope(TEXT("InteractionChanneledTest"));
	UWorld* World = Scope.World;
	const FGameplayTag Use = CGFGameplayTags::Interaction_Type_Use;

	// A target 300 uu away whose Use option is a 1 s hold; its handler records who ran it.
	AActor* Target = nullptr;
	UInteractableComponent* Interactable = SpawnWith<UInteractableComponent>(World, FVector(300.0, 0.0, 0.0), false, Target);
	FInteractionOption Hold;
	Hold.InteractionType = Use;
	Hold.bRequiresHold = true;
	Hold.HoldDuration = 1.0f;
	Interactable->InteractionOptions.Add(Hold);
	TArray<AActor*> Callers;
	Interactable->InteractionHandler = [&Callers](AActor* Caller, FGameplayTag) { Callers.Add(Caller); return EInteractionResult::Success; };

	AActor* ServerPlayer = nullptr;
	AActor* ClientPlayer = nullptr;
	UInteractionComponent* Server = SpawnWith<UInteractionComponent>(World, FVector::ZeroVector, false, ServerPlayer);
	UInteractionComponent* Client = SpawnWith<UInteractionComponent>(World, FVector::ZeroVector, true, ClientPlayer);

	// The client asks for 0.1 s: the option's 1 s hold is the floor on both machines.
	TestEqual(TEXT("The client's start is InProgress"), static_cast<int32>(Client->StartChanneledInteraction(Target, Use, 0.1f)),
		static_cast<int32>(EInteractionResult::InProgress));
	TestTrue(TEXT("The request reached the server"), StartOnServer(Server, Target, Use, 0.1f));
	TestEqual(TEXT("The server channels"), State(Server), static_cast<int32>(EChanneledState::Channeling));

	// The client's channel runs out first: it shows full progress and waits; Interact never runs on the client.
	Tick(Client, 0.6f);
	Tick(Client, 0.6f);
	TestEqual(TEXT("The client is still channeling after its time is up"), State(Client), static_cast<int32>(EChanneledState::Channeling));
	TestEqual(TEXT("The client shows full progress"), Client->ChanneledProgress, 1.0f);
	TestEqual(TEXT("No handler ran on the client"), Callers.Num(), 0);

	// The server ignores the client's 0.1 s: half way it has not completed; past the hold it runs the handler once.
	Tick(Server, 0.5f);
	TestEqual(TEXT("At 0.5 s of a 1 s hold the server has not completed"), Callers.Num(), 0);
	Tick(Server, 0.6f);
	TestEqual(TEXT("The server ran the handler once"), Callers.Num(), 1);
	TestTrue(TEXT("With the server's interactor"), Callers.Num() == 1 && Callers[0] == ServerPlayer);
	TestEqual(TEXT("The server is idle"), State(Server), static_cast<int32>(EChanneledState::Idle));

	// The server's answer ends the client's channel; the handler still ran once.
	TestTrue(TEXT("The answer reached the client"), AnswerClient(Client, Target, Use, EInteractionResult::Success));
	TestEqual(TEXT("The client is idle"), State(Client), static_cast<int32>(EChanneledState::Idle));
	TestEqual(TEXT("The client's progress is reset"), Client->ChanneledProgress, 0.0f);
	TestEqual(TEXT("The handler ran once in all"), Callers.Num(), 1);

	// The damage-cancel flag nothing read is gone (G92).
	TestNull(TEXT("UInteractionComponent has no bCancelOnDamage"), FindFProperty<FProperty>(UInteractionComponent::StaticClass(), TEXT("bCancelOnDamage")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInteractionChanneledRefusalsTest,
	"Interaction.Channeled.Refusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInteractionChanneledRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace InteractionChanneledTestHelpers;
	FWorldScope Scope(TEXT("InteractionChanneledRefusalsTest"));
	UWorld* World = Scope.World;
	const FGameplayTag Use = CGFGameplayTags::Interaction_Type_Use;
	const FGameplayTag Pickup = CGFGameplayTags::Interaction_Type_Pickup;

	AActor* Target = nullptr;
	UInteractableComponent* Interactable = SpawnWith<UInteractableComponent>(World, FVector(300.0, 0.0, 0.0), false, Target);
	FInteractionOption Option;
	Option.InteractionType = Use;
	Interactable->InteractionOptions.Add(Option);
	int32 Calls = 0;
	Interactable->InteractionHandler = [&Calls](AActor*, FGameplayTag) { ++Calls; return EInteractionResult::Success; };

	AActor* ServerPlayer = nullptr;
	AActor* ClientPlayer = nullptr;
	UInteractionComponent* Server = SpawnWith<UInteractionComponent>(World, FVector::ZeroVector, false, ServerPlayer);
	UInteractionComponent* Client = SpawnWith<UInteractionComponent>(World, FVector::ZeroVector, true, ClientPlayer);

	// The server checks what the client checked, and more: an option the target does not offer, a disabled target and
	// one out of range are refused there, with no channel.
	StartOnServer(Server, Target, Pickup, 1.0f);
	TestEqual(TEXT("An option the target does not offer starts no server channel"), State(Server), static_cast<int32>(EChanneledState::Idle));
	Interactable->bIsEnabled = false;
	StartOnServer(Server, Target, Use, 1.0f);
	TestEqual(TEXT("A disabled target starts no server channel"), State(Server), static_cast<int32>(EChanneledState::Idle));
	Interactable->bIsEnabled = true;
	ServerPlayer->SetActorLocation(FVector(-Server->InteractionRange, 0.0, 0.0));
	StartOnServer(Server, Target, Use, 1.0f);
	TestEqual(TEXT("A target out of range starts no server channel"), State(Server), static_cast<int32>(EChanneledState::Idle));
	ServerPlayer->SetActorLocation(FVector::ZeroVector);
	TestEqual(TEXT("The client refuses an option the target does not offer"), static_cast<int32>(Client->StartChanneledInteraction(Target, Pickup, 1.0f)),
		static_cast<int32>(EInteractionResult::Failed));

	// The server's refusal ends the progress the client shows.
	Client->StartChanneledInteraction(Target, Use, 1.0f);
	Tick(Client, 0.3f);
	AnswerClient(Client, Target, Use, EInteractionResult::NotAllowed);
	TestEqual(TEXT("A refusal ends the client's channel"), State(Client), static_cast<int32>(EChanneledState::Idle));

	// No answer at all: the client gives up after its result timeout, and still runs nothing.
	AddExpectedMessage(TEXT("no result from the server"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Client->StartChanneledInteraction(Target, Use, 1.0f);
	Tick(Client, 1.0f);
	Tick(Client, Client->ChannelResultTimeout * 0.5f);
	TestEqual(TEXT("Within the timeout the client still waits"), State(Client), static_cast<int32>(EChanneledState::Channeling));
	Tick(Client, Client->ChannelResultTimeout);
	TestEqual(TEXT("Past the timeout the client has cancelled"), State(Client), static_cast<int32>(EChanneledState::Idle));

	// The client cancels (it moved off): the cancel request stops the server's channel before it completes.
	TestTrue(TEXT("The server channels a valid start"), StartOnServer(Server, Target, Use, 1.0f) && Server->ChanneledState == EChanneledState::Channeling);
	Tick(Server, 0.5f);
	CGFTesting::FRPCCall(Server, TEXT("ServerRPC_CancelChanneledInteraction")).Send();
	TestEqual(TEXT("The cancel request stops the server's channel"), State(Server), static_cast<int32>(EChanneledState::Idle));
	Tick(Server, 1.0f);
	TestEqual(TEXT("No handler ran for any of it"), Calls, 0);

	// The server's own cancel rule: its interactor moving past the threshold ends its channel with no handler.
	StartOnServer(Server, Target, Use, 1.0f);
	ServerPlayer->SetActorLocation(FVector(0.0, Server->CancelMoveThreshold * 2.0, 0.0));
	Tick(Server, 2.0f);
	TestEqual(TEXT("Moving away cancels the server's channel"), State(Server), static_cast<int32>(EChanneledState::Idle));
	TestEqual(TEXT("Still no handler ran"), Calls, 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
