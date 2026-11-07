// Copyright 2026 Jot. All Rights Reserved.

using UnrealBuildTool;

public class GraphAutoLayout : ModuleRules
{
	public GraphAutoLayout(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivatePCHHeaderFile = "Private/GraphAutoLayoutPCH.h";

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"GraphAutoLayoutCore",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"ToolMenus",      // right-click "Auto Layout Selected"
			"Slate",
			"SlateCore",
			"UnrealEd",       // FScopedTransaction, UEdGraphNode_Comment, MaterialGraph
			"GraphEditor",    // SGraphPanel, SGraphNode, SGraphPin
			"BlueprintGraph", // UEdGraphSchema_K2
			"AnimGraph",      // Animation Blueprint schemas and result nodes
			"Niagara",        // FNiagaraTypeDefinition
			"NiagaraCore",
			"NiagaraEditor",  // UNiagaraGraph schema/nodes (public headers only)
			"PCG",            // UPCGNode / UPCGGraph (runtime); PCG editor classes aren't exported
		});
	}
}
