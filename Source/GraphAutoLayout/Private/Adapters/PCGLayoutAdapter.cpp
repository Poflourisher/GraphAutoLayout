// Copyright 2026 Jot. All Rights Reserved.

#include "PCGLayoutAdapter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "GraphAutoLayoutSettings.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "UObject/UnrealType.h"

namespace UE::GraphAutoLayout
{
	namespace PCGPrivate
	{
		// UPCGEditorGraphSchema (PCGEditor/Public/Schema/PCGEditorGraphSchema.h) is not exported, so match by name.
		static const FName SchemaClassName(TEXT("PCGEditorGraphSchema"));

		// UPROPERTY on UPCGEditorGraphNodeBase (PCGEditor/Public/Nodes/PCGEditorGraphNodeBase.h).
		static const FName PCGNodePropertyName(TEXT("PCGNode"));

		static UPCGNode* GetPCGNode(const UEdGraphNode& Node)
		{
			const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Node.GetClass(), PCGNodePropertyName);
			return Property ? Cast<UPCGNode>(Property->GetObjectPropertyValue_InContainer(&Node)) : nullptr;
		}
	}

	bool FPCGLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		for (const UClass* Class = Schema ? Schema->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetFName() == PCGPrivate::SchemaClassName)
			{
				return true;
			}
		}
		return false;
	}

	bool FPCGLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnablePCG;
	}

	void FPCGLayoutAdapter::ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const
	{
		const UPCGNode* PCGNode = PCGPrivate::GetPCGNode(Node);
		const UPCGGraph* PCGGraph = PCGNode ? PCGNode->GetGraph() : nullptr;
		if (!PCGGraph)
		{
			return;
		}
		if (PCGNode == PCGGraph->GetInputNode())
		{
			OutNode.AnchorPriority = 100;
			OutNode.LayerConstraint = ELayerConstraint::First;
		}
		else if (PCGNode == PCGGraph->GetOutputNode())
		{
			OutNode.AnchorPriority = 50;
			OutNode.LayerConstraint = ELayerConstraint::Last;
		}
	}

	void FPCGLayoutAdapter::PostMoveNode(UEdGraphNode& Node) const
	{
		if (UPCGNode* PCGNode = PCGPrivate::GetPCGNode(Node))
		{
			PCGNode->Modify();
			PCGNode->SetNodePosition(Node.NodePosX, Node.NodePosY);
		}
	}
}
