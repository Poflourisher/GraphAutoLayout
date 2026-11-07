// Copyright 2026 Jot. All Rights Reserved.

using UnrealBuildTool;

// Pure layout algorithm. Deliberately depends on Core only so it stays independent of Slate,
// UObjects and any specific graph type (and can be tested with synthetic graphs).
public class GraphAutoLayoutCore : ModuleRules
{
	public GraphAutoLayoutCore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivatePCHHeaderFile = "Private/GraphAutoLayoutCorePCH.h";

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
