// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Actors/WorldItem.h"
#include "Subsystems/WorldItemPoolSubsystem.h"
#include "Subsystems/CGFWaterSubsystem.h"
#include "Testing/CGFTestWaterSurface.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// A world item in water (the water plan's W2B, D129 and D130). The framework knows no world: the water is whatever the
// provider registered with UCGFWaterSubsystem says, here a plane and then a sphere (UCGFTestWaterSurface). An item
// that settles in the water, or from within the trace's reach over it, rests at the surface straight over where it was
// instead of on the bed, and its resting place is recorded for the clients (Rest). The ground keeps the item where it
// lies between the item and the surface (a pier), where the water over it is too shallow, where something solid lies
// between the item and the surface (a flooded cave's roof), and where the provider says the place holds no water (a
// dry cave under the level). With no provider nothing floats. The bob is drawn about the resting place and never
// moves it; a change of the water settles the items in its bounds again.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldItemFloatsOnWaterTest,
	"Interaction.WorldItem.FloatsOnWater",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldItemFloatsOnWaterTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("The engine's cube mesh"), Cube))
	{
		return true;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WorldItemFloatTest"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	// A slab whose top is at Z TopZ, 2,000 uu square, around (X, Y).
	auto SpawnSlab = [World, Cube](double X, double Y, double TopZ, double Thickness = 100.0)
	{
		AStaticMeshActor* Slab = World->SpawnActor<AStaticMeshActor>(FVector(X, Y, TopZ - Thickness * 0.5), FRotator::ZeroRotator);
		Slab->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Slab->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Slab->SetActorScale3D(FVector(20.0, 20.0, Thickness / 100.0));
		Slab->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
		return Slab;
	};

	UWorldItemPoolSubsystem* Pool = World->GetSubsystem<UWorldItemPoolSubsystem>();
	UCGFWaterSubsystem* Water = World->GetSubsystem<UCGFWaterSubsystem>();
	if (TestNotNull(TEXT("The world has a pool"), Pool) && TestNotNull(TEXT("The world has a water subsystem"), Water))
	{
		UCGFTestWaterSurface* Sea = NewObject<UCGFTestWaterSurface>(World);
		Sea->Level = 0.0;

		// No provider: nothing floats, and an item with no ground under it stays where it was dropped, as before.
		AWorldItem* Dry = Pool->SpawnWorldItem(FItemInstance(), FVector(0.0, 0.0, -300.0));
		if (TestNotNull(TEXT("An item from the pool"), Dry))
		{
			TestFalse(TEXT("No provider: not floating"), Dry->IsFloating());
			TestTrue(TEXT("No provider: where it was dropped"), Dry->GetActorLocation().Equals(FVector(0.0, 0.0, -300.0), 0.01));
			TestTrue(TEXT("No provider: the resting place is recorded all the same"), Dry->Rest.bValid && Dry->Rest.Location.Equals(Dry->GetActorLocation(), 0.01));
			Pool->ReturnWorldItem(Dry);
			TestFalse(TEXT("Back in the pool: no resting place"), Dry->Rest.bValid);
		}

		Water->SetProvider(Sea);
		TestTrue(TEXT("The provider is registered"), Water->HasProvider());

		// Under the water, no bed in reach: at the surface straight over the drop.
		AWorldItem* Deep = Pool->SpawnWorldItem(FItemInstance(), FVector(100.0, 200.0, -300.0), FRotator(0.0, 30.0, 0.0));
		if (TestNotNull(TEXT("An item from the pool"), Deep))
		{
			AddInfo(FString::Printf(TEXT("Dropped 300 under: rests at %s"), *Deep->GetActorLocation().ToCompactString()));
			TestTrue(TEXT("Under the water: floating"), Deep->IsFloating());
			TestTrue(TEXT("Under the water: at the surface over the drop"), Deep->GetActorLocation().Equals(FVector(100.0, 200.0, 0.0), 0.01));
			TestTrue(TEXT("Under the water: the rest is the place, the pose and the up"), Deep->Rest.Location.Equals(Deep->GetActorLocation(), 0.01)
				&& Deep->Rest.Rotation.Equals(FRotator(0.0, 30.0, 0.0), 0.01) && FVector(Deep->Rest.Up).Equals(FVector::UpVector, 1.0e-3));
			TestTrue(TEXT("A floating item ticks to draw its bob (a machine with a view)"), Deep->IsActorTickEnabled());

			// With a mesh the item lies in the water: FloatSubmersion of its height under the surface.
			Deep->MeshComponent->SetStaticMesh(Cube); // 100 uu
			Deep->SnapToGround();
			Deep->MeshComponent->UpdateBounds();
			const double Bottom = Deep->MeshComponent->Bounds.Origin.Z - Deep->MeshComponent->Bounds.BoxExtent.Z;
			const double Top = Deep->MeshComponent->Bounds.Origin.Z + Deep->MeshComponent->Bounds.BoxExtent.Z;
			AddInfo(FString::Printf(TEXT("A 100 uu cube: bottom %.2f, top %.2f"), Bottom, Top));
			TestTrue(TEXT("A cube: 0.6 of it under the surface"), FMath::Abs(Bottom + 60.0) < 0.5 && FMath::Abs(Top - 40.0) < 0.5);

			// The bob: about the resting place along the up, within the amplitude; the resting place does not move.
			const FVector RestLocation = Deep->Rest.Location;
			double MaxOffset = 0.0;
			double MaxSideways = 0.0;
			for (int32 Step = 0; Step < 64; ++Step)
			{
				FVector Location;
				FQuat Rotation;
				Deep->GetFloatingPose(Step * 0.11, Location, Rotation);
				MaxOffset = FMath::Max(MaxOffset, FMath::Abs(Location.Z - RestLocation.Z));
				MaxSideways = FMath::Max(MaxSideways, FVector2D(Location - RestLocation).Size());
			}
			AddInfo(FString::Printf(TEXT("The bob over 7 s: up to %.2f uu along the up, %.4f uu sideways"), MaxOffset, MaxSideways));
			TestTrue(TEXT("The bob reaches near its amplitude and no further"), MaxOffset > Deep->FloatBobAmplitude * 0.9 && MaxOffset <= Deep->FloatBobAmplitude + 1.0e-3);
			TestTrue(TEXT("The bob is along the up"), MaxSideways < 1.0e-3);
			Deep->Tick(0.1f);
			TestTrue(TEXT("Ticking never moves the resting place"), Deep->Rest.Location.Equals(RestLocation, 1.0e-6));

			// The water changes (D112): a notification settles the items in its bounds again, and no others.
			Sea->Level = 150.0;
			Water->NotifyWaterChanged(FBox(FVector(5000.0), FVector(6000.0)));
			TestTrue(TEXT("A change elsewhere: the item stays"), Deep->Rest.Location.Equals(RestLocation, 0.01));
			Water->NotifyWaterChanged(FBox(FVector(-1000.0), FVector(1000.0)));
			TestTrue(TEXT("A change here: the item is at the new surface"), FMath::Abs(Deep->Rest.Location.Z - (RestLocation.Z + 150.0)) < 0.01 && Deep->IsFloating());
			Sea->Level = 0.0;
			Water->NotifyWaterChanged(FBox(ForceInit));
			TestTrue(TEXT("A change everywhere: back at the surface"), Deep->Rest.Location.Equals(RestLocation, 0.01));
			// The item is drawn rocking between settles (the tick above): the drawn pose never becomes the resting one.
			TestTrue(TEXT("Settling again keeps the resting pose"), Deep->Rest.Rotation.Equals(FRotator(0.0, 30.0, 0.0), 0.01));

			Pool->ReturnWorldItem(Deep);
			TestFalse(TEXT("Back in the pool: not floating"), Deep->IsFloating());
			TestFalse(TEXT("Back in the pool: no tick"), Deep->IsActorTickEnabled());
		}

		// Over the water: within the trace's reach it falls to the surface, beyond it it stays (as over the ground).
		AWorldItem* Over = Pool->SpawnWorldItem(FItemInstance(), FVector(0.0, 0.0, 300.0));
		if (TestNotNull(TEXT("An item from the pool"), Over))
		{
			TestTrue(TEXT("300 over the water: floating at the surface"), Over->IsFloating() && Over->GetActorLocation().Equals(FVector(0.0, 0.0, 0.0), 0.01));
			Pool->ReturnWorldItem(Over);
		}
		AWorldItem* High = Pool->SpawnWorldItem(FItemInstance(), FVector(0.0, 0.0, 800.0));
		if (TestNotNull(TEXT("An item from the pool"), High))
		{
			TestTrue(TEXT("800 over the water: out of reach, it stays"), !High->IsFloating() && High->GetActorLocation().Equals(FVector(0.0, 0.0, 800.0), 0.01));
			Pool->ReturnWorldItem(High);
		}

		// The ground has it: a pier over the water, water too shallow to float in, a roof between the item and the surface.
		{
			AStaticMeshActor* Pier = SpawnSlab(10000.0, 0.0, 50.0);
			AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), FVector(10000.0, 0.0, 200.0));
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				AddInfo(FString::Printf(TEXT("Over a pier: rests at Z %.2f"), Item->GetActorLocation().Z));
				TestTrue(TEXT("A pier over the water: on the pier"), !Item->IsFloating() && FMath::Abs(Item->GetActorLocation().Z - 51.0) < 0.5);
				Pool->ReturnWorldItem(Item);
			}
			Pier->Destroy();
		}
		{
			AStaticMeshActor* Shoal = SpawnSlab(20000.0, 0.0, -10.0);
			AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), FVector(20000.0, 0.0, 100.0));
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				TestTrue(TEXT("10 uu of water: on the ground"), !Item->IsFloating() && FMath::Abs(Item->GetActorLocation().Z + 9.0) < 0.5);
				Pool->ReturnWorldItem(Item);
			}
			Shoal->Destroy();
			AStaticMeshActor* Bed = SpawnSlab(20000.0, 0.0, -100.0);
			Item = Pool->SpawnWorldItem(FItemInstance(), FVector(20000.0, 0.0, 100.0));
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				TestTrue(TEXT("100 uu of water: floating, not on the bed"), Item->IsFloating() && FMath::Abs(Item->GetActorLocation().Z) < 0.01);
				Pool->ReturnWorldItem(Item);
			}
			Bed->Destroy();
		}
		{
			AStaticMeshActor* Roof = SpawnSlab(30000.0, 0.0, -100.0);
			AStaticMeshActor* Floor = SpawnSlab(30000.0, 0.0, -400.0);
			AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), FVector(30000.0, 0.0, -300.0));
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				AddInfo(FString::Printf(TEXT("Under a roof: rests at Z %.2f"), Item->GetActorLocation().Z));
				TestTrue(TEXT("A roof between the item and the surface: on the floor under it"), !Item->IsFloating() && FMath::Abs(Item->GetActorLocation().Z + 399.0) < 0.5);
				Pool->ReturnWorldItem(Item);
			}
			Roof->Destroy();
			Floor->Destroy();
		}

		// A place under the level that holds no water (a dry cave): nothing floats there.
		{
			Sea->DryBounds.Add(FBox(FVector(39000.0, -1000.0, -1000.0), FVector(41000.0, 1000.0, -100.0)));
			AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), FVector(40000.0, 0.0, -300.0));
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				TestTrue(TEXT("A dry cave under the level: not floating, where it was dropped"), !Item->IsFloating() && Item->GetActorLocation().Equals(FVector(40000.0, 0.0, -300.0), 0.01));
				Pool->ReturnWorldItem(Item);
			}
			Sea->DryBounds.Reset();
		}

		// The item's own switch.
		{
			AWorldItem* Item = Pool->GetWorldItem();
			if (TestNotNull(TEXT("An item from the pool"), Item))
			{
				Item->bFloatOnWater = false;
				Item->SetActorLocation(FVector(0.0, 5000.0, -300.0));
				Item->InitializeFromItem(FItemInstance());
				Item->SnapToGround();
				TestTrue(TEXT("bFloatOnWater off: not floating"), !Item->IsFloating() && Item->GetActorLocation().Equals(FVector(0.0, 5000.0, -300.0), 0.01));
				Item->bFloatOnWater = true;
				Pool->ReturnWorldItem(Item);
			}
		}

		// A planet: the sea is a sphere, up is the radial, and the level is a radius.
		{
			const FVector Centre(200000.0, 0.0, 0.0);
			Sea->bRadial = true;
			Sea->Center = Centre;
			Sea->Level = 5000.0;
			Pool->SetGravityCenter(Centre);
			for (const FVector& Direction : { FVector(0.0, 0.0, 1.0), FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0), FVector(1, 1, 1).GetSafeNormal() })
			{
				for (const double Radius : { 4700.0, 5300.0 })
				{
					AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), Centre + Direction * Radius);
					if (!TestNotNull(TEXT("An item from the pool"), Item))
					{
						continue;
					}
					const FVector FromCentre = Item->GetActorLocation() - Centre;
					AddInfo(FString::Printf(TEXT("Planet, over %s from radius %.0f: rests at radius %.3f"), *Direction.ToCompactString(), Radius, FromCentre.Size()));
					TestTrue(TEXT("Planet: floating"), Item->IsFloating());
					TestTrue(TEXT("Planet: at the sea's radius"), FMath::Abs(FromCentre.Size() - 5000.0) < 0.01);
					TestTrue(TEXT("Planet: on the radial it was dropped on"), FVector::CrossProduct(FromCentre.GetSafeNormal(), Direction).Size() < 1.0e-6);
					TestTrue(TEXT("Planet: the rest's up is the radial"), FVector(Item->Rest.Up).Equals(Direction, 1.0e-3));
					FVector Location;
					FQuat Rotation;
					Item->GetFloatingPose(0.4, Location, Rotation);
					TestTrue(TEXT("Planet: the bob is along the radial"), FVector::CrossProduct((Location - Item->Rest.Location).GetSafeNormal(), Direction).Size() < 1.0e-3);
					Pool->ReturnWorldItem(Item);
				}
			}
			// 800 over the sphere: out of reach.
			AWorldItem* HighOver = Pool->SpawnWorldItem(FItemInstance(), Centre + FVector(0.0, 0.0, 5800.0));
			if (TestNotNull(TEXT("An item from the pool"), HighOver))
			{
				TestFalse(TEXT("Planet: 800 over the sea, out of reach"), HighOver->IsFloating());
				Pool->ReturnWorldItem(HighOver);
			}
			Pool->ClearGravityCenter();
		}

		// The provider goes: the water is gone.
		Sea->bRadial = false;
		Sea->Level = 0.0;
		AWorldItem* Last = Pool->SpawnWorldItem(FItemInstance(), FVector(0.0, -5000.0, -300.0));
		if (TestNotNull(TEXT("An item from the pool"), Last))
		{
			TestTrue(TEXT("Floating before the provider goes"), Last->IsFloating());
			Water->ClearProvider(Sea);
			TestFalse(TEXT("The provider is gone"), Water->HasProvider());
			TestFalse(TEXT("The provider gone: the item settled again and no longer floats"), Last->IsFloating());
			Pool->ReturnWorldItem(Last);
		}
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
