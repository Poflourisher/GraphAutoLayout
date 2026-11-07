// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * Material and Material Function graphs. Data flows into the sink: the root (material output) node,
	 * or FunctionOutput nodes in functions, or else whatever selected nodes have no selected consumer.
	 * Layers are counted back from the sink so heavy fan-in stacks to the left of its consumer.
	 *
	 * The Material Editor edits a transient copy of the material and only applies it back on Apply/Save
	 * when its dirty flag is set (FMaterialEditor::SaveAsset_Execute -> UpdateOriginalMaterial ->
	 * UMaterialGraph::LinkMaterialExpressionsFromGraph). We mirror SGraphNodeMaterialBase::MoveTo and
	 * SGraphNodeMaterialResult::MoveTo: sync the expression/material position and fire MaterialDirtyDelegate.
	 */
	class FMaterialLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("Material"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSinks; }
		virtual bool IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const override;
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const override;
		virtual void PostMoveNode(UEdGraphNode& Node) const override;
		virtual void PreModifyComment(UEdGraphNode_Comment& Comment) const override;
	};
}
