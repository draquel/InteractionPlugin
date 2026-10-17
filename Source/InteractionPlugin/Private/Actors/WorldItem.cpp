#include "Actors/WorldItem.h"
#include "Components/InteractableComponent.h"
#include "Components/InventoryComponent.h"
#include "Subsystems/ItemDatabaseSubsystem.h"
#include "Data/ItemDefinition.h"
#include "Data/Fragments/ItemFragment_WorldDisplay.h"
#include "Interfaces/CGFInventoryInterface.h"
#include "Tags/CGFGameplayTags.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Subsystems/WorldItemPoolSubsystem.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

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

	// Placeholder shown until (or instead of) the definition's WorldDisplay mesh: items whose
	// definition ships no world mesh were spawning as INVISIBLE pickups — present, interactable,
	// impossible to find.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		FallbackMesh = CubeMesh.Object;
	}

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
	if (bHasGravityCenter)
	{
		// The pool has just stood the actor upright on local up (UWorldItemPoolSubsystem::SpawnWorldItem): the frame the
		// resting poses below are applied in.
		UprightRotation = GetActorQuat();
	}

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

	// Show the placeholder immediately and unhide the ACTOR — ResetForPool hides at the actor
	// level, and only OnMeshLoaded cleared it, so items whose definition has no WorldDisplay
	// mesh (or whose load was still in flight) stayed invisible: present and interactable but
	// impossible to find. The async load below swaps in the real mesh when there is one.
	if (FallbackMesh)
	{
		MeshComponent->SetStaticMesh(FallbackMesh);
		MeshComponent->SetWorldScale3D(FVector(0.25f));
		ApplyRestingPose(FRotator::ZeroRotator);
	}
	SetActorHiddenInGame(false);
	MeshComponent->SetVisibility(true);

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
	// The lying pose: a mesh authored upright (a sword, blade +Z) rests flat on the ground.
	ApplyRestingPose(DisplayFrag->WorldRotation);
	SetActorHiddenInGame(false);
	MeshComponent->SetVisibility(true);

	// The real mesh's bounds differ from the placeholder's: settle again so it lies on the ground, not in or over it.
	if (bSnapToGround)
	{
		SnapToGround();
	}
}

void AWorldItem::ApplyRestingPose(const FRotator& Pose)
{
	if (!MeshComponent)
	{
		return;
	}
	if (bHasGravityCenter)
	{
		MeshComponent->SetWorldRotation(UprightRotation * Pose.Quaternion());
	}
	else
	{
		MeshComponent->SetRelativeRotation(Pose);
	}
}

bool AWorldItem::SnapToGround()
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || !MeshComponent)
	{
		return false;
	}

	// Start just above the item so a drop in a low room never lands on the ceiling's far side; search a few metres
	// down (drops spawn near a pawn's centre or a creature's body, well above the floor).
	constexpr float TraceUp = 50.0f;
	constexpr float TraceDown = 500.0f;
	const FVector Location = GetActorLocation();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WorldItemSnapToGround), false, this);
	FHitResult Hit;
	if (bHasGravityCenter)
	{
		// Along local down from the gravity centre; the mesh's lowest corner along local up rests on the hit.
		const FVector Up = (Location - GravityCenter).GetSafeNormal();
		if (!World->LineTraceSingleByObjectType(Hit, Location + Up * TraceUp, Location - Up * TraceDown, FCollisionObjectQueryParams(ECC_WorldStatic), Params))
		{
			return false;
		}
		const FBox LocalBox = MeshComponent->CalcLocalBounds().GetBox();
		const FTransform& MeshTransform = MeshComponent->GetComponentTransform();
		double Lowest = TNumericLimits<double>::Max();
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector LocalCorner((Corner & 1) ? LocalBox.Max.X : LocalBox.Min.X, (Corner & 2) ? LocalBox.Max.Y : LocalBox.Min.Y,
				(Corner & 4) ? LocalBox.Max.Z : LocalBox.Min.Z);
			Lowest = FMath::Min(Lowest, FVector::DotProduct(MeshTransform.TransformPosition(LocalCorner) - Location, Up));
		}
		SetActorLocation(Hit.ImpactPoint + Up * (-Lowest + 1.0));
		return true;
	}
	if (!World->LineTraceSingleByObjectType(Hit, Location + FVector(0.0f, 0.0f, TraceUp), Location - FVector(0.0f, 0.0f, TraceDown),
		FCollisionObjectQueryParams(ECC_WorldStatic), Params))
	{
		return false;
	}

	MeshComponent->UpdateBounds();
	const FBoxSphereBounds& Bounds = MeshComponent->Bounds;
	const float PivotAboveBottom = Location.Z - (Bounds.Origin.Z - Bounds.BoxExtent.Z);
	SetActorLocation(FVector(Location.X, Location.Y, Hit.ImpactPoint.Z + PivotAboveBottom + 1.0f));
	return true;
}

const FVector AWorldItem::DefaultParkingLocation(0.0, 0.0, -100000.0);

void AWorldItem::ResetForPool(const FVector& ParkingLocation)
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
	SetActorLocation(ParkingLocation);
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
		// Hand the actor back to the pool. Only resetting it left it counted as active until its despawn timer
		// (300 s), so heavy looting could exhaust the pool and later drops were lost.
		UWorldItemPoolSubsystem* Pool = GetWorld() ? GetWorld()->GetSubsystem<UWorldItemPoolSubsystem>() : nullptr;
		if (Pool)
		{
			Pool->ReturnWorldItem(this);
		}
		else
		{
			ResetForPool();
		}
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
