// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class SGraphPanel;
class UEdGraph;
class UEdGraphNode;
struct FEdGraphEditAction;

/**
 * "Auto Place New Nodes": watches graphs that get keyboard focus for nodes added from the UI and snaps a
 * single new node next to what it is wired to (FGraphAutoLayoutRunner::PlaceNewNode).
 *
 * Placement waits for the next frames rather than reacting inside the add-node notification: auto-wiring
 * happens right after the node is added, and the node's widget (needed for its real size) only exists once
 * the panel has refreshed. Batches of several nodes in one frame (paste, duplicate) are ignored.
 */
class FGraphAutoPlaceWatcher
{
public:
	void Start();
	void Stop();

private:
	bool Tick(float DeltaTime);
	void WatchFocusedGraph();
	void OnGraphChanged(const FEdGraphEditAction& Action);

	struct FWatchedGraph
	{
		FDelegateHandle Handle;
		TWeakPtr<SGraphPanel> Panel;
	};

	struct FPendingPlacement
	{
		TArray<TWeakObjectPtr<UEdGraphNode>> Nodes;
		uint64 LastAddFrame = 0;
		int32 FramesWaited = 0;
	};

	FTSTicker::FDelegateHandle TickerHandle;
	TMap<TWeakObjectPtr<UEdGraph>, FWatchedGraph> Watched;
	TMap<TWeakObjectPtr<UEdGraph>, FPendingPlacement> Pending;
};
