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

**`InitializeFromItem(FItemInstance)`** (the server):
1. Store the instance (`ItemInstance`, replicated).
2. Show the item (`ShowItem`, which a client runs too, from `OnRep_ItemInstance`): the fallback cube at a quarter scale
   at once (items without a world mesh would otherwise be invisible pickups), collision on, physics off; look up the
   definition through `UItemDatabaseSubsystem` and, when it has a `UItemFragment_WorldDisplay` mesh, request it
   asynchronously.
3. Add the "Pick Up <name>" option (`Interaction.Type.Pickup`) to the `UInteractableComponent` and enable it.
4. Record where the item stands (`Rest`, replicated; below).
5. On load (`OnMeshLoaded`, every machine): set the mesh, `WorldMaterial` and `WorldScale`. The server then applies
   `WorldRotation` as the resting pose (`ApplyRestingPose`) and settles the item again (`SnapToGround()`), because the
   real mesh's bounds differ from the placeholder's.

**Settling (`SnapToGround`, the server).** The item rests on what is below it: it traces 500 uu down (from 50 uu above
itself) for `ECC_WorldStatic` and rests the mesh's lowest corner 1 uu over the hit; with no hit it stays where it was
dropped. Water comes first (`bFloatOnWater`, on by default): the item asks `UCGFWaterSubsystem` for the water over its
position (the next section), and in the water, or within the trace's 500 uu over it, it rests at the surface straight
over where it is, `FloatSubmersion` (0.6) of its height under the surface. The ground keeps it where the ground lies
between the item and the surface (a pier), where the water over the ground is shallower than `MinFloatWaterDepth`
(20 uu), where something solid lies between an item under water and the surface over it (a flooded cave's roof), and
where the provider says the place holds no water (a dry cave below the level). The settle runs at the spawn, after the
mesh loads, and when the water changes (`UWorldItemPoolSubsystem::SettleItems`).

**The resting place (`Rest`, `FWorldItemRest`).** Every settle records the item's location, its rotation, the up it
rests along and whether it floats. The item does not replicate movement: a client places its copy by `OnRep_Rest`, so
the server's second settle and the resting pose reach it (before, a client kept the place and the pose its copy first
arrived with, 12 uu off after the mesh loaded: G203). The server keeps the resting rotation apart (`PoseRotation`), so a
later settle starts from the pose and not from the rotation the item is drawn with.

**The bob.** A floating item (`IsFloating()`) ticks on every machine with a view, never on a dedicated server, and is
drawn about its resting place: `GetFloatingPose` adds a rise and fall along the rest's up (`FloatBobAmplitude` 2.5 uu,
`FloatBobPeriod` 2.8 s, a phase from the place so items dropped together do not move as one) and a small rock
(`FloatRockDegrees` 3). It is drawn, not replicated: each machine uses its own clock, `Rest` never changes, and an item
nobody has seen in the last half second is left as it is. The tick is off for every item that does not float.

On the same machines a floating item's mesh draws custom depth (`bCustomDepthWhileFloating`, on by default; off again
when it no longer floats). A water material that shades by the water's depth, or draws foam where the water is thin,
takes an item lying just under the surface for a shallow bed; with the item in custom depth it can tell the two apart
(this project's water draws no shore foam over it, as over a swimmer).

**Pickup (`OnPickupInteraction`, the component's handler):** find the interactor's inventory through
`ICGFInventoryInterface` (else a `UInventoryComponent` on the actor), `TryAddItem(ItemInstance)`; on success return the
actor through `UWorldItemPoolSubsystem::ReturnWorldItem`. On failure (inventory full) the item stays.

**`ResetForPool(ParkingLocation)`:** cancel the mesh load, hide, collision and physics off, clear the mesh (`HideItem`,
which a client runs when its copy's instance is cleared), disable and clear the interactable's options, clear the
instance and the resting place, detach, move to the parking location.

Never `LoadSynchronous` a world mesh in gameplay code: one mesh is a hitch, a loot burst of fifty is a freeze.

### In Water

The framework knows no world, so the water is whatever the game layer registers with CommonGameFramework's
`UCGFWaterSubsystem` (an object implementing `ICGFWaterSurfaceInterface`; `Plugins/CommonGameFramework/Documentation/COMMON_TYPES.md`).
`AWorldItem::SnapToGround` asks it for the surface over the item's position: the point of the surface, its up, the
item's depth and whether water is really there. In this project VoxelCharacterPlugin's `UVCWaterSurfaceBridge` answers
from the voxel world's water levels, on a plane (the level is a world Z), an island, a block world and a planet (the up
is the radial and the level a radius). With no provider there is no water and an item settles as it did before.

The answer is read per settle and never kept: a world may have several bodies of water, and a body's level may change.
When the water's owner says it changed (`UCGFWaterSubsystem::NotifyWaterChanged(Bounds)`, or the provider being set or
cleared), the pool settles the active items inside the bounds again (`UWorldItemPoolSubsystem::SettleItems`, bound to
`OnWaterChanged`); nothing polls. A change of the ground under an item (a dig) does not move it; game code may call
`SettleItems` for the place.

Tests: `Interaction.WorldItem.FloatsOnWater` (a plane and a sphere from `UCGFTestWaterSurface`: the surface, the
submersion, the bob, a pier, shallow water, a roof, a dry place, a changed level, no provider); with a voxel world,
VoxelCharacterPlugin's `VoxelCharacter.Water.ItemFloats.*`; over a connection, the game module's
`VoxelEngine.Replication.Framework` (the client's copy shown, at the server's resting place, bobbing).

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
    int32 SettleItems(Bounds)                              settle the active items in the bounds again (the water changed)
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

`AWorldItem` replicates (`bReplicates`, not always relevant) two properties: `ItemInstance` and `Rest`. Its mesh
component does not replicate, so a client builds what its copy shows from the instance (`OnRep_ItemInstance`: `ShowItem`
for a valid instance, the placeholder and then the definition's mesh, material and scale; `HideItem` when the server
clears it) and places it by the resting place (`OnRep_Rest`). Before this a client's copy had no mesh and was not
visible at all on a dedicated server's client (G203). Clients see the item vanish when the server returns it to the
pool and their inventory update through the `FFastArraySerializer`; the pickup itself happens only on the server.
