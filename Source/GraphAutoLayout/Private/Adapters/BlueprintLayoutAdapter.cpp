// Copyright 2026 Jot. All Rights Reserved.

#include "BlueprintLayoutAdapter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphNodeMeasurer.h"

namespace UE::GraphAutoLayout
{
	static bool IsExecPin(const UEdGraphPin& Pin)
	{
		return Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	}

	bool FBlueprintLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		return Schema && Schema->IsA<UEdGraphSchema_K2>();
	}

	bool FBlueprintLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnableBlueprint;
	}

	bool FBlueprintLayoutAdapter::IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const
	{
		return IsExecPin(OutputPin) && IsExecPin(InputPin);
	}

	void FBlueprintLayoutAdapter::ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const
	{
		// Classify by pins rather than UK2Node::IsNodePure so knots, macros and custom nodes behave the
		// same way: anything that takes part in exec flow is on the spine.
		bool bHasExecInput = false;
		bool bHasExecOutput = false;
		float ExecBandBottom = 0.f;
		for (const FMeasuredPin& MeasuredPin : Measured.Pins)
		{
			if (!IsExecPin(*MeasuredPin.Pin))
			{
				continue;
			}
			if (MeasuredPin.Pin->Direction == EGPD_Input)
			{
				bHasExecInput = true;
				// A full row of clearance below the exec pin keeps the incoming exec wire visibly clear of satellites.
				ExecBandBottom = FMath::Max(ExecBandBottom, MeasuredPin.OffsetY + FGraphNodeMeasurer::PinRowHeight);
			}
			else
			{
				bHasExecOutput = true;
			}
		}

		OutNode.bSatellite = !bHasExecInput && !bHasExecOutput;
		OutNode.SatelliteMinTop = ExecBandBottom;

		// Events and function entries start the flow, so they are the natural anchor.
		if (bHasExecOutput && !bHasExecInput)
		{
			OutNode.AnchorPriority = 10;
		}
	}
}
