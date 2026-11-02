# InteractionPlugin — Plugin Instructions

> State as of 2026-11-02 (code review CR5).

## Purpose

World interaction: detecting what the player can interact with, running instant and channeled interactions
server-authoritatively, representing items in the world (`AWorldItem`) and pooling those actors. Depends on
CommonGameFramework and, privately, on ItemInventoryPlugin: `AWorldItem` uses `UItemDefinition`,
`UItemFragment_WorldDisplay`, `UItemDatabaseSubsystem` and `UInventoryComponent`. That dependency is downward and
required (the `.uplugin` lists ItemInventoryPlugin; D-CR22). The plugin never references EquipmentPlugin, and nothing in
the framework depends on it.

## Reading Order

1. `Documentation/INTERACTION_SYSTEM.md` — detection and scoring, detection strategies, instant and channeled flows,
   `AWorldItem` and the pool, interactable patterns, server validation.
2. `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md` — the pickup flow across plugins, the plugin boundaries.
3. `Plugins/CommonGameFramework/Documentation/COMMON_TYPES.md` — `FInteractionOption`, `FInteractionContext`, the
   interaction delegates, `Interaction.Type.*` tags.

## Module Structure

```
InteractionPlugin/
├── Source/InteractionPlugin/
│   ├── Public/
│   │   ├── Components/
│   │   │   ├── InteractionComponent.h        ← on the interactor: detection, TryInteract, channeled flow, server RPCs
│   │   │   └── InteractableComponent.h       ← on the target: options, enable/disable, Interact + InteractionHandler
│   │   ├── Detection/
│   │   │   ├── InteractionDetectionStrategy.h← abstract strategy (EditInlineNew)
│   │   │   ├── SphereOverlapDetection.h      ← overlap around the interactor (third person)
│   │   │   └── LineTraceDetection.h          ← camera trace (first person)
│   │   ├── Actors/WorldItem.h                ← pooled item-in-the-world actor (FItemInstance + mesh + pickup)
│   │   ├── Subsystems/WorldItemPoolSubsystem.h ← world subsystem: pool of AWorldItem actors
│   │   ├── UI/InteractionPromptWidget.h      ← code-built prompt ("E — Pick Up X")
│   │   └── InteractionPlugin.h               ← module header, log category LogIAP
│   ├── Private/ (mirrors Public)
│   └── Tests/                                ← automation tests (Interaction.*)
└── InteractionPlugin.uplugin
```

## Key Classes

### UInteractionComponent (the interactor)

- `InteractionRange` (1,000 uu), `DetectionTickRate` (0.1 s: detection is a timer, never per frame, and runs on the
  locally controlled pawn only), `DetectionStrategy` (instanced), scoring weights (`DistanceWeight`, `AngleWeight`,
  `PriorityWeight`), `CancelMoveThreshold` (50 uu), `ChannelResultTimeout` (2 s: how long a client holds a finished
  channel for the server's result). No damage cancel: the unread `bCancelOnDamage` was removed (G92).
- `TryInteract(Type)` on the current best target; `TryInteractWith(Target, Type)`;
  `StartChanneledInteraction(Target, Type, Duration)` (returns `InProgress` or the refusal) /
  `CancelChanneledInteraction()`; `RefreshCurrentTarget()`. `CheckInteraction` is the one predicate both machines run.
- Delegates: `OnInteractableFound` / `OnInteractableLost` (drive the prompt), `OnInteractionStarted`,
  `OnInteractionCompleted` / `OnInteractionFailed`, `OnChanneledProgress`.
- Server RPCs `ServerRPC_RequestInteract`, `ServerRPC_StartChanneledInteraction`,
  `ServerRPC_CancelChanneledInteraction`; `ClientRPC_InteractionResult` reports back.

### UInteractableComponent (the target)

- The component is what makes an actor interactable: the interaction component finds it with `FindComponentByClass`.
  It offers `GetInteractionOptions`, `CanInteract` and `Interact` itself; CommonGameFramework's
  `ICGFInteractableInterface` is not implemented by it or anything else.
- `InteractionOptions` (`FInteractionOption`: type tag, display text, priority, hold), `InteractionPriority`,
  `bIsEnabled` (replicated, `BlueprintReadOnly`; change it on the server with `Enable()` / `Disable()` /
  `SetEnabled()`; disabling fires `OnInteractableLost` on anyone targeting it).
- `InteractionHandler`: a `TFunction<EInteractionResult(AActor* Interactor, FGameplayTag Type)>` the owning actor
  binds in `PostInitializeComponents`; `Interact()` runs it where the flow executes (the server), so it is the place for
  authoritative logic (open the door, roll the loot). `OnInteractionTriggered` fires alongside for Blueprint.
- `OptionsModifier`: a native hook that edits the options per interactor; `NotifyOptionsChanged()` /
  `OnOptionsChanged` tell the current interactor to refresh its prompt.

### AWorldItem + UWorldItemPoolSubsystem

- `InitializeFromItem(FItemInstance)`: stores the instance (replicated), builds the "Pick Up <name>" option, shows the
  fallback cube at once, async-loads the `UItemFragment_WorldDisplay` mesh and applies `WorldMaterial`, `WorldScale`
  and `WorldRotation` (the lying pose) on load. Pickup hands the instance to the interactor's inventory
  (`ICGFInventoryInterface`, else a `UInventoryComponent` on the actor) and returns the actor to the pool.
- `UWorldItemPoolSubsystem::SpawnWorldItem(Item, Location, Rotation)` / `GetWorldItem` / `ReturnWorldItem` /
  `ReturnAllWorldItems`; pre-warmed, capped, despawn timeout. The pool is for runtime drops (loot bursts, inventory
  drops); placed items are regular actors.
- Pooled items wait hidden, without collision or physics, at `ParkingLocation` (default (0, 0, -100,000), far under a
  flat world's ground). `SetParkingLocation` moves the spot and every waiting item; game code calls it for a world whose
  ground passes through the default (VoxelCharacterPlugin's game mode parks them at a planet's centre).
- `SetGravityCenter` / `ClearGravityCenter` (a planet; the same game mode sets it): spawned items stand upright on the
  direction from it (the requested heading kept), `AWorldItem::SnapToGround` traces along local down and rests the
  mesh's lowest corner on the hit, and `AWorldItem::ApplyRestingPose` turns the placeholder's or the WorldDisplay
  fragment's pose relative to that upright frame. The mesh is the actor's root, so never `SetRelativeRotation` it
  directly: that is the world rotation. Without a centre everything is world-Z.

## Rules

1. **Detection is client-side, interaction is server-side.** The server never runs detection. Every request is checked
   again on the server (target valid, within 1.1 x `InteractionRange`, an enabled `UInteractableComponent`,
   `CanInteract`) before `Interact` runs. Detection runs exactly while the owning pawn is locally controlled, re-checked
   on every controller change: a respawned pawn runs BeginPlay before it is possessed, so a BeginPlay-only check leaves
   it blind (no target, E does nothing) for the rest of the session.
2. **Channeled interactions complete on the server only (G92).** The server runs the channel from
   `ServerRPC_StartChanneledInteraction` (after `CheckInteraction`, `CanInteract` included, and with the option's
   `HoldDuration` as the floor) and calls `Interact` when it completes, answering a remote owner with
   `ClientRPC_InteractionResult`. The owning client's copy only drives `OnChanneledProgress`: it holds at full progress
   until that answer ends it, and gives up after `ChannelResultTimeout`. Never call `Interact` from client code.
3. **Never `LoadSynchronous` a world mesh in gameplay code.** Async-load through the streamable manager; the fallback
   mesh covers the gap.
4. **Options are data, logic is the handler.** Prompt text and hold rules live in `InteractionOptions`; what happens
   lives in `InteractionHandler` (or a Blueprint override of `GetInteractionOptions` for context-dependent options such
   as a locked door).
5. **Interactable actors that keep state replicate it themselves** (door state, chest opened, container searched) and
   `Disable()` their component when they are spent so the prompt dies on every machine.
6. **New interaction kinds are new tags** (`Interaction.Type.*` in CommonGameFramework), never new enum values.
7. **Every interactable needs a simple collision shape.** `USphereOverlapDetection` queries simple shapes only, so a mesh
   exported without simple collision is invisible to detection (no prompt, E does nothing). Give such actors their own
   query-only box (the dungeon wall torch and container do). `vox.Interact detect` and the
   `VoxelWorlds.POI.InteractionDetection` tests go through detection; plain `vox.Interact` does not.
8. **Per-player prompt text uses `OptionsModifier`; state changes call `NotifyOptionsChanged()`.** The prompt reads
   `GetInteractionOptions` for the local pawn, so a native `OptionsModifier` can tailor text to the interactor (a dark
   torch reads "Light torch" only to someone carrying a flame). When an interactable's options or text change, call
   `NotifyOptionsChanged()` on every machine: the interaction component re-announces its current target and the prompt
   refreshes. An interactor whose own state changes what targets offer calls `RefreshCurrentTarget()`.
9. **World items rest on the ground and go back to the pool.** `SnapToGround()` runs at spawn and after the real mesh
   loads; pickup returns the actor through `UWorldItemPoolSubsystem::ReturnWorldItem` (resetting it alone left it
   counted as active until its despawn timer and could exhaust the pool).

## Build.cs Dependencies

```csharp
PublicDependencyModuleNames:  Core, CoreUObject, Engine, NetCore, GameplayTags, CommonGameFramework
PrivateDependencyModuleNames: ItemInventoryPlugin (AWorldItem), UMG, Slate, SlateCore (prompt widget)
```

## Logging and Tests

- Log category `LogIAP` (`InteractionPlugin.h`).
- Tests in `Source/InteractionPlugin/Tests/`: `Interaction.Flow.LineTraceAndInteract` (the line-trace strategy detects
  an interactable, `TryInteractWith` runs its handler, a disabled or out-of-range one fails), `Interaction.Channeled.*`
  (simulated server/client in one world with `CGFTesting::FRPCCall`: the server alone runs the handler, at the option's
  hold whatever the client asked; the server's refusals, the client's wait and timeout, cancels on both sides) and
  `Interaction.WorldItem.SnapAlongGravity` (a world item resting along a planet's gravity). VoxelWorldPOI's
  `VoxelWorlds.POI.InteractionDetection.*` covers detection of self-shaped interactables. Not covered yet: the scoring
  function, the pool's return-and-reuse cycle.
