using UnrealBuildTool;

public class DoomLyraToolsRuntime : ModuleRules
{
	public DoomLyraToolsRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
