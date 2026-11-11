using UnrealBuildTool;

// The level leak test, the heap findings writer, the panel and the in-game overlay (UC-2, UC-3.7, UC-4).
public class MemorySentinel : ModuleRules
{
	public MemorySentinel(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Slate", "SlateCore" });
		PrivateDependencyModuleNames.AddRange(new string[] { "MemorySentinelHeap", "Json", "Projects", "InputCore", "ApplicationCore" });
	}
}
