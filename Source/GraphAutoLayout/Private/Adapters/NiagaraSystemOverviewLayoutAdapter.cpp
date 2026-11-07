// Copyright 2026 Jot. All Rights Reserved.

#include "NiagaraSystemOverviewLayoutAdapter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_NiagaraSystemOverview.h"
#include "GraphAutoLayoutSettings.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraOverviewNode.h"
#include "NiagaraSystem.h"

namespace UE::GraphAutoLayout
{
	namespace OverviewPrivate
	{
		/** The System node first, then emitters in emitter-list order, then anything else (left to right as they were). */
		static int64 SortKey(const UEdGraphNode& Node)
		{
			constexpr int64 OtherNodes = 1'000'000;
			const UNiagaraOverviewNode* Overview = Cast<UNiagaraOverviewNode>(&Node);
			if (!Overview)
			{
				return OtherNodes * 2 + Node.NodePosX;
			}
			const FGuid EmitterId = Overview->GetEmitterHandleGuid();
			if (!EmitterId.IsValid())
			{
				return -1; // The System node.
			}
			if (const UNiagaraSystem* System = Overview->GetOwningSystem())
			{
				const int32 Index = System->GetEmitterHandles().IndexOfByPredicate([&EmitterId](const FNiagaraEmitterHandle& Handle) { return Handle.GetId() == EmitterId; });
				if (Index != INDEX_NONE)
				{
					return Index;
				}
			}
			return OtherNodes + Node.NodePosX;
		}
	}

	bool FNiagaraSystemOverviewLayoutAdapter::Supports(const UEdGraph& Graph) const
	{
		const UEdGraphSchema* Schema = Graph.GetSchema();
		return Schema && Schema->IsA<UEdGraphSchema_NiagaraSystemOverview>();
	}

	bool FNiagaraSystemOverviewLayoutAdapter::IsEnabled() const
	{
		return GetDefault<UGraphAutoLayoutSettings>()->bEnableNiagaraSystemOverview;
	}

	bool FNiagaraSystemOverviewLayoutAdapter::ComputeRowLayout(const TArray<UEdGraphNode*>& Nodes, const TArray<FVector2f>& Sizes, float Spacing, TArray<FVector2f>& OutPositions) const
	{
		OutPositions.SetNum(Nodes.Num());
		if (Nodes.Num() == 0)
		{
			return true;
		}

		TArray<int32> Order;
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			Order.Add(Index);
		}
		Order.StableSort([&Nodes](int32 A, int32 B) { return OverviewPrivate::SortKey(*Nodes[A]) < OverviewPrivate::SortKey(*Nodes[B]); });

		// The row starts where the first node in the new order already is, so the view doesn't jump away.
		const UEdGraphNode& First = *Nodes[Order[0]];
		float X = static_cast<float>(First.NodePosX);
		const float Y = static_cast<float>(First.NodePosY);
		for (const int32 Index : Order)
		{
			OutPositions[Index] = FVector2f(X, Y);
			X += Sizes[Index].X + Spacing;
		}
		return true;
	}
}
