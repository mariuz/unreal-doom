using UnrealBuildTool;

// Doom's data without the game: no Engine dependency, so other projects (and tools) can use it.
public class DoomWadTools : ModuleRules
{
	public DoomWadTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib"); // compressed nodes (ZNOD, ZGLN...)
	}
}
