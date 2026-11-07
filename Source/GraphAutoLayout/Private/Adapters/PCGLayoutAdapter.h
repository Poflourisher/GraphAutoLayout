// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * PCG graphs. Data flows left to right from the graph's Input node, which stays put; the Output node
	 * is kept in the right-most column.
	 *
	 * The PCG editor classes (schema, editor nodes, editor graph) are not exported from PCGEditor, so the
	 * schema is recognised by class name and the runtime UPCGNode is read through the editor node's
	 * "PCGNode" UPROPERTY. Positions persist on the UPCGNode: we set it directly inside our transaction,
	 * mirroring FPCGEditor's own layout code, and the editor node's PostTransacted (UpdatePosition) does
	 * the same when the transaction finishes or is undone.
	 */
	class FPCGLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("PCG"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSources; }
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const override;
		virtual void PostMoveNode(UEdGraphNode& Node) const override;
	};
}
