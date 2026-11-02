# InteractionPlugin

World interaction for Unreal Engine 5.8. `UInteractionComponent` on the player detects nearby interactables on a timer
(sphere overlap or camera trace, pluggable), scores them by distance, view angle and priority, and runs instant or
channeled interactions that the server re-validates; `UInteractableComponent` makes any actor interactable through
data-driven options and a handler. `AWorldItem` represents an `FItemInstance` in the world (async mesh, resting pose,
pickup into the interactor's inventory) and `UWorldItemPoolSubsystem` pools those actors. Requires CommonGameFramework
(`Plugins/CommonGameFramework/`) and ItemInventoryPlugin (`Plugins/ItemInventoryPlugin/`, used by `AWorldItem`).

- [INTERACTION_SYSTEM.md](Documentation/INTERACTION_SYSTEM.md) — the system design
- [.claude/instructions.md](.claude/instructions.md) — rules for working in this plugin
- `Plugins/CommonGameFramework/Documentation/ARCHITECTURE.md` — how the framework plugins fit together
