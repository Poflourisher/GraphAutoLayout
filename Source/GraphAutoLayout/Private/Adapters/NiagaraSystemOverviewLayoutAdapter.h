// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * The Niagara System Overview: one System node and one tall node per emitter, with no wires between them.
	 * There is no flow to follow, so instead of the layered layout the nodes are put in one row: the System node
	 * first, then the emitters in the order of the system's emitter list, top edges aligned.
	 */
	class FNiagaraSystemOverviewLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("NiagaraSystemOverview"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSources; }
		virtual bool ComputeRowLayout(const TArray<UEdGraphNode*>& Nodes, const TArray<FVector2f>& Sizes, float Spacing, TArray<FVector2f>& OutPositions) const override;
	};
}
