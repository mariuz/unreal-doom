using UnrealBuildTool;

public class DoomUEEditorTarget : TargetRules
{
	public DoomUEEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.AddRange(new string[] { "DoomUE", "DoomUEEditor" });
	}
}
