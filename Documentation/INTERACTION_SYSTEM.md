# Interaction System

> State as of 2026-11-02 (code review CR5).

## Overview

The design of InteractionPlugin: detection and scoring, the interaction component's instant and channeled flows, the
interactable component, the world item actor and its pool, and server validation. How the plugin fits with
ItemInventoryPlugin and EquipmentPlugin is in `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md`. Module
`InteractionPlugin`, log category `LogIAP`.

---

## Detection System

### Detection Loop

`UInteractionComponent` does not detect every frame. A timer every `DetectionTickRate` (0.1 s) runs `TickDetection()`:

```
1. DetectionStrategy->DetectInteractables(Owner, InteractionRange) → candidate actors
2. keep actors with an enabled UInteractableComponent whose CanInteract(Owner, Context) is true → NearbyInteractables
3. score each candidate (ScoreCandidate); the highest is the best target
4. when the best target changes: OnInteractableLost for the old one, OnInteractableFound for the new one
```

The best target's `OnOptionsChanged` is followed while it is the target, so a change of prompt text (a torch lit or put
out) re-announces it through `OnInteractableFound`.

**Detection runs only on the locally controlled pawn.** The server never detects; it checks requests when they arrive.
The timer starts and stops with local control, not with BeginPlay: the component binds the owning pawn's
`ReceiveControllerChangedDelegate` and re-evaluates `IsLocallyControlled()` at BeginPlay and on every possession change.
A respawned pawn is spawned (BeginPlay) before the game mode possesses it; gating only at BeginPlay would leave it with
no detection, so `TryInteract` would have no target. Losing local control clears the nearby list and broadcasts
`OnInteractableLost` for the current target.

### Scoring

```
Score = DistanceScore * DistanceWeight + AngleScore * AngleWeight + PriorityScore * PriorityWeight

DistanceScore = 1 - clamp(Distance / InteractionRange, 0, 1)        closer = higher
AngleScore    = dot(view forward, direction to the target)          -1..1; the camera's view for a player,
                                                                     else the actor's forward
PriorityScore = clamp(UInteractableComponent::InteractionPriority / 10, 0, 1)

Default weights: Distance 0.4, Angle 0.4, Priority 0.2
```

`ScoreCandidate` is a `BlueprintNativeEvent`: a game can override it (prefer quest objects regardless of distance).

### Detection Strategies

`UInteractionDetectionStrategy` is an abstract `EditInlineNew` object, instanced on the component and configurable per
actor; subclasses implement `DetectInteractables(SourceActor, InteractionRange, OutCandidates)`.

- **`USphereOverlapDetection`**: `OverlapMultiByChannel` with a sphere of `InteractionRange` around the source actor on
  `CollisionChannel` (`ECC_WorldDynamic`); every overlapped actor is a candidate. Overlaps see simple collision only, so
  an interactable needs a simple shape. Third-person use.
- **`ULineTraceDetection`**: a trace of `InteractionRange` from the player camera along its view (the actor's location
  and forward without a player controller) on `CollisionChannel` (`ECC_Visibility`); the first hit, or every hit with
  `bMultiTrace`. First-person use.

---

## Interaction Flow

### Instant Interaction

```
Interact input → UInteractionComponent::TryInteract(InteractionType)
    → TryInteractWith(CurrentBestTarget, Type)
        → ValidateInteraction: target valid, an enabled UInteractableComponent, within InteractionRange, CanInteract
        → client: ServerRPC_RequestInteract(Target, Type), returns InProgress
              server checks again (see Multiplayer), runs Interact, answers with ClientRPC_InteractionResult
              → OnInteractionCompleted (Success) or OnInteractionFailed on the client
        → server / standalone: OnInteractionStarted, UInteractableComponent::Interact(Owner, Type),
              OnInteractionCompleted or OnInteractionFailed, returns the result
```

### Channeled Interaction

For interactions that take time (an option with `bRequiresHold` and a `HoldDuration`):

```
StartChanneledInteraction(Target, Type, Duration):
    ValidateInteraction, Duration > 0
    client: also ServerRPC_StartChanneledInteraction (the server checks range and the interactable, starts its own channel)
    state Channeling, OnInteractionStarted

Tick while Channeling (TickComponent):
    cancel when the target is gone or disabled, the owner moved more than CancelMoveThreshold (50 uu)
    from where it started, or the target is beyond 1.1 x InteractionRange
    progress = elapsed / Duration → OnChanneledProgress
    at Duration: UInteractableComponent::Interact, OnInteractionCompleted or OnInteractionFailed, back to Idle

CancelChanneledInteraction(): client also sends ServerRPC_CancelChanneledInteraction; OnInteractionFailed(Cancelled)
```

Both the server and the owning client run the channel: the server's completion is the authoritative `Interact`; the
client's copy drives `OnChanneledProgress` and, today, also calls `Interact` on the client when its time is up, so
handlers that change state must check authority. Progress is not replicated. `bCancelOnDamage` is declared on the
component but nothing reads it.

---

## World Item Actor

### AWorldItem Lifecycle

```
POOLED (hidden, parked) → InitializeFromItem → ACTIVE (in the world) → picked up      → POOLED
                                                      └──────────→ despawn timeout → POOLED
```

**`InitializeFromItem(FItemInstance)`:**
1. Store the instance (`ItemInstance`, replicated).
2. Add the "Pick Up <name>" option (`Interaction.Type.Pickup`) to the `UInteractableComponent` and enable it.
3. Show the fallback cube at a quarter scale at once (items without a world mesh would otherwise be invisible pickups),
   collision on, physics off.
4. Look up the definition through `UItemDatabaseSubsystem`; when it has a `UItemFragment_WorldDisplay` mesh, request it
   asynchronously.
5. On load (`OnMeshLoaded`): set the mesh, `WorldMaterial` and `WorldScale`, apply `WorldRotation` as the resting pose
   (`ApplyRestingPose`) and `SnapToGround()`.

**Pickup (`OnPickupInteraction`, the component's handler):** find the interactor's inventory through
`ICGFInventoryInterface` (else a `UInventoryComponent` on the actor), `TryAddItem(ItemInstance)`; on success return the
actor through `UWorldItemPoolSubsystem::ReturnWorldItem`. On failure (inventory full) the item stays.

**`ResetForPool(ParkingLocation)`:** cancel the mesh load, hide, collision and physics off, clear the mesh, disable and
clear the interactable's options, clear the instance, detach, move to the parking location.

Never `LoadSynchronous` a world mesh in gameplay code: one mesh is a hitch, a loot burst of fifty is a freeze.

### On a Planet

With the pool's gravity centre (`UWorldItemPoolSubsystem::SetGravityCenter`, set by VoxelCharacterPlugin's game mode on a
planet) a spawned item stands upright on the direction from the centre, its requested heading kept;
`AWorldItem::SnapToGround` traces along local down and rests the mesh's lowest corner (from its local bounds) 1 uu over
the hit; and the resting pose (the placeholder's zero, the WorldDisplay fragment's `WorldRotation`) is applied on top of
that upright rotation by `AWorldItem::ApplyRestingPose`. The mesh component is the root, so its relative rotation is the
world rotation: setting the pose directly tipped every planet drop over (G22). Without a centre all of this is world-Z.
Test: `Interaction.WorldItem.SnapAlongGravity`.

---

## Object Pooling

### UWorldItemPoolSubsystem

`UWorldSubsystem`, `Subsystems/WorldItemPoolSubsystem.h`.

```
Configuration:
    InitialPoolSize = 50          pre-spawned from OnWorldBeginPlay
    PreWarmBatchSize = 10         actors spawned per pre-warm timer tick
    MaxPoolSize = 200             cap on pooled + active actors
    DespawnTimeout = 300 s        an active item returns to the pool after this (0 = never)
    bExpandPoolOnDemand = true    spawn a new actor when the pool is empty, up to MaxPoolSize
    ParkingLocation = (0, 0, -100,000)

API:
    AWorldItem* SpawnWorldItem(Item, Location, Rotation)   a pooled actor, placed and initialised
    AWorldItem* GetWorldItem()                             a pooled actor, or nullptr when exhausted
    void ReturnWorldItem(AWorldItem*)                      ResetForPool and back to the pool
    void ReturnAllWorldItems()                             every active item back (level cleanup)
    SetParkingLocation, SetGravityCenter / ClearGravityCenter
```

The despawn timer starts when an item becomes active and is cleared when it returns; a player walking toward an item does
not reset it.

Items placed by level designers are ordinary actors, not pooled. The pool is for runtime drops: loot bursts and
inventory drops.

---

## InteractableComponent Details

### Configuration Patterns

**Single interaction (an item pickup):**
```
InteractionOptions:
  [0] Type=Interaction.Type.Pickup, Text="Pick Up", Priority=0, bRequiresHold=false
```

**Several options (a container):**
```
InteractionOptions:
  [0] Type=Interaction.Type.Open, Text="Open", Priority=0, bRequiresHold=true, HoldDuration=1.0
  [1] Type=Interaction.Type.Inspect, Text="Inspect", Priority=1, bRequiresHold=false
```

**Context-dependent (a locked door):** override `GetInteractionOptions` in Blueprint, or bind the native
`OptionsModifier`, to show "Open" only to an interactor holding the key and "Locked" to everyone else. Call
`NotifyOptionsChanged()` when the state behind the text changes.

### Behaviour

`Interact(Interactor, Type)` runs the owner's `InteractionHandler` (bound in C++) and fires `OnInteractionTriggered`
for Blueprint; it runs where the flow executes, the server.

### Enable / Disable

`bIsEnabled` decides whether detection sees the component. It replicates and is `BlueprintReadOnly`: change it on the
server with `Enable()`, `Disable()` or `SetEnabled()` (`OnStatusChanged` fires). Use cases: a looted chest, an NPC in
combat, a door while it animates. Disabling fires `OnInteractableLost` on any player targeting it.

---

## Multiplayer Specifics

### Interaction Validation on the Server

```cpp
void UInteractionComponent::ServerRPC_RequestInteract_Implementation(AActor* TargetActor, FGameplayTag InteractionType)
{
    if (!IsValid(TargetActor)) return;

    // Range, with 10% tolerance for latency
    const float Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
    if (Distance > InteractionRange * 1.1f)
    {
        ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::OutOfRange);
        return;
    }

    UInteractableComponent* Interactable = GetInteractable(TargetActor);    // FindComponentByClass
    if (!Interactable || !Interactable->IsEnabled())
    {
        ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::Failed);
        return;
    }

    const FInteractionContext Context = MakeContext(TargetActor, InteractionType);
    if (!Interactable->CanInteract(GetOwner(), Context))
    {
        ClientRPC_InteractionResult(TargetActor, InteractionType, EInteractionResult::NotAllowed);
        return;
    }

    ClientRPC_InteractionResult(TargetActor, InteractionType, Interactable->Interact(GetOwner(), InteractionType));
}
```

The client runs the same checks before it sends (`ValidateInteraction`, without the tolerance), so most refusals never
reach the network; the server repeats them inline (code review decision D-CR16). The 10% range tolerance absorbs the
client being slightly ahead of where the server sees it.

### World Item Replication

`AWorldItem` replicates (`bReplicates`, not always relevant) and its `ItemInstance` is a replicated property. Clients
see the item vanish when the server returns it to the pool and their inventory update through the
`FFastArraySerializer`; the pickup itself happens only on the server.
