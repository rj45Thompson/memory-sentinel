using UnrealBuildTool;

public class SentinelTest : ModuleRules
{
	public SentinelTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.AddRange(new string[] { ModuleDirectory, System.IO.Path.Combine(ModuleDirectory, "Planted"),
			System.IO.Path.Combine(ModuleDirectory, "Clean"), System.IO.Path.Combine(ModuleDirectory, "Scenarios") });
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
		PrivateDependencyModuleNames.AddRange(new string[] { "MemorySentinel" });
	}
}
