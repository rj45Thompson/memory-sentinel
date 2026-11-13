// Copyright (c) 2026 RJ Thompson. Released under the MIT License; see LICENSE.
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
