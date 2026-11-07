// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * Animation Blueprint graphs: AnimGraph, state graphs, custom transition graphs (UAnimationGraphSchema),
	 * transition rules (UAnimationTransitionSchema) and conduits (UAnimationConduitGraphSchema). These schemas
	 * derive from the K2 schema, so this adapter must be checked before the Blueprint one. State machine
	 * overview graphs (UAnimationStateMachineSchema) are not side-to-side graphs and are not supported.
	 *
	 * Poses flow into the Output Pose / State Result / Transition Result node, which stays put. Pose wires
	 * (UAnimationGraphSchema::IsPosePin) are the spine; nodes without pose pins (variables, math feeding
	 * properties like Alpha) hang as small trees below the anim node they feed, like Blueprint pure nodes.
	 */
	class FAnimGraphLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("AnimGraph"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSinks; }
		virtual bool IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const override;
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const override;
	};
}
