using UnrealBuildTool;

public class DoomUE : ModuleRules
{
	public DoomUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput",
			"ProceduralMeshComponent", "DeveloperSettings", "AudioMixer", "AudioExtensions", "AIModule",
			"MeshDescription", "StaticMeshDescription", "Niagara"
		});
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI", "Slate", "SlateCore" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
	}
}
