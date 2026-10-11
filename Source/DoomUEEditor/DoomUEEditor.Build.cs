using UnrealBuildTool;

public class DoomUEEditor : ModuleRules
{
	public DoomUEEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] {
			"Core", "CoreUObject", "Engine", "UnrealEd", "MaterialEditor", "AssetRegistry", "DoomUE", "Niagara", "NiagaraCore", "ToolMenus", "Slate", "SlateCore", "Json"
		});
	}
}
