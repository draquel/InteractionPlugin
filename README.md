# InteractionPlugin

General-purpose world interaction framework for Unreal Engine 5.7. Handles detecting nearby interactable objects, selecting the best candidate, and executing interactions with full multiplayer support.

## What This Plugin Does

InteractionPlugin provides the layer between the player and the world. It's not inventory-specific — doors, levers, NPCs, terminals, vehicles, and item pickups all use the same interaction system.

### Detection

The `UInteractionComponent` (attached to the player) continuously scans for nearby interactable objects using pluggable detection strategies:

- **Sphere overlap** — Broad area detection for third-person games
- **Line trace** — Precision aim-based detection for first-person games
- **Custom** — Subclass the strategy base for game-specific needs

Detection runs on a throttled timer (default 0.1s, configurable), not every frame. Candidates are scored by distance, angle to camera, and priority to determine the best interaction target.

### Interactions

Two interaction models:

- **Instant** — Press interact, action happens immediately (pick up item, flip switch)
- **Channeled** — Press and hold, progress bar fills, action happens on completion (open chest, pick lock, loot body). Cancels on movement, damage, or input release. Server-authoritative timer prevents speed exploits.

### World Items

`AWorldItem` bridges the interaction and item systems. It's a physicalized actor that represents an `FItemInstance` in the world — with mesh, collision, and a pickup interaction. Supports:

- Async mesh loading from item definitions
- Object pooling via `UWorldItemPoolSubsystem` (pre-spawns actors, recycles on pickup/despawn)
- Configurable despawn timeout for uncollected drops

### InteractableComponent

A convenience component for making any actor interactable without writing C++. Attach it, configure interaction options in the editor, and bind to the `OnInteracted` event in Blueprint.

## Requirements

- Unreal Engine 5.7
- [CommonGameFramework](../CommonGameFramework/) plugin
- [ItemInventoryPlugin](../ItemInventoryPlugin/) plugin (for `AWorldItem` — the core interaction framework works without it)

## Installation

Clone into your project's `Plugins/` directory:

```bash
git clone <repo-url> Plugins/InteractionPlugin
```

Ensure CommonGameFramework is also present in `Plugins/`.

Add to your module's `Build.cs`:

```csharp
PublicDependencyModuleNames.Add("InteractionSystem");
```

## Module Dependencies

```
InteractionSystem (Runtime)
├── CommonGameFramework
├── Core, CoreUObject, Engine, NetCore
├── GameplayTags
└── ItemSystem  (private dependency, for AWorldItem)
```

## Plugin Structure

```
InteractionPlugin/
├── Source/InteractionSystem/
│   ├── Public/
│   │   ├── Components/        UInteractionComponent, UInteractableComponent
│   │   ├── Actors/            AWorldItem
│   │   ├── Detection/         Strategy base, sphere overlap, line trace
│   │   ├── Subsystems/        World item object pool
│   │   └── Types/             Interaction system types
│   └── Private/
├── Documentation/
│   └── INTERACTION_SYSTEM.md  Detailed system design
└── .claude/
    └── instructions.md        Claude Code implementation instructions
```

## Quick Start

### Make the Player an Interaction Source

Attach `UInteractionComponent` to your player character:

```cpp
UPROPERTY(VisibleAnywhere, BlueprintReadOnly)
TObjectPtr<UInteractionComponent> InteractionComponent;

// In constructor
InteractionComponent = CreateDefaultSubobject<UInteractionComponent>(TEXT("Interaction"));
InteractionComponent->InteractionRange = 300.0f;
InteractionComponent->DetectionTickRate = 0.1f;
```

### Make an Actor Interactable

**Option A: InteractableComponent (no C++ required)**

Attach `UInteractableComponent` to any actor. Configure in the editor:
```
InteractionOptions:
  [0] Type: Interaction.Type.Open
      Text: "Open Chest"
      bRequiresHold: true
      HoldDuration: 1.5
```

Bind to `OnInteracted` in Blueprint to define what happens.

**Option B: IInteractable interface (full control)**

Implement `ICGFInteractableInterface` on your actor class:

```cpp
bool AMyDoor::CanInteract_Implementation(AActor* Interactor, const FInteractionContext& Context) const
{
    return !bIsLocked;
}

TArray<FInteractionOption> AMyDoor::GetInteractionOptions_Implementation(AActor* Interactor) const
{
    FInteractionOption Option;
    Option.InteractionType = InteractionTags::Open;
    Option.DisplayText = bIsLocked ? LOCTEXT("Locked", "Locked") : LOCTEXT("Open", "Open Door");
    return { Option };
}

EInteractionResult AMyDoor::Interact_Implementation(AActor* Interactor, FGameplayTag InteractionType)
{
    OpenDoor();
    return EInteractionResult::Success;
}
```

### Bind UI to Interaction Events

```cpp
InteractionComponent->OnInteractableFound.AddDynamic(this, &AMyHUD::ShowInteractionPrompt);
InteractionComponent->OnInteractableLost.AddDynamic(this, &AMyHUD::HideInteractionPrompt);
InteractionComponent->OnChanneledProgress.AddDynamic(this, &AMyHUD::UpdateProgressBar);

// Get current options for prompt display
TArray<FInteractionOption> Options = InteractionComponent->GetCurrentInteractionOptions();
```

### Spawn World Items

```cpp
UWorldItemPoolSubsystem* Pool = GetWorld()->GetSubsystem<UWorldItemPoolSubsystem>();

AWorldItem* WorldItem = Pool->GetWorldItem();
WorldItem->SetActorLocation(DropLocation);
WorldItem->InitializeFromItem(DroppedItemInstance);
```

## Multiplayer

All interactions are server-validated. The client detects targets locally and sends RPC requests to the server. The server re-validates range, target state, and permissions before executing the interaction. Channeled interaction timers run on the server to prevent speed exploits. A 10% range tolerance accommodates network latency.

## Blueprint Support

All components, detection strategies, and interaction flow are fully Blueprint-exposed. Detection strategies are `EditInlineNew` objects configurable per-component in the editor. Interaction scoring is a `BlueprintNativeEvent` overridable for custom prioritization logic.

## Related Plugins

| Plugin | Integration |
|--------|------------|
| [CommonGameFramework](../CommonGameFramework/) | IInteractable, IInteractionSource interfaces (required) |
| [ItemInventoryPlugin](../ItemInventoryPlugin/) | AWorldItem uses item definitions and inventory operations |
| [EquipmentPlugin](../EquipmentPlugin/) | "Equip" interaction type triggers equipment system |

## Documentation

- [INTERACTION_SYSTEM.md](Documentation/INTERACTION_SYSTEM.md) — Detection algorithms, interaction flow, world item lifecycle, object pooling
- [ARCHITECTURE.md](../CommonGameFramework/Documentation/ARCHITECTURE.md) — System-wide architecture and integration patterns

## License

[Your license here]
