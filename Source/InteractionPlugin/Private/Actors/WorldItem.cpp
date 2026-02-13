#include "Actors/WorldItem.h"
#include "Components/InteractableComponent.h"
#include "Components/InventoryComponent.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_WorldDisplay.h"
#include "Interfaces/CGFInventoryInterface.h"
#include "Tags/CGFGameplayTags.h"
#include "Engine/AssetManager.h"
#include "Components/StaticMeshComponent.h"
#include "Net/UnrealNetwork.h"

AWorldItem::AWorldItem()
{
	bReplicates = true;
	bAlwaysRelevant = false;

	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MeshComponent"));
	RootComponent = MeshComponent;
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionObjectType(ECC_WorldDynamic);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
	MeshComponent->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
	MeshComponent->SetGenerateOverlapEvents(true);
	MeshComponent->SetSimulatePhysics(false);
	MeshComponent->SetVisibility(false);

	InteractableComponent = CreateDefaultSubobject<UInteractableComponent>(TEXT("InteractableComponent"));
}

void AWorldItem::BeginPlay()
{
	Super::BeginPlay();

	// Wire up the pickup handler
	InteractableComponent->InteractionHandler =
		[this](AActor* Interactor, FGameplayTag Type) -> EInteractionResult
		{
			return OnPickupInteraction(Interactor, Type);
		};
}

void AWorldItem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWorldItem, ItemInstance);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void AWorldItem::InitializeFromItem(const FItemInstance& Item)
{
	ItemInstance = Item;

	UItemDatabaseSubsystem* DB = GetItemDatabase();
	if (!DB)
	{
		return;
	}

	UItemDefinition* Def = DB->GetDefinition(Item.ItemDefinitionId);
	if (!Def)
	{
		return;
	}

	// Configure interaction options
	FInteractionOption PickupOption;
	PickupOption.InteractionType = CGFGameplayTags::Interaction_Type_Pickup;
	PickupOption.DisplayText = FText::Format(
		NSLOCTEXT("WorldItem", "PickupFormat", "Pick Up {0}"),
		Def->DisplayName);
	PickupOption.Priority = 0;
	PickupOption.bRequiresHold = false;
	InteractableComponent->InteractionOptions.Reset();
	InteractableComponent->InteractionOptions.Add(PickupOption);
	InteractableComponent->Enable();

	// Async load mesh from WorldDisplay fragment
	UItemFragment_WorldDisplay* DisplayFrag = Def->FindFragment<UItemFragment_WorldDisplay>();
	if (DisplayFrag && !DisplayFrag->WorldMesh.IsNull())
	{
		FStreamableManager& Manager = UAssetManager::GetStreamableManager();
		MeshLoadHandle = Manager.RequestAsyncLoad(
			DisplayFrag->WorldMesh.ToSoftObjectPath(),
			FStreamableDelegate::CreateUObject(this, &AWorldItem::OnMeshLoaded)
		);
	}
	else
	{
		// No mesh to load — just make visible
		MeshComponent->SetVisibility(true);
	}

	// Enable collision for overlap detection (no physics — items stay where spawned)
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetSimulatePhysics(false);
}

void AWorldItem::OnMeshLoaded()
{
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	if (!DB)
	{
		return;
	}

	UItemDefinition* Def = DB->GetDefinition(ItemInstance.ItemDefinitionId);
	if (!Def)
	{
		return;
	}

	UItemFragment_WorldDisplay* DisplayFrag = Def->FindFragment<UItemFragment_WorldDisplay>();
	if (!DisplayFrag)
	{
		return;
	}

	UStaticMesh* Mesh = DisplayFrag->WorldMesh.Get();
	if (Mesh)
	{
		MeshComponent->SetStaticMesh(Mesh);
	}

	if (!DisplayFrag->WorldMaterial.IsNull())
	{
		UMaterialInterface* Mat = DisplayFrag->WorldMaterial.Get();
		if (Mat)
		{
			MeshComponent->SetMaterial(0, Mat);
		}
	}

	MeshComponent->SetWorldScale3D(DisplayFrag->WorldScale);
	SetActorHiddenInGame(false);
	MeshComponent->SetVisibility(true);
}

void AWorldItem::ResetForPool()
{
	// Cancel any pending async load
	if (MeshLoadHandle.IsValid())
	{
		MeshLoadHandle->CancelHandle();
		MeshLoadHandle.Reset();
	}

	SetActorHiddenInGame(true);
	MeshComponent->SetVisibility(false);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetSimulatePhysics(false);
	MeshComponent->SetStaticMesh(nullptr);

	InteractableComponent->Disable();
	InteractableComponent->InteractionOptions.Reset();

	ItemInstance = FItemInstance();

	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	SetActorLocation(FVector(0.0f, 0.0f, -100000.0f));
}

// ===========================================================================
// Pickup
// ===========================================================================

EInteractionResult AWorldItem::OnPickupInteraction(AActor* Interactor, FGameplayTag InteractionType)
{
	if (!Interactor || !ItemInstance.IsValid())
	{
		return EInteractionResult::Failed;
	}

	// Get inventory from interactor via IInventoryOwner interface
	UInventoryComponent* Inventory = nullptr;

	if (Interactor->Implements<UCGFInventoryInterface>())
	{
		UActorComponent* Comp = ICGFInventoryInterface::Execute_GetInventoryComponent(Interactor);
		Inventory = Cast<UInventoryComponent>(Comp);
	}

	// Fallback: try finding InventoryComponent directly
	if (!Inventory)
	{
		Inventory = Interactor->FindComponentByClass<UInventoryComponent>();
	}

	if (!Inventory)
	{
		return EInteractionResult::Failed;
	}

	EInventoryOperationResult AddResult = Inventory->TryAddItem(ItemInstance);
	if (AddResult == EInventoryOperationResult::Success)
	{
		// Return to pool — the pool subsystem will handle this
		ResetForPool();
		return EInteractionResult::Success;
	}

	// Inventory full or other failure — remain in world
	return EInteractionResult::Failed;
}

// ===========================================================================
// Helpers
// ===========================================================================

UItemDatabaseSubsystem* AWorldItem::GetItemDatabase() const
{
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UItemDatabaseSubsystem>() : nullptr;
}
