using UnrealBuildTool;

// The editor tab and the Tools menu entry. The panel itself lives in the runtime module, so the same widget is the
// in-game overlay (UC-4.1).
public class MemorySentinelEditor : ModuleRules
{
	public MemorySentinelEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Slate", "SlateCore",
			"UnrealEd", "ToolMenus", "WorkspaceMenuStructure", "MemorySentinel" });
	}
}
