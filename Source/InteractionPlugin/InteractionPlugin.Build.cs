using UnrealBuildTool;

public class InteractionPlugin : ModuleRules
{
	public InteractionPlugin(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"NetCore",
			"GameplayTags",
			"CommonGameFramework",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"ItemInventoryPlugin",
			"UMG",
			"Slate",
			"SlateCore",
		});
	}
}
