using UnrealBuildTool;

public class SentinelTestTarget : TargetRules
{
	public SentinelTestTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("SentinelTest");
	}
}
