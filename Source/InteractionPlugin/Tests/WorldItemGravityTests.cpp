// Copyright Daniel Raquel. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Actors/WorldItem.h"
#include "Subsystems/WorldItemPoolSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// World items on a planet (VoxelWorlds SPHERICAL_PLANET_REQUIREMENTS.md SP-F7; spherical Phase 4, WM-SY16). With the
// pool's gravity centre an item dropped over a sphere stands upright on the radial and snaps along it, its lowest point
// on the ground, and its resting pose (the WorldDisplay fragment's lying rotation) turns it relative to that upright frame
// (the mesh is the root, so a relative rotation used to be the world rotation and tipped every planet drop over); without
// a centre it snaps along world -Z and the pose is the world rotation, as before.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWorldItemSnapAlongGravityTest,
	"InteractionPlugin.WorldItem.SnapAlongGravity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FWorldItemSnapAlongGravityTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("The engine's sphere mesh"), Sphere) || !TestNotNull(TEXT("The engine's cube mesh"), Cube))
	{
		return true;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("WorldItemGravityTest"));
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);

	auto SpawnGround = [World](UStaticMesh* Mesh, const FVector& Location, const FVector& Scale)
	{
		AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Ground->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable); // a static mesh can only be set at runtime on a movable component
		Ground->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Ground->SetActorScale3D(Scale);
		Ground->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
		return Ground;
	};
	auto Trace = [World](const FVector& From, const FVector& To, const AActor* Ignore, FHitResult& Hit)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(WorldItemGravityTest), false, Ignore);
		return World->LineTraceSingleByObjectType(Hit, From, To, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
	};
	auto LowestAlong = [](const AWorldItem* Item, const FVector& Up)
	{
		const FBox LocalBox = Item->MeshComponent->CalcLocalBounds().GetBox();
		double Lowest = TNumericLimits<double>::Max();
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector LocalCorner((Corner & 1) ? LocalBox.Max.X : LocalBox.Min.X, (Corner & 2) ? LocalBox.Max.Y : LocalBox.Min.Y,
				(Corner & 4) ? LocalBox.Max.Z : LocalBox.Min.Z);
			Lowest = FMath::Min(Lowest, FVector::DotProduct(Item->MeshComponent->GetComponentTransform().TransformPosition(LocalCorner), Up));
		}
		return Lowest;
	};

	UWorldItemPoolSubsystem* Pool = World->GetSubsystem<UWorldItemPoolSubsystem>();
	if (TestNotNull(TEXT("The world has a pool"), Pool))
	{
		// A planet: a sphere of radius 5,000 at the origin of its own.
		const FVector Centre(20000.0, 0.0, 0.0);
		SpawnGround(Sphere, Centre, FVector(100.0)); // the basic sphere is 100 uu across
		Pool->SetGravityCenter(Centre);
		const double C45 = FMath::Cos(FMath::DegreesToRadians(45.0));
		for (const FVector& Direction : { FVector(C45, 0.0, C45), FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0), FVector(1, 1, 1).GetSafeNormal() })
		{
			AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), Centre + Direction * 5300.0, FRotator(0.0, 30.0, 0.0));
			if (!TestNotNull(TEXT("An item from the pool"), Item))
			{
				continue;
			}
			const FVector Up = (Item->GetActorLocation() - Centre).GetSafeNormal();
			const double Tilt = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Item->GetActorUpVector(), Up), -1.0, 1.0)));
			FHitResult Hit;
			const bool bGround = Trace(Item->GetActorLocation() + Up * 50.0, Item->GetActorLocation() - Up * 500.0, Item, Hit);
			const double Gap = bGround ? LowestAlong(Item, Up) - FVector::DotProduct(Hit.ImpactPoint, Up) : -1.0;
			AddInfo(FString::Printf(TEXT("Over %s: %.3f deg off the radial, %.2f uu above the ground"), *Direction.ToCompactString(), Tilt, Gap));
			TestTrue(TEXT("Upright on the radial"), Tilt < 0.01);
			TestTrue(TEXT("Snapped along the radial (on the radial line)"), FVector::CrossProduct(Up, Direction).Size() < 1.0e-6);
			TestTrue(TEXT("Its lowest point a uu over the ground"), bGround && FMath::Abs(Gap - 1.0) < 0.5);

			// A lying pose (on its side): relative to the upright frame, so the forward stays on the tangent plane and the
			// item's own up lies across the radial.
			const FVector UprightForward = Item->GetActorForwardVector();
			Item->ApplyRestingPose(FRotator(0.0, 0.0, 90.0));
			AddInfo(FString::Printf(TEXT("  posed on its side: up . radial %.4f, forward kept %.4f"),
				FVector::DotProduct(Item->GetActorUpVector(), Up), FVector::DotProduct(Item->GetActorForwardVector(), UprightForward)));
			TestTrue(TEXT("The pose is relative to the upright frame (on its side across the radial)"),
				FMath::Abs(FVector::DotProduct(Item->GetActorUpVector(), Up)) < 1.0e-4);
			TestTrue(TEXT("The pose keeps the upright heading (a roll about the forward axis)"),
				FVector::DotProduct(Item->GetActorForwardVector(), UprightForward) > 1.0 - 1.0e-6);
			Pool->ReturnWorldItem(Item);
		}

		// A plane: world -Z onto a floor, unchanged.
		Pool->ClearGravityCenter();
		AStaticMeshActor* Floor = SpawnGround(Cube, FVector(-20000.0, 0.0, -50.0), FVector(20.0, 20.0, 1.0)); // top at Z 0
		AWorldItem* Item = Pool->SpawnWorldItem(FItemInstance(), FVector(-20000.0, 100.0, 300.0), FRotator(0.0, 30.0, 0.0));
		if (TestNotNull(TEXT("An item from the pool"), Item))
		{
			Item->MeshComponent->UpdateBounds();
			const double Bottom = Item->MeshComponent->Bounds.Origin.Z - Item->MeshComponent->Bounds.BoxExtent.Z;
			AddInfo(FString::Printf(TEXT("Over the floor: bounds bottom at Z %.2f, rotation %s"), Bottom, *Item->GetActorRotation().ToCompactString()));
			TestTrue(TEXT("Plane: the requested rotation kept"), Item->GetActorRotation().Equals(FRotator(0.0, 30.0, 0.0), 1.0e-3));
			TestTrue(TEXT("Plane: the bounds' bottom a uu over the floor"), FMath::Abs(Bottom - 1.0) < 0.5);
			Item->ApplyRestingPose(FRotator(0.0, 0.0, 90.0));
			TestTrue(TEXT("Plane: the pose is the world rotation, as before"), Item->GetActorRotation().Equals(FRotator(0.0, 0.0, 90.0), 1.0e-3));
		}
		Floor->Destroy();
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
