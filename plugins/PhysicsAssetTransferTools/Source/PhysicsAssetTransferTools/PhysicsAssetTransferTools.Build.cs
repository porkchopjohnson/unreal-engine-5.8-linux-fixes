using UnrealBuildTool;

public class PhysicsAssetTransferTools : ModuleRules
{
	public PhysicsAssetTransferTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// This custom plugin is built against the existing precompiled editor.
		bUsePrecompiled = false;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"UnrealEd", "PhysicsAssetEditor", "Persona", "SkeletonEditor",
			"Slate", "SlateCore", "ToolMenus", "PropertyEditor", "AssetRegistry"
		});
	}
}
