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
        → CheckInteraction: target valid, within InteractionRange, an enabled UInteractableComponent, CanInteract
        → client: ServerRPC_RequestInteract(Target, Type), returns InProgress
              server checks again (see Multiplayer), runs Interact, answers with ClientRPC_InteractionResult
              → OnInteractionCompleted (Success) or OnInteractionFailed on the client
        → server / standalone: OnInteractionStarted, UInteractableComponent::Interact(Owner, Type),
              OnInteractionCompleted or OnInteractionFailed, returns the result
```

### Channeled Interaction

For interactions that take time (an option with `bRequiresHold` and a `HoldDuration`):

```
StartChanneledInteraction(Target, Type, Duration) → EInteractionResult:
    CheckInteraction; the target must offer the option (the first one when Type is empty)
    length = Duration, never shorter than the option's HoldDuration when it has bRequiresHold (ResolveChannelDuration)
    client: also ServerRPC_StartChanneledInteraction; the server runs CheckInteraction (with the 1.1 range
        tolerance, CanInteract included) and ResolveChannelDuration itself and starts its own channel, or answers the
        refusal with ClientRPC_InteractionResult
    state Channeling (the tick is turned on), OnInteractionStarted; returns InProgress

Tick while Channeling (TickComponent), on both machines:
    cancel when the target is gone or disabled, the owner moved more than CancelMoveThreshold (50 uu)
    from where it started, or the target is beyond 1.1 x InteractionRange
    progress = elapsed / length → OnChanneledProgress
    at the length:
        server / standalone: UInteractableComponent::Interact, OnInteractionCompleted or OnInteractionFailed, back to
            Idle; a remote owner gets the result by ClientRPC_InteractionResult
        client: hold at full progress until the server's result arrives (it ends the channel and fires
            OnInteractionCompleted or OnInteractionFailed); after ChannelResultTimeout (2 s) more, cancel

CancelChanneledInteraction(): client also sends ServerRPC_CancelChanneledInteraction; OnInteractionFailed(Cancelled).
    A cancel on the server (the player moved, the target went away) tells a remote owner with a Cancelled result.
```

`Interact` runs on the server only (G92): the owning client's channel is a progress display that ends with the server's
answer, so an interaction handler never runs on a client. The length is the server's: a client asking for less than the
option's hold still waits the hold. Progress is not replicated; each machine counts its own. The component has no
damage cancel: the old `bCancelOnDamage` flag was never read, and the framework has no damage notification to wire it
to (damage lives in the game layer's combat component), so it was removed; a game that wants it calls
`CancelChanneledInteraction` from its own damage event on the server. Tests: `Interaction.Channeled.ServerCompletes` and
`Interaction.Channeled.Refusals` (simulated server/client in one world); over a real connection, the game module's
`VoxelEngine.Replication.Framework` (a dedicated server and a client in PIE: a held option the server completes after
its hold with its copy of the player, and `ClientRPC_InteractionResult` ending the client's channel), run by the
in-editor gate.

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
EInteractionResult UInteractionComponent::CheckInteraction(AActor* TargetActor, FGameplayTag InteractionType,
    float RangeTolerance) const
{
    if (!IsValid(TargetActor) || !GetOwner()) return EInteractionResult::Failed;
    if (Distance > InteractionRange * RangeTolerance) return EInteractionResult::OutOfRange;
    if (!Interactable || !Interactable->IsEnabled()) return EInteractionResult::Failed;   // FindComponentByClass
    if (!Interactable->CanInteract(GetOwner(), MakeContext(...))) return EInteractionResult::NotAllowed;
    return EInteractionResult::Success;
}

void UInteractionComponent::ServerRPC_RequestInteract_Implementation(AActor* TargetActor, FGameplayTag InteractionType)
{
    const EInteractionResult Check = CheckInteraction(TargetActor, InteractionType, 1.1f);   // 10% for latency
    if (Check != EInteractionResult::Success)
    {
        ClientRPC_InteractionResult(TargetActor, InteractionType, Check);
        return;
    }
    ClientRPC_InteractionResult(TargetActor, InteractionType, GetInteractable(TargetActor)->Interact(GetOwner(), InteractionType));
}
```

`CheckInteraction` is the one predicate (code review decision D-CR16): the client runs it before it sends (without the
tolerance), so most refusals never reach the network, and both server RPCs, the instant request and the channeled start,
run it again (G92 added the channeled start's `CanInteract`). The 10% range tolerance absorbs the
client being slightly ahead of where the server sees it.

### World Item Replication

`AWorldItem` replicates (`bReplicates`, not always relevant) and its `ItemInstance` is a replicated property. Clients
see the item vanish when the server returns it to the pool and their inventory update through the
`FFastArraySerializer`; the pickup itself happens only on the server.
