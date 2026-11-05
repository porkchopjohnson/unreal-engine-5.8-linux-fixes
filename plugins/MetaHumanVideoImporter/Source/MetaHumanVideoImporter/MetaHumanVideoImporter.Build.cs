using UnrealBuildTool;

public class MetaHumanVideoImporter : ModuleRules
{
	public MetaHumanVideoImporter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[]
		{
			"Core", "CoreUObject", "Engine", "UnrealEd", "Slate", "SlateCore", "InputCore",
			"ToolMenus", "DesktopPlatform", "AssetTools", "AssetRegistry", "ContentBrowser",
			"Json", "ImgMedia", "MediaAssets", "CaptureDataCore", "MetaHumanPerformance",
			"MetaHumanPipeline", "MetaHumanPipelineCore", "MetaHumanCoreTech", "ControlRig",
			"RigVM", "NNE", "MetaHumanCore", "MetaHumanBodyTrackerInterface"
		});
	}
}
