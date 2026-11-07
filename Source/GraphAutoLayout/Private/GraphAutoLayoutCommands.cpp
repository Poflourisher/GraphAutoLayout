// Copyright 2026 Jot. All Rights Reserved.

#include "GraphAutoLayoutCommands.h"

#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayoutCommands"

FGraphAutoLayoutCommands::FGraphAutoLayoutCommands()
	: TCommands<FGraphAutoLayoutCommands>(
		TEXT("GraphAutoLayout"),
		LOCTEXT("ContextDescription", "Graph Auto Layout"),
		NAME_None,
		FAppStyle::GetAppStyleSetName())
{
}

void FGraphAutoLayoutCommands::RegisterCommands()
{
	UI_COMMAND(LayoutSelectedNodes, "Layout Selected Nodes",
		"Lays out the selected nodes of the focused graph. Fires when the key is released, and only if no mouse button was pressed while it was held, so hold-key-and-click node spawning keeps working.",
		EUserInterfaceActionType::Button, FInputChord(EKeys::L));
}

#undef LOCTEXT_NAMESPACE
