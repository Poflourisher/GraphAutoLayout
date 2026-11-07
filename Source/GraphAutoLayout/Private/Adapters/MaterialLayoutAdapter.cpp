// Copyright 2026 Jot. All Rights Reserved.

#include "MaterialLayoutAdapter.h"

#include "GraphAutoLayoutSettings.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "Materials/MaterialExpressionComment.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionOutput.h"

namespace UE::GraphAutoLayout
{
	bool FMaterialLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		return Schema && Schema->IsA<UMaterialGraphSchema>();
	}

	bool FMaterialLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnableMaterial;
	}

	bool FMaterialLayoutAdapter::IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const
	{
		// Material execution-flow pins (UMaterialGraphSchema::PC_Exec) form the spine when present.
		return OutputPin.PinType.PinCategory == UMaterialGraphSchema::PC_Exec && InputPin.PinType.PinCategory == UMaterialGraphSchema::PC_Exec;
	}

	void FMaterialLayoutAdapter::ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const
	{
		// The output node stays put and everything else is arranged leftwards from it.
		if (Node.IsA<UMaterialGraphNode_Root>())
		{
			OutNode.AnchorPriority = 100;
			OutNode.LayerConstraint = ELayerConstraint::Last;
		}
		else if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(&Node))
		{
			if (MaterialNode->MaterialExpression && MaterialNode->MaterialExpression->IsA<UMaterialExpressionFunctionOutput>())
			{
				OutNode.AnchorPriority = 50;
				OutNode.LayerConstraint = ELayerConstraint::Last;
			}
		}
	}

	void FMaterialLayoutAdapter::PreModifyComment(UEdGraphNode_Comment& Comment) const
	{
		// UMaterialGraphNode_Comment::ResizeNode copies size and position into the comment expression and
		// flags the editor dirty; record the expression first so undo restores it too.
		if (UMaterialGraphNode_Comment* MaterialComment = Cast<UMaterialGraphNode_Comment>(&Comment))
		{
			if (MaterialComment->MaterialExpressionComment)
			{
				MaterialComment->MaterialExpressionComment->Modify();
			}
		}
	}

	void FMaterialLayoutAdapter::PostMoveNode(UEdGraphNode& Node) const
	{
		if (UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(&Node))
		{
			if (UMaterialExpression* Expression = MaterialNode->MaterialExpression)
			{
				// Record the expression for undo, but don't let Modify() flag its preview for a recompile:
				// moving a node doesn't change what it renders (the engine's own drag path doesn't either).
				const bool bNeededPreviewUpdate = Expression->bNeedToUpdatePreview;
				Expression->Modify();
				Expression->bNeedToUpdatePreview = bNeededPreviewUpdate;
				Expression->MaterialExpressionEditorX = Node.NodePosX;
				Expression->MaterialExpressionEditorY = Node.NodePosY;
			}
			MaterialNode->MaterialDirtyDelegate.ExecuteIfBound();
		}
		else if (UMaterialGraphNode_Root* RootNode = Cast<UMaterialGraphNode_Root>(&Node))
		{
			if (UMaterial* Material = RootNode->Material)
			{
				Material->Modify();
				Material->EditorX = Node.NodePosX;
				Material->EditorY = Node.NodePosY;
			}
			if (UMaterialGraph* Graph = Cast<UMaterialGraph>(Node.GetGraph()))
			{
				Graph->MaterialDirtyDelegate.ExecuteIfBound();
			}
		}
	}
}
