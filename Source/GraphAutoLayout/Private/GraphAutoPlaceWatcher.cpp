// Copyright 2026 Jot. All Rights Reserved.

#include "GraphAutoPlaceWatcher.h"

#include "EdGraph/EdGraph.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphAutoLayoutInputProcessor.h"
#include "GraphAutoLayoutLog.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphEditAction.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"

namespace UE::GraphAutoLayout::AutoPlacePrivate
{
	// Give up waiting for the new node's widget after this many frames and place it with an estimated size.
	constexpr int32 MaxFramesToWaitForWidget = 10;
}

void FGraphAutoPlaceWatcher::Start()
{
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGraphAutoPlaceWatcher::Tick));
}

void FGraphAutoPlaceWatcher::Stop()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	for (TPair<TWeakObjectPtr<UEdGraph>, FWatchedGraph>& Pair : Watched)
	{
		if (UEdGraph* Graph = Pair.Key.Get())
		{
			Graph->RemoveOnGraphChangedHandler(Pair.Value.Handle);
		}
	}
	Watched.Empty();
	Pending.Empty();
}

void FGraphAutoPlaceWatcher::WatchFocusedGraph()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	const TSharedPtr<SGraphPanel> Panel = FGraphAutoLayoutInputProcessor::FindFocusedGraphPanel(FSlateApplication::Get(), 0);
	UEdGraph* Graph = Panel.IsValid() ? Panel->GetGraphObj() : nullptr;
	if (!Graph)
	{
		return;
	}
	if (FWatchedGraph* Existing = Watched.Find(Graph))
	{
		Existing->Panel = Panel;
		return;
	}
	FWatchedGraph& Watch = Watched.Add(Graph);
	Watch.Panel = Panel;
	Watch.Handle = Graph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateRaw(this, &FGraphAutoPlaceWatcher::OnGraphChanged));
	GRAPHAUTOLAYOUT_DEBUG(TEXT("Auto place: watching graph %s"), *Graph->GetName());
}

void FGraphAutoPlaceWatcher::OnGraphChanged(const FEdGraphEditAction& Action)
{
	// Only nodes the user added through the UI (action menu, drag off a pin, shortcut spawn).
	if ((Action.Action & GRAPHACTION_AddNode) == 0 || !Action.bUserInvoked || !Action.Graph
		|| !GetDefault<UGraphAutoLayoutSettings>()->bAutoFormatOnNodePlaced)
	{
		return;
	}
	FPendingPlacement& Placement = Pending.FindOrAdd(Action.Graph);
	for (const UEdGraphNode* Node : Action.Nodes)
	{
		Placement.Nodes.Add(const_cast<UEdGraphNode*>(Node));
	}
	Placement.LastAddFrame = GFrameCounter;
	Placement.FramesWaited = 0;
}

bool FGraphAutoPlaceWatcher::Tick(float DeltaTime)
{
	using namespace UE::GraphAutoLayout::AutoPlacePrivate;

	if (!GetDefault<UGraphAutoLayoutSettings>()->bAutoFormatOnNodePlaced)
	{
		Pending.Empty();
		return true;
	}
	WatchFocusedGraph();

	for (auto It = Pending.CreateIterator(); It; ++It)
	{
		FPendingPlacement& Placement = It.Value();
		UEdGraph* Graph = It.Key().Get();
		// Wait at least one frame so auto-wiring and any other nodes of the same paste have arrived.
		if (!Graph || Placement.LastAddFrame == GFrameCounter)
		{
			if (!Graph)
			{
				It.RemoveCurrent();
			}
			continue;
		}
		if (Placement.Nodes.Num() != 1)
		{
			GRAPHAUTOLAYOUT_DEBUG(TEXT("Auto place: skipped %d nodes added together (paste/duplicate)"), Placement.Nodes.Num());
			It.RemoveCurrent();
			continue;
		}
		UEdGraphNode* Node = Placement.Nodes[0].Get();
		const FWatchedGraph* Watch = Watched.Find(Graph);
		const TSharedPtr<SGraphPanel> Panel = Watch ? Watch->Panel.Pin() : nullptr;
		if (!Node)
		{
			It.RemoveCurrent();
			continue;
		}

		// Wait for the widget so the real size is used, but never forever.
		const TSharedPtr<SGraphNode> Widget = Panel.IsValid() ? Panel->GetNodeWidgetFromGuid(Node->NodeGuid) : nullptr;
		const bool bWidgetReady = Widget.IsValid() && Widget->GetDesiredSize().X > 1.f;
		if (!bWidgetReady && ++Placement.FramesWaited < MaxFramesToWaitForWidget)
		{
			continue;
		}

		const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::PlaceNewNode(Graph, Node, Panel.Get());
		GRAPHAUTOLAYOUT_DEBUG(TEXT("Auto place %s: %s"), *Node->GetName(), Result.NumMoved > 0 ? TEXT("placed") : *Result.Message.ToString());
		It.RemoveCurrent();
	}

	// Forget graphs that were destroyed.
	for (auto It = Watched.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}
	return true;
}
