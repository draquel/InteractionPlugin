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
#include "Subsystems/CGFWaterSubsystem.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AWorldItem::AWorldItem()
{
	bReplicates = true;
	bAlwaysRelevant = false;
	// Ticks only while it floats, to draw the bob (UpdateFloatTick).
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

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
	DOREPLIFETIME(AWorldItem, Rest);
}

// ===========================================================================
// Lifecycle
// ===========================================================================

void AWorldItem::InitializeFromItem(const FItemInstance& Item)
{
	ItemInstance = Item;
	PoseRotation = GetActorQuat();
	if (bHasGravityCenter)
	{
		// The pool has just stood the actor upright on local up (UWorldItemPoolSubsystem::SpawnWorldItem): the frame the
		// resting poses below are applied in.
		UprightRotation = GetActorQuat();
	}

	if (UItemDefinition* Def = ShowItem())
	{
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
	}

	// Where it stands until it settles (SnapToGround publishes again).
	PublishRest(false, GetSettleUp());
}

UItemDefinition* AWorldItem::ShowItem()
{
	UItemDatabaseSubsystem* DB = GetItemDatabase();
	UItemDefinition* Def = DB ? DB->GetDefinition(ItemInstance.ItemDefinitionId) : nullptr;
	if (!Def)
	{
		return nullptr;
	}

	// A pooled actor's earlier load must not land on this item.
	if (MeshLoadHandle.IsValid())
	{
		MeshLoadHandle->CancelHandle();
		MeshLoadHandle.Reset();
	}

	// Show the placeholder immediately and unhide the ACTOR — ResetForPool hides at the actor
	// level, and only OnMeshLoaded cleared it, so items whose definition has no WorldDisplay
	// mesh (or whose load was still in flight) stayed invisible: present and interactable but
	// impossible to find. The async load below swaps in the real mesh when there is one.
	if (FallbackMesh)
	{
		MeshComponent->SetStaticMesh(FallbackMesh);
		MeshComponent->SetWorldScale3D(FVector(0.25f));
		if (HasAuthority())
		{
			// The pose is the server's: a client's copy takes its rotation from Rest.
			ApplyRestingPose(FRotator::ZeroRotator);
		}
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
	return Def;
}

void AWorldItem::HideItem()
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
	MeshComponent->SetStaticMesh(nullptr);
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

	if (!HasAuthority())
	{
		// A client's copy: the mesh and its scale are its own to build; the place and the pose are the server's (Rest).
		return;
	}

	// The lying pose: a mesh authored upright (a sword, blade +Z) rests flat on the ground.
	ApplyRestingPose(DisplayFrag->WorldRotation);

	// The real mesh's bounds differ from the placeholder's: settle again so it lies on the ground or at the water's
	// surface, not in or over it. Either way the new pose reaches the clients.
	if (bSnapToGround)
	{
		SnapToGround();
	}
	else
	{
		PublishRest(false, GetSettleUp());
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
	PoseRotation = GetActorQuat();
}

FVector AWorldItem::GetSettleUp() const
{
	return bHasGravityCenter ? (GetActorLocation() - GravityCenter).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector) : FVector::UpVector;
}

void AWorldItem::GetExtentAlong(const FVector& Up, double& OutLowest, double& OutHighest) const
{
	// The mesh's local box, corner by corner: the item may lie in any pose, and up may be any direction.
	const FBox LocalBox = MeshComponent->CalcLocalBounds().GetBox();
	const FTransform& MeshTransform = MeshComponent->GetComponentTransform();
	const FVector Location = GetActorLocation();
	OutLowest = TNumericLimits<double>::Max();
	OutHighest = TNumericLimits<double>::Lowest();
	for (int32 Corner = 0; Corner < 8; ++Corner)
	{
		const FVector LocalCorner((Corner & 1) ? LocalBox.Max.X : LocalBox.Min.X, (Corner & 2) ? LocalBox.Max.Y : LocalBox.Min.Y,
			(Corner & 4) ? LocalBox.Max.Z : LocalBox.Min.Z);
		const double Along = FVector::DotProduct(MeshTransform.TransformPosition(LocalCorner) - Location, Up);
		OutLowest = FMath::Min(OutLowest, Along);
		OutHighest = FMath::Max(OutHighest, Along);
	}
}

bool AWorldItem::SnapToGround()
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || !MeshComponent)
	{
		return false;
	}

	// A floating item is drawn bobbing and rocking about its resting place on a machine with a view: settle from the
	// place and the pose themselves, or the drawn offset would become the next resting place.
	if (IsFloating())
	{
		SetActorLocationAndRotation(Rest.Location, PoseRotation);
	}

	// Start just above the item so a drop in a low room never lands on the ceiling's far side; search a few metres
	// down (drops spawn near a pawn's centre or a creature's body, well above the floor). Along local down from the
	// gravity centre on a planet, world -Z otherwise.
	constexpr float TraceUp = 50.0f;
	constexpr float TraceDown = 500.0f;
	const FVector Location = GetActorLocation();
	const FVector Up = GetSettleUp();
	FCollisionQueryParams Params(SCENE_QUERY_STAT(WorldItemSnapToGround), false, this);
	const FCollisionObjectQueryParams Ground(ECC_WorldStatic);
	FHitResult Hit;
	const bool bGround = World->LineTraceSingleByObjectType(Hit, Location + Up * TraceUp, Location - Up * TraceDown, Ground, Params);

	// Water first: an item in the water, or dropped from within the trace's reach over it, rests at the surface
	// straight over where it is, not on the bed. The world's water is the game layer's to say (UCGFWaterSubsystem);
	// with no provider there is none.
	if (bFloatOnWater)
	{
		const UCGFWaterSubsystem* Water = World->GetSubsystem<UCGFWaterSubsystem>();
		FCGFWaterSurfaceSample Sample;
		if (Water && Water->SampleWaterSurface(Location, TraceDown, Sample) && Sample.bWater)
		{
			const FVector WaterUp = Sample.Up.GetSafeNormal(UE_SMALL_NUMBER, Up);
			// Ground between the item and the surface (a pier, a rock standing out of the water) or water too shallow to
			// float in: the ground has it.
			bool bFloat = !bGround || FVector::DotProduct(Sample.SurfacePoint - Hit.ImpactPoint, WaterUp) >= MinFloatWaterDepth;
			if (bFloat && Sample.Depth > 0.0)
			{
				// Under the water: nothing solid may lie between the item and the surface over it (a flooded cave's roof).
				FHitResult Roof;
				bFloat = !World->LineTraceSingleByObjectType(Roof, Location, Sample.SurfacePoint, Ground, Params);
			}
			if (bFloat)
			{
				double Lowest = 0.0;
				double Highest = 0.0;
				GetExtentAlong(WaterUp, Lowest, Highest);
				SetActorLocation(Sample.SurfacePoint - WaterUp * (Lowest + FloatSubmersion * (Highest - Lowest)));
				PublishRest(true, WaterUp);
				return true;
			}
		}
	}

	if (!bGround)
	{
		PublishRest(false, Up);
		return false;
	}

	// The mesh's lowest corner along up rests a unit over the hit.
	double Lowest = 0.0;
	double Highest = 0.0;
	GetExtentAlong(Up, Lowest, Highest);
	SetActorLocation(Hit.ImpactPoint + Up * (1.0 - Lowest));
	PublishRest(false, Up);
	return true;
}

// ===========================================================================
// The resting place and the bob
// ===========================================================================

void AWorldItem::PublishRest(bool bFloating, const FVector& Up)
{
	if (!HasAuthority())
	{
		return;
	}
	Rest.bValid = true;
	Rest.bFloating = bFloating;
	Rest.Location = GetActorLocation();
	Rest.Rotation = GetActorRotation();
	Rest.Up = Up;
	UpdateFloatTick();
}

void AWorldItem::OnRep_ItemInstance()
{
	// A client's copy shows nothing of its own accord: the server's mesh component does not replicate.
	if (ItemInstance.IsValid())
	{
		ShowItem();
	}
	else
	{
		HideItem();
	}
}

void AWorldItem::OnRep_Rest()
{
	if (Rest.bValid)
	{
		SetActorLocationAndRotation(Rest.Location, Rest.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	}
	UpdateFloatTick();
}

void AWorldItem::UpdateFloatTick()
{
	// The bob is something to look at: a dedicated server draws nothing and keeps the item at its resting place.
	const bool bSeen = IsFloating() && GetNetMode() != NM_DedicatedServer;
	SetActorTickEnabled(bSeen && (FloatBobAmplitude != 0.0f || FloatRockDegrees != 0.0f));
	if (MeshComponent && MeshComponent->bRenderCustomDepth != (bSeen && bCustomDepthWhileFloating))
	{
		MeshComponent->SetRenderCustomDepth(bSeen && bCustomDepthWhileFloating);
	}
}

void AWorldItem::GetFloatingPose(double TimeSeconds, FVector& OutLocation, FQuat& OutRotation) const
{
	OutLocation = Rest.Location;
	OutRotation = Rest.Rotation.Quaternion();
	if (!IsFloating())
	{
		return;
	}
	// A phase from the resting place, so items dropped together do not rise and fall as one.
	const double Phase = FMath::Frac(Rest.Location.X * 0.0131 + Rest.Location.Y * 0.0073 + Rest.Location.Z * 0.0117) * UE_DOUBLE_TWO_PI;
	const double Angle = UE_DOUBLE_TWO_PI * TimeSeconds / FMath::Max(static_cast<double>(FloatBobPeriod), 0.1) + Phase;
	const FVector Up = Rest.Up;
	OutLocation += Up * (static_cast<double>(FloatBobAmplitude) * FMath::Sin(Angle));
	if (FloatRockDegrees != 0.0f)
	{
		// A rock about a level axis, a little slower than the bob so the two never lock into one motion.
		const FVector Axis = FVector::CrossProduct(Up, FMath::Abs(Up.X) < 0.9 ? FVector::ForwardVector : FVector::RightVector).GetSafeNormal();
		OutRotation = FQuat(Axis, FMath::DegreesToRadians(static_cast<double>(FloatRockDegrees)) * FMath::Sin(Angle * 0.73 + 1.3)) * OutRotation;
	}
}

void AWorldItem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const UWorld* World = GetWorld();
	if (!World || !IsFloating())
	{
		UpdateFloatTick();
		return;
	}
	// Nothing to draw for an item nobody has seen lately: it waits at whatever point of the bob it had.
	if (!WasRecentlyRendered(0.5f))
	{
		return;
	}
	FVector Location;
	FQuat Rotation;
	GetFloatingPose(World->GetTimeSeconds(), Location, Rotation);
	// The authority's copy moves too (a listen server's host sees the bob); Rest, not the transform, is what replicates.
	SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
}

const FVector AWorldItem::DefaultParkingLocation(0.0, 0.0, -100000.0);

void AWorldItem::ResetForPool(const FVector& ParkingLocation)
{
	HideItem();
	MeshComponent->SetSimulatePhysics(false);

	InteractableComponent->Disable();
	InteractableComponent->InteractionOptions.Reset();

	ItemInstance = FItemInstance();
	// No resting place while pooled: a client's copy stops bobbing and waits for the next settle.
	Rest = FWorldItemRest();
	UpdateFloatTick();

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
