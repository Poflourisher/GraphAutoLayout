// Copyright 2026 Jot. All Rights Reserved.

#include "NiagaraScratchPadLayoutAdapter.h"

#include "EdGraphSchema_Niagara.h"
#include "GraphAutoLayoutLog.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphNodeMeasurer.h"
#include "NiagaraNode.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraTypes.h"
#include "SGraphPanel.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayout"

namespace UE::GraphAutoLayout
{
	namespace NiagaraPrivate
	{
		// Created with SNew in NiagaraSystemEditorDocumentsViewModel.cpp; the class itself is private to NiagaraEditor.
		static const FName ScratchPadEditorType(TEXT("SNiagaraScratchPadScriptEditor"));

		// UNiagaraNodeParameterMapGet is private to NiagaraEditor, so it is recognised by class name.
		static const FName MapGetClassName(TEXT("NiagaraNodeParameterMapGet"));

		static bool IsParameterMapPin(const UEdGraphPin& Pin)
		{
			// Same encoding as UEdGraphSchema_Niagara::TypeDefinitionToPinType for the parameter map struct.
			return Pin.PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryType
				&& Pin.PinType.PinSubCategoryObject == FNiagaraTypeDefinition::GetParameterMapStruct();
		}

		static bool IsA(const UClass* Class, FName ClassName)
		{
			for (; Class; Class = Class->GetSuperClass())
			{
				if (Class->GetFName() == ClassName)
				{
					return true;
				}
			}
			return false;
		}
	}

	bool FNiagaraScratchPadLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		return Schema && Schema->IsA<UEdGraphSchema_Niagara>();
	}

	bool FNiagaraScratchPadLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnableNiagaraScratchPad;
	}

	bool FNiagaraScratchPadLayoutAdapter::CheckHost(const SGraphPanel& Panel, FText& OutReason) const
	{
		for (TSharedPtr<const SWidget> Widget = Panel.GetParentWidget(); Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget->GetType() == NiagaraPrivate::ScratchPadEditorType)
			{
				return true;
			}
		}
		OutReason = LOCTEXT("NiagaraScratchPadOnly", "Auto layout only supports Niagara Scratch Pad graphs, not standalone Niagara script assets.");
		return false;
	}

	bool FNiagaraScratchPadLayoutAdapter::IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const
	{
		using namespace NiagaraPrivate;
		if (!IsParameterMapPin(OutputPin) || !IsParameterMapPin(InputPin))
		{
			return false;
		}
		// Map Get nodes only read from the map; keep them near the operations they feed instead of on the spine.
		const UEdGraphNode* Consumer = InputPin.GetOwningNodeUnchecked();
		return !(Consumer && IsA(Consumer->GetClass(), MapGetClassName));
	}

	void FNiagaraScratchPadLayoutAdapter::ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const
	{
		if (Node.IsA<UNiagaraNodeOutput>())
		{
			OutNode.AnchorPriority = 100;
			OutNode.LayerConstraint = ELayerConstraint::Last;
			return;
		}
		if (Node.IsA<UNiagaraNodeInput>())
		{
			// The InputMap node starts the parameter-map chain, so it belongs in the left-most column.
			for (const FMeasuredPin& MeasuredPin : Measured.Pins)
			{
				if (MeasuredPin.Pin->Direction == EGPD_Output && NiagaraPrivate::IsParameterMapPin(*MeasuredPin.Pin))
				{
					OutNode.LayerConstraint = ELayerConstraint::First;
					break;
				}
			}
		}
	}

	void FNiagaraScratchPadLayoutAdapter::PostLayout(UEdGraph& Graph, const TArray<UEdGraphNode*>& MovedNodes) const
	{
		if (!GetDefault<UGraphAutoLayoutSettings>()->bMarkNiagaraScratchPadChanged)
		{
			return;
		}
		// One node is enough: MarkNodeRequiresSynchronization(.., true) also gives the graph a new change id and
		// raises "needs recompile", which is what Scratch Pad's Apply and pending-changes state listen to.
		auto TryMark = [&Graph](UEdGraphNode* Node)
		{
			if (UNiagaraNode* NiagaraNode = Cast<UNiagaraNode>(Node))
			{
				NiagaraNode->MarkNodeRequiresSynchronization(TEXT("GraphAutoLayout moved nodes"), true);
				GRAPHAUTOLAYOUT_DEBUG(TEXT("Marked Niagara graph %s as changed so the layout is kept on Apply"), *Graph.GetName());
				return true;
			}
			return false;
		};
		for (UEdGraphNode* Node : MovedNodes)
		{
			if (TryMark(Node))
			{
				return;
			}
		}
		// Only comments changed: any Niagara node in the graph will do.
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (TryMark(Node))
			{
				return;
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
