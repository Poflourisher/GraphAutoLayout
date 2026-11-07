// Copyright 2026 Jot. All Rights Reserved.

using UnrealBuildTool;

public class GraphAutoLayoutTests : ModuleRules
{
	public GraphAutoLayoutTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivatePCHHeaderFile = "Private/GraphAutoLayoutTestsPCH.h";

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"BlueprintGraph",
			"AnimGraph",
			"MaterialEditor", // UMaterialEditingLibrary
			"Niagara",
			"NiagaraCore",
			"NiagaraEditor",
			"GraphAutoLayoutCore",
			"GraphAutoLayout",
		});
	}
}
