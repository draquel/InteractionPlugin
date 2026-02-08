# Interaction System — Detailed Design

## Overview

This document covers the internal design of the InteractionPlugin: detection strategies, the interaction component lifecycle, the world item actor, object pooling, and channeled interactions. For how this plugin integrates with ItemInventoryPlugin and EquipmentPlugin, see `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md`.

---

## Detection System

### Detection Loop

The `UInteractionComponent` does NOT run detection every frame. It uses a configurable timer (default 0.1s) to reduce cost.

```
TickDetection() — fires every DetectionTickRate seconds:
    1. Ask DetectionStrategy for nearby actors within InteractionRange
    2. Filter to actors implementing IInteractable
    3. Filter to actors where CanInteract() returns true
    4. Score each candidate
    5. Update NearbyInteractables array
    6. Determine best target
    7. If best target changed: fire OnInteractableFound / OnInteractableLost
```

**Detection only runs on the local client.** The server never runs detection — it only validates interaction requests when they arrive via RPC.

### Scoring Algorithm

Each candidate is scored by a weighted combination:

```
Score = (DistanceScore * DistanceWeight)
      + (AngleScore * AngleWeight)
      + (PriorityScore * PriorityWeight)

DistanceScore = 1.0 - (Distance / InteractionRange)      // 0-1, closer = higher
AngleScore = DotProduct(CameraForward, DirectionToTarget) // -1 to 1, centered = higher
PriorityScore = InteractionPriority / MaxPriority          // 0-1, from IInteractable

Default weights: Distance=0.4, Angle=0.4, Priority=0.2
```

The scoring function is `BlueprintNativeEvent` — games can override it for custom behavior (e.g., prioritize quest-relevant objects regardless of distance).

### Detection Strategies

**USphereOverlapDetection:**
```
Performs UWorld::OverlapMultiByChannel centered on the source actor.
Uses a dedicated collision channel or object type (configurable).
Returns all overlapping actors.

Best for: Third-person games, broad-area interaction.
```

**ULineTraceDetection:**
```
Performs a line trace from the camera location along camera forward.
Range = InteractionRange.
Uses configurable collision channel.
Returns the first hit actor (or all hits in multi-trace mode).

Best for: First-person games, precision aiming.
```

**Custom strategies** subclass `UInteractionDetectionStrategy` and override `DetectInteractables`. The strategy is an `EditInlineNew` instanced object on `UInteractionComponent`, so it's configurable per-actor in the editor.

---

## Interaction Flow

### Instant Interaction

```
Player presses Interact input
    → UInteractionComponent::TryInteract(InteractionType)
        → Checks CurrentBestTarget is valid
        → If multiplayer: sends ServerRPC_RequestInteract(TargetActor, InteractionType)
        → If standalone/server:
            → Validates range (re-check, target may have moved since detection)
            → Calls IInteractable::Interact(Interactor, InteractionType) on target
            → Receives EInteractionResult
            → Fires OnInteractionCompleted or OnInteractionFailed
```

### Channeled Interaction

For interactions that take time (opening a chest, picking a lock, looting):

```
State Machine:
    IDLE → CHANNELING → COMPLETED
                      → CANCELLED

StartChanneledInteraction(Target, InteractionType, Duration):
    → Validates target supports this interaction
    → Sets state to CHANNELING
    → Starts timer (server-driven in multiplayer)
    → Fires OnInteractionStarted

Tick (while CHANNELING):
    → Update progress: CurrentTime / Duration (0.0 → 1.0)
    → Fire OnChanneledProgress(Progress)
    → Check cancel conditions:
        - Player moved beyond CancelMoveThreshold (default 50 units)
        - Player took damage (if bCancelOnDamage is true)
        - Target became invalid (destroyed, disabled)
        - Player pressed cancel input
    → If any cancel condition met:
        → Set state to CANCELLED
        → Fire OnInteractionFailed(Cancelled)

Timer completes:
    → Call IInteractable::Interact(Interactor, InteractionType)
    → Set state to COMPLETED
    → Fire OnInteractionCompleted
    → Return to IDLE
```

**Multiplayer channeling:** The server owns the timer. Client sends `ServerRPC_StartChanneledInteraction`. Server starts the timer, validates cancellation conditions server-side, and replicates progress to the client for UI. This prevents speed hacks where the client claims the channel completed instantly.

---

## World Item Actor

### AWorldItem Lifecycle

```
POOLED (inactive) → INITIALIZING → ACTIVE (in world) → PICKUP → POOLED
                                         ↓
                                    DESPAWN (timeout) → POOLED
```

**InitializeFromItem(FItemInstance):**
1. Store the FItemInstance
2. Look up UItemDefinition via ItemDatabaseSubsystem
3. Find UItemFragment_WorldDisplay on the definition
4. Async load the world mesh → `OnMeshLoaded` callback
5. Configure InteractableComponent with pickup options
6. Enable collision and physics
7. Set actor to visible (after mesh loads)

**OnPickupInteraction(Interactor, InteractionType):**
1. Get `UInventoryComponent` from interactor (via `IInventoryOwner`)
2. Call `InventoryComponent->TryAddItem(ItemInstance)`
3. If success: return self to object pool
4. If failure (inventory full): remain in world, optionally show "inventory full" feedback

**ResetForPool():**
1. Set actor to hidden
2. Disable collision and physics
3. Clear mesh component
4. Clear ItemInstance data
5. Detach from any parent
6. Move to pool staging location (below world, or origin)

### Mesh Loading

World items use **async loading** exclusively. The pattern:

```cpp
void AWorldItem::InitializeFromItem(const FItemInstance& Item)
{
    ItemInstance = Item;
    UItemDefinition* Def = GetItemDatabaseSubsystem()->GetDefinition(Item.ItemDefinitionId);
    UItemFragment_WorldDisplay* DisplayFrag = Def->FindFragment<UItemFragment_WorldDisplay>();

    if (DisplayFrag && !DisplayFrag->WorldMesh.IsNull())
    {
        // Start async load
        FStreamableManager& Manager = UAssetManager::GetStreamableManager();
        MeshLoadHandle = Manager.RequestAsyncLoad(
            DisplayFrag->WorldMesh.ToSoftObjectPath(),
            FStreamableDelegate::CreateUObject(this, &AWorldItem::OnMeshLoaded)
        );
    }
}

void AWorldItem::OnMeshLoaded()
{
    UItemDefinition* Def = GetItemDatabaseSubsystem()->GetDefinition(ItemInstance.ItemDefinitionId);
    UItemFragment_WorldDisplay* DisplayFrag = Def->FindFragment<UItemFragment_WorldDisplay>();

    MeshComponent->SetStaticMesh(DisplayFrag->WorldMesh.Get());
    if (!DisplayFrag->WorldMaterial.IsNull())
    {
        MeshComponent->SetMaterial(0, DisplayFrag->WorldMaterial.Get());
    }
    MeshComponent->SetWorldScale3D(DisplayFrag->WorldScale);
    SetActorHiddenInGame(false);
}
```

**Never** call `LoadSynchronous` in gameplay code paths. A hitch from loading one mesh is bad; loading 50 items from a loot drop synchronously would freeze the game.

---

## Object Pooling

### UWorldItemPoolSubsystem

`UWorldSubsystem` that manages a pool of `AWorldItem` actors.

```
Configuration:
    InitialPoolSize = 50          (pre-spawned on world begin)
    MaxPoolSize = 200             (hard cap, prevents unbounded growth)
    DespawnTimeout = 300.0f       (seconds before uncollected items despawn, 0 = never)
    bExpandPoolOnDemand = true    (spawn new if pool empty, up to MaxPoolSize)

API:
    AWorldItem* GetWorldItem()
        → Returns an inactive item from pool, or spawns new if allowed
        → Returns nullptr if pool exhausted and expansion disabled/capped

    void ReturnWorldItem(AWorldItem* Item)
        → Calls ResetForPool() on the item
        → Returns it to the available pool

    void ReturnAllWorldItems()
        → Returns every active world item to the pool (level cleanup)
```

**Pre-warming:** During `OnWorldBeginPlay`, the subsystem spawns `InitialPoolSize` actors, calls `ResetForPool()` on each, and stores them in a TArray. Spawning is spread across multiple frames if needed (e.g., 10 per frame) to avoid a hitch.

**Despawn timer:** Active world items that aren't picked up within `DespawnTimeout` are automatically returned to the pool. The timer starts when the item becomes active. Reset the timer if a player comes within interaction range (to avoid despawning items the player is walking toward).

### When NOT to Pool

Items that are placed by level designers (not dropped at runtime) should be regular actors, not pooled. Pooled items are for dynamic runtime spawning: loot drops, inventory drops, loot container populations.

---

## InteractableComponent Details

### Configuration Patterns

**Simple single-interaction (e.g., item pickup):**
```
InteractionOptions:
  [0] Type=Interaction.Type.Pickup, Text="Pick Up", Priority=0, bRequiresHold=false
```

**Multi-option (e.g., loot container):**
```
InteractionOptions:
  [0] Type=Interaction.Type.Open, Text="Open", Priority=0, bRequiresHold=true, HoldDuration=1.0
  [1] Type=Interaction.Type.Inspect, Text="Inspect", Priority=1, bRequiresHold=false
```

**Context-dependent (e.g., locked door):**
Override `GetInteractionOptions` in Blueprint to check if the player has the key:
```
If player has key → [Open]
Else → [Locked (greyed out)]
```

### Enable/Disable

`bIsEnabled` on the InteractableComponent controls whether it appears in detection results. Use cases:
- Chest already looted → disable
- NPC in combat → disable dialogue interaction
- Door animation playing → disable until animation completes

Disabling fires `OnInteractableLost` on any player currently targeting this interactable.

---

## Multiplayer Specifics

### Interaction Validation on Server

The server validates every interaction request:

```cpp
void UInteractionComponent::ServerRPC_RequestInteract_Implementation(
    AActor* TargetActor, FGameplayTag InteractionType)
{
    // 1. Actor validity
    if (!IsValid(TargetActor)) return;

    // 2. Interface check
    if (!TargetActor->Implements<UCGFInteractableInterface>()) return;

    // 3. Range check (server re-validates, client may have stale data)
    float Distance = FVector::Dist(GetOwner()->GetActorLocation(), TargetActor->GetActorLocation());
    if (Distance > InteractionRange * 1.1f) return;  // 10% tolerance for latency

    // 4. Can interact check
    FInteractionContext Context;
    Context.Interactor = GetOwner();
    Context.InteractableActor = TargetActor;
    Context.InteractionType = InteractionType;
    Context.Distance = Distance;

    if (!ICGFInteractableInterface::Execute_CanInteract(TargetActor, GetOwner(), Context)) return;

    // 5. Execute
    EInteractionResult Result = ICGFInteractableInterface::Execute_Interact(
        TargetActor, GetOwner(), InteractionType);

    // 6. Notify client of result
    ClientRPC_InteractionResult(TargetActor, InteractionType, Result);
}
```

**Range tolerance:** The server allows 10% extra range to account for network latency. Without this, a player at exactly max range might fail the server check because their position is slightly behind where they were on the client.

### World Item Replication

`AWorldItem` replicates:
- `FItemInstance` (for display on clients)
- Actor location/rotation (standard movement replication)
- Hidden/visible state (for pool transitions)

Clients use the replicated `FItemInstance` to display the correct mesh and interaction prompt. The actual pickup operation happens on the server — the client just sees the item disappear (via replication) and their inventory update (via FFastArraySerializer).
