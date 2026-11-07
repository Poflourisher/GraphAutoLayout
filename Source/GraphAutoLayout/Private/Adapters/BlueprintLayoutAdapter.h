// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * K2 graphs (event graphs, functions, macros). Exec flow runs left to right as the main spine;
	 * nodes without exec pins (pure nodes, data reroutes) become satellites that hang as small trees
	 * to the left of, and below, the exec node that first consumes them.
	 */
	class FBlueprintLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("Blueprint"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSources; }
		virtual bool IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const override;
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const override;
	};
}
