// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class SGraphPanel;
class UEdGraph;
class UEdGraphNode;

struct FGraphAutoLayoutRunResult
{
	bool bSucceeded = false;
	int32 NumLaidOut = 0;
	int32 NumMoved = 0;
	int32 NumCommentsResized = 0;
	/** Keep Together mode: comments whose nodes were laid out as a group (nested ones included). */
	int32 NumCommentGroups = 0;
	FName AdapterName;
	FText Message;

	/** Final bounds of every laid-out node and of every obstacle (unselected node), as used by the layout. */
	TMap<const UEdGraphNode*, FBox2f> NodeBounds;
	TMap<const UEdGraphNode*, FBox2f> ObstacleBounds;
};

/** Entry points shared by the hotkey, the context menu, auto-placement and the automation tests. */
class GRAPHAUTOLAYOUT_API FGraphAutoLayoutRunner
{
public:
	/**
	 * Lays out Nodes (comments are skipped) inside Graph as one undoable transaction, and resizes comments
	 * that contained moved nodes (per settings). In Keep Together mode each Group Movement comment's nodes are
	 * laid out as one block, and a selected Group Movement comment adds every node inside it. Panel is optional:
	 * with it, real widget sizes and pin positions are used; without it (e.g. headless tests) sizes are estimated
	 * from titles and pin counts.
	 */
	static FGraphAutoLayoutRunResult LayoutNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes, const SGraphPanel* Panel = nullptr);

	/**
	 * Snaps a newly added node next to the node it is wired to (straight wire, normal spacing), then nudges it
	 * up or down until it is clear of every other node. No other node moves. Does nothing for unconnected nodes.
	 */
	static FGraphAutoLayoutRunResult PlaceNewNode(UEdGraph* Graph, UEdGraphNode* NewNode, const SGraphPanel* Panel = nullptr);

	/** Whether Graph (shown in Panel, if given) has an enabled layout adapter. Used to show/hide the menu entry. */
	static bool CanLayout(const UEdGraph* Graph, const SGraphPanel* Panel);
};
