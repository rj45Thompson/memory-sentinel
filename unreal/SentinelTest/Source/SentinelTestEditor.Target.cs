using UnrealBuildTool;

public class SentinelTestEditorTarget : TargetRules
{
	public SentinelTestEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SentinelTest");
	}
}
