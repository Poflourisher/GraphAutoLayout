// Copyright 2026 Jot. All Rights Reserved.

#include "AnimGraphLayoutAdapter.h"

#include "AnimationConduitGraphSchema.h"
#include "AnimationGraphSchema.h"
#include "AnimationTransitionSchema.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphNodeMeasurer.h"

namespace UE::GraphAutoLayout
{
	static bool IsPosePin(const UEdGraphPin& Pin)
	{
		return UAnimationGraphSchema::IsPosePin(Pin.PinType);
	}

	bool FAnimGraphLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		return Schema && (Schema->IsA<UAnimationGraphSchema>() || Schema->IsA<UAnimationTransitionSchema>() || Schema->IsA<UAnimationConduitGraphSchema>());
	}

	bool FAnimGraphLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnableAnimGraph;
	}

	bool FAnimGraphLayoutAdapter::IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const
	{
		return IsPosePin(OutputPin) && IsPosePin(InputPin);
	}

	void FAnimGraphLayoutAdapter::ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const
	{
		// Result nodes (Output Pose, State Result incl. custom transitions, Transition Result) are the sink and stay put.
		if (Node.IsA<UAnimGraphNode_Root>() || Node.IsA<UAnimGraphNode_StateResult>() || Node.IsA<UAnimGraphNode_TransitionResult>())
		{
			OutNode.AnchorPriority = 100;
			OutNode.LayerConstraint = ELayerConstraint::Last;
			return;
		}

		bool bHasPosePin = false;
		float PoseBandBottom = 0.f;
		for (const FMeasuredPin& MeasuredPin : Measured.Pins)
		{
			if (IsPosePin(*MeasuredPin.Pin))
			{
				bHasPosePin = true;
				if (MeasuredPin.Pin->Direction == EGPD_Input)
				{
					// Keep property trees below the incoming pose wires, which run left from these pins.
					PoseBandBottom = FMath::Max(PoseBandBottom, MeasuredPin.OffsetY + FGraphNodeMeasurer::PinRowHeight);
				}
			}
		}
		OutNode.bSatellite = !bHasPosePin;
		OutNode.SatelliteMinTop = PoseBandBottom;
	}
}
