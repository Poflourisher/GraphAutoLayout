// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GraphLayoutTypes.h"

class SGraphPanel;
class UEdGraph;
class UEdGraphNode;
class UEdGraphNode_Comment;
class UEdGraphPin;

namespace UE::GraphAutoLayout
{
	struct FMeasuredNode;

	/**
	 * Per-graph-type policy. The runner does the generic work (measuring, building the abstract
	 * graph, running the engine, the transaction); an adapter only says how this graph type flows
	 * and what has to happen besides writing NodePosX/Y.
	 */
	class IGraphLayoutAdapter
	{
	public:
		virtual ~IGraphLayoutAdapter() = default;

		virtual FName GetName() const = 0;
		virtual bool Supports(const UEdGraph& Graph) const = 0;
		virtual bool IsEnabled() const = 0;
		virtual ELayeringMode GetLayeringMode() const = 0;

		/** Whether a wire from an output pin to an input pin is part of the main flow (exec, parameter map). */
		virtual bool IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const { return false; }

		/** Fill in satellite flag, anchor priority, layer constraint, etc. Size and pins are already set. */
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const {}

		/**
		 * Whether this adapter may run in the editor hosting Panel (e.g. only inside Niagara Scratch Pad).
		 * Only checked when there is a panel (the hotkey path); OutReason is shown to the user when false.
		 */
		virtual bool CheckHost(const SGraphPanel& Panel, FText& OutReason) const { return true; }

		/** Called after Modify() and NodePosX/Y were written, inside the transaction (e.g. sync material expressions). */
		virtual void PostMoveNode(UEdGraphNode& Node) const {}

		/** Called before a comment is moved/resized, so data mirrored elsewhere can be recorded for undo. */
		virtual void PreModifyComment(UEdGraphNode_Comment& Comment) const {}

		/**
		 * For graphs with no flow to follow (no wires), the adapter may place the nodes itself: fill OutPositions
		 * (one per node, top-left corners) and return true to use them instead of the layered layout.
		 */
		virtual bool ComputeRowLayout(const TArray<UEdGraphNode*>& Nodes, const TArray<FVector2f>& Sizes, float Spacing, TArray<FVector2f>& OutPositions) const { return false; }

		/** Called once inside the transaction after all nodes moved, if any did (e.g. flag the asset as changed). */
		virtual void PostLayout(UEdGraph& Graph, const TArray<UEdGraphNode*>& MovedNodes) const {}
	};

	/** All adapters, in priority order. */
	const TArray<TUniquePtr<IGraphLayoutAdapter>>& GetGraphLayoutAdapters();

	const IGraphLayoutAdapter* FindGraphLayoutAdapter(const UEdGraph& Graph);
}
