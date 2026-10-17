# InteractionPlugin — Plugin Instructions

## Purpose

World interaction: detecting what the player can interact with, running instant and channeled
interactions server-authoritatively, representing items in the world (`AWorldItem`) and pooling
those actors. Depends on CommonGameFramework; has an acknowledged private dependency on
ItemInventoryPlugin for `AWorldItem` (item definitions, the world-display fragment, the item
database). Nothing else may reference ItemInventoryPlugin or EquipmentPlugin.

## Documentation

- `Documentation/INTERACTION_SYSTEM.md` — detection loop and scoring, detection strategies,
  instant vs channeled interaction flow, `AWorldItem` lifecycle and async mesh loading, the world
  item pool, interactable component patterns, multiplayer validation.
- `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md` — the pickup / loot-container
  sequence diagrams and the plugin boundaries.
- `Plugins/CommonGameFramework/Documentation/COMMON_TYPES.md` — `IInteractable`,
  `IInteractionSource`, `FInteractionOption`, `FInteractionContext`, `Interaction.Type.*` tags.

## Module Structure

```
InteractionPlugin/
├── Source/InteractionPlugin/
│   ├── Public/
│   │   ├── Components/
│   │   │   ├── InteractionComponent.h        ← On the interactor: detection, TryInteract, channeled flow, server RPCs
│   │   │   └── InteractableComponent.h       ← On the target: options, enable/disable, Interact + InteractionHandler
│   │   ├── Detection/
│   │   │   ├── InteractionDetectionStrategy.h← Abstract strategy (EditInlineNew)
│   │   │   ├── SphereOverlapDetection.h      ← Overlap around the interactor (third person)
│   │   │   └── LineTraceDetection.h          ← Camera trace (first person)
│   │   ├── Actors/
│   │   │   └── WorldItem.h                   ← Pooled item-in-the-world actor (FItemInstance + mesh + pickup)
│   │   ├── Subsystems/
│   │   │   └── WorldItemPoolSubsystem.h      ← World subsystem: pool of AWorldItem actors
│   │   ├── UI/
│   │   │   └── InteractionPromptWidget.h     ← Programmatic prompt ("E — Pick Up X")
│   │   └── InteractionPlugin.h
│   └── Private/ (mirrors Public)
└── InteractionPlugin.uplugin
```

## Key Classes

### UInteractionComponent (the interactor)

- `InteractionRange`, `DetectionTickRate` (default 0.1 s — detection is a timer, never per frame,
  and runs on the local client only), `DetectionStrategy` (instanced), scoring weights
  (`DistanceWeight`, `AngleWeight`, `PriorityWeight`), `CancelMoveThreshold`, `bCancelOnDamage`.
- `TryInteract(InteractionType)` on the current best target; `TryInteractWith(Target, Type)`;
  `StartChanneledInteraction(Target, Type, Duration)` / `CancelChanneledInteraction()`.
- Delegates: `OnInteractableFound` / `OnInteractableLost` (drive the prompt),
  `OnInteractionCompleted` / `OnInteractionFailed`, channel progress.
- Server RPCs `ServerRPC_RequestInteract`, `ServerRPC_StartChanneledInteraction`,
  `ServerRPC_CancelChanneledInteraction`; `ClientRPC_InteractionResult` reports back.

### UInteractableComponent (the target)

- Implements `IInteractable` on behalf of its actor (the pattern other components copy:
  `UVCCombatComponent` implements `ICGFDamageableInterface` the same way).
- `InteractionOptions` (`FInteractionOption`: type tag, display text, priority, hold), `InteractionPriority`,
  `bIsEnabled` (+ `Enable()` / `Disable()`; disabling fires `OnInteractableLost` on anyone targeting it).
- `InteractionHandler` — a `TFunction<EInteractionResult(AActor* Interactor, FGameplayTag Type)>` the owning
  actor binds in `PostInitializeComponents`; `Interact()` runs where the flow executes (the server), so the
  handler is the place for authoritative logic (open the door, roll the loot). `OnInteractionTriggered`
  fires alongside for Blueprint.

### AWorldItem + UWorldItemPoolSubsystem

- `InitializeFromItem(FItemInstance)`: stores the instance, builds the pickup option, shows the fallback
  mesh immediately, async-loads the `UItemFragment_WorldDisplay` mesh and applies `WorldScale` and
  `WorldRotation` (the lying pose) on load. Pickup hands the instance to the interactor's inventory via
  `IInventoryOwner` and returns the actor to the pool.
- `UWorldItemPoolSubsystem::SpawnWorldItem(Item, Location, Rotation)` / `ReturnWorldItem` / `ReturnAllWorldItems`;
  pre-warmed, capped, despawn timeout. Pool only runtime drops (loot bursts, inventory drops); placed items
  are regular actors.
- Pooled items wait hidden, without collision or physics, at `ParkingLocation` (default (0, 0, -100,000), far under a
  flat world's ground). `SetParkingLocation` moves the spot and every waiting item; game code calls it for a world whose
  ground passes through the default (VoxelCharacterPlugin's game mode parks them at a planet's centre).
- `SetGravityCenter` / `ClearGravityCenter` (a planet; the same game mode sets it): spawned items stand upright on the
  direction from it (the requested heading kept), `AWorldItem::SnapToGround` traces along local down and rests the
  mesh's lowest corner on the hit, and `AWorldItem::ApplyRestingPose` turns the placeholder's or the WorldDisplay
  fragment's pose relative to that upright frame. The mesh is the actor's root, so never `SetRelativeRotation` it
  directly: that is the world rotation. Without a centre everything is the old world-Z behaviour.

## Rules

1. **Detection is client-side, interaction is server-side.** The server never runs detection; every
   interaction request is re-validated on the server (actor valid, implements the interface, in range with
   10% tolerance, `CanInteract`) before `Interact` executes. Channeled timers are server-owned.
   Detection runs exactly while the owning pawn is locally controlled, re-checked on every controller
   change: a respawned pawn runs BeginPlay before it is possessed, so a BeginPlay-only check leaves it
   blind (no target, E does nothing) for the rest of the session.
2. **Never `LoadSynchronous` a world mesh in gameplay code.** Async-load through the streamable manager; the
   fallback mesh covers the gap.
3. **Options are data, logic is the handler.** Prompt text and hold rules live in `InteractionOptions`;
   what happens lives in `InteractionHandler` (or a Blueprint override of `GetInteractionOptions` for
   context-dependent options such as a locked door).
4. **Interactable actors that keep state replicate it themselves** (door state, chest opened, container
   searched) and `Disable()` their component when they are spent so the prompt dies on every machine.
5. **New interaction kinds are new tags** (`Interaction.Type.*` in CommonGameFramework), never new enum values.
6. **Every interactable needs a simple collision shape.** `USphereOverlapDetection` queries simple shapes only, so a
   mesh exported without simple collision is invisible to detection (no prompt, E does nothing). Give such actors
   their own query-only box (the dungeon wall torch and container do). `vox.Interact detect` and the
   `VoxelWorlds.POI.InteractionDetection` test go through detection; plain `vox.Interact` does not.
7. **Per-player prompt text uses `OptionsModifier`; state changes call `NotifyOptionsChanged()`.** The prompt reads
   `GetInteractionOptions(LocalPawn)`, so a native `OptionsModifier` can tailor text to the interactor (a dark torch
   reads "Light torch" only to someone carrying a flame). When an interactable's options or text change, call
   `NotifyOptionsChanged()` on every machine: the interaction component re-announces its current target and the
   prompt refreshes. An interactor whose own state changes what targets offer calls `RefreshCurrentTarget()`.
8. **World items rest on the ground and go back to the pool.** `SnapToGround()` runs at spawn and after the real mesh
   loads; pickup returns the actor through `UWorldItemPoolSubsystem::ReturnWorldItem` (resetting it alone left it
   counted as active until its despawn timer and could exhaust the pool).

## Build.cs Dependencies

```csharp
PublicDependencyModuleNames: Core, CoreUObject, Engine, NetCore, GameplayTags, CommonGameFramework
PrivateDependencyModuleNames: ItemInventoryPlugin (AWorldItem only, acknowledged coupling), UMG, Slate, SlateCore (prompt widget)
```

## Tests

None yet. First candidates: server-side request validation (range tolerance, interface check), the
scoring function, and the pool's return/reuse cycle.
