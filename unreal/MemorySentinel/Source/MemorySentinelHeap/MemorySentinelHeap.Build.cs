using UnrealBuildTool;

// The heap sampler. It loads at PostConfigInit, before most allocations, and holds no UObjects (UC-3).
public class MemorySentinelHeap : ModuleRules
{
	public MemorySentinelHeap(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
