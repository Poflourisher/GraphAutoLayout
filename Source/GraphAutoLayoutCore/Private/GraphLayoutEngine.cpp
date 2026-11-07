// Copyright 2026 Jot. All Rights Reserved.

#include "GraphLayoutEngine.h"

#include "HAL/PlatformTime.h"

namespace UE::GraphAutoLayout
{
namespace LayoutPrivate
{
	/** Node as seen by the layered layout. Can be a real node, a block of nodes, or a dummy. */
	struct FLNode
	{
		FVector2f Size = FVector2f::ZeroVector;
		FVector2f OrigPos = FVector2f::ZeroVector;
		TArray<float> PinOffsets;
		ELayerConstraint Constraint = ELayerConstraint::None;
		bool bDummy = false;
	};

	struct FLEdge
	{
		int32 Src = INDEX_NONE;
		int32 SrcPin = INDEX_NONE;
		int32 Dst = INDEX_NONE;
		int32 DstPin = INDEX_NONE;
		float Weight = 1.f;
	};

	struct FLSettings
	{
		ELayeringMode Mode = ELayeringMode::FromSources;
		float HSpacing = 80.f;
		float VSpacing = 40.f;
		int32 CrossingSweeps = 8;
		int32 AlignmentSweeps = 8;
	};

	struct FLOutput
	{
		TArray<FVector2f> Positions;
		TArray<int32> Layers;
		int32 NumLayers = 0;
		int32 NumDummies = 0;
		int32 NumReversed = 0;
		int64 CrossingsBefore = 0;
		int64 CrossingsAfter = 0;
	};

	static float PinOffset(const FLNode& Node, int32 Pin)
	{
		// Unknown pins attach to the vertical centre, which is the least surprising default.
		return Node.PinOffsets.IsValidIndex(Pin) ? Node.PinOffsets[Pin] : Node.Size.Y * 0.5f;
	}

	/**
	 * DFS-based cycle breaking. Returns per edge whether it must be reversed to make the graph acyclic.
	 * Roots are visited left-to-right (true sources first), so in a loop the edge that already points
	 * backwards on screen is the one that gets reversed. Iterative to survive deep graphs. O(V + E).
	 */
	static TArray<bool> FindBackEdges(const TArray<FLNode>& Nodes, const TArray<FLEdge>& Edges)
	{
		const int32 NumNodes = Nodes.Num();
		TArray<TArray<int32>> OutEdges;
		OutEdges.SetNum(NumNodes);
		TArray<int32> InDegree;
		InDegree.Init(0, NumNodes);
		for (int32 EdgeIdx = 0; EdgeIdx < Edges.Num(); ++EdgeIdx)
		{
			OutEdges[Edges[EdgeIdx].Src].Add(EdgeIdx);
			++InDegree[Edges[EdgeIdx].Dst];
		}

		TArray<int32> RootOrder;
		RootOrder.Reserve(NumNodes);
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			RootOrder.Add(Idx);
		}
		RootOrder.StableSort([&](int32 A, int32 B)
		{
			const bool bSourceA = InDegree[A] == 0;
			const bool bSourceB = InDegree[B] == 0;
			if (bSourceA != bSourceB)
			{
				return bSourceA;
			}
			if (Nodes[A].OrigPos.X != Nodes[B].OrigPos.X)
			{
				return Nodes[A].OrigPos.X < Nodes[B].OrigPos.X;
			}
			return Nodes[A].OrigPos.Y < Nodes[B].OrigPos.Y;
		});

		enum EState : uint8 { White, Grey, Black };
		TArray<uint8> State;
		State.Init(White, NumNodes);
		TArray<bool> bBack;
		bBack.Init(false, Edges.Num());

		TArray<TPair<int32, int32>> Stack; // (node, next out-edge cursor)
		for (int32 Root : RootOrder)
		{
			if (State[Root] != White)
			{
				continue;
			}
			State[Root] = Grey;
			Stack.Push({ Root, 0 });
			while (Stack.Num() > 0)
			{
				const int32 Node = Stack.Last().Key;
				const int32 Cursor = Stack.Last().Value;
				if (Cursor < OutEdges[Node].Num())
				{
					++Stack.Last().Value;
					const int32 EdgeIdx = OutEdges[Node][Cursor];
					const int32 Next = Edges[EdgeIdx].Dst;
					if (State[Next] == Grey)
					{
						bBack[EdgeIdx] = true;
					}
					else if (State[Next] == White)
					{
						State[Next] = Grey;
						Stack.Push({ Next, 0 });
					}
				}
				else
				{
					State[Node] = Black;
					Stack.Pop(EAllowShrinking::No);
				}
			}
		}
		return bBack;
	}

	/** Longest-path layering on a DAG. O(V + E). */
	static TArray<int32> AssignLayers(const TArray<FLNode>& Nodes, const TArray<FLEdge>& DagEdges, ELayeringMode Mode, int32& OutNumLayers)
	{
		const int32 NumNodes = Nodes.Num();
		TArray<TArray<int32>> Succ;
		TArray<TArray<int32>> Pred;
		Succ.SetNum(NumNodes);
		Pred.SetNum(NumNodes);
		TArray<int32> InDegree;
		InDegree.Init(0, NumNodes);
		for (const FLEdge& Edge : DagEdges)
		{
			Succ[Edge.Src].Add(Edge.Dst);
			Pred[Edge.Dst].Add(Edge.Src);
			++InDegree[Edge.Dst];
		}

		// Kahn's algorithm; index order keeps the result deterministic.
		TArray<int32> Topo;
		Topo.Reserve(NumNodes);
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			if (InDegree[Idx] == 0)
			{
				Topo.Add(Idx);
			}
		}
		for (int32 Head = 0; Head < Topo.Num(); ++Head)
		{
			for (int32 Next : Succ[Topo[Head]])
			{
				if (--InDegree[Next] == 0)
				{
					Topo.Add(Next);
				}
			}
		}
		check(Topo.Num() == NumNodes); // Cycle breaking guarantees a DAG.

		TArray<int32> Layer;
		Layer.Init(0, NumNodes);
		int32 MaxLayer = 0;
		if (Mode == ELayeringMode::FromSources)
		{
			for (int32 Node : Topo)
			{
				for (int32 P : Pred[Node])
				{
					Layer[Node] = FMath::Max(Layer[Node], Layer[P] + 1);
				}
				MaxLayer = FMath::Max(MaxLayer, Layer[Node]);
			}
		}
		else
		{
			// Depth = longest distance to a sink. A node with several consumers ends up just left of
			// the leftmost one, so every edge still points right (handles heavy fan-in / fan-out).
			TArray<int32> Depth;
			Depth.Init(0, NumNodes);
			for (int32 TopoIdx = Topo.Num() - 1; TopoIdx >= 0; --TopoIdx)
			{
				const int32 Node = Topo[TopoIdx];
				for (int32 S : Succ[Node])
				{
					Depth[Node] = FMath::Max(Depth[Node], Depth[S] + 1);
				}
				MaxLayer = FMath::Max(MaxLayer, Depth[Node]);
			}
			for (int32 Idx = 0; Idx < NumNodes; ++Idx)
			{
				Layer[Idx] = MaxLayer - Depth[Idx];
			}
		}

		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			if (Nodes[Idx].Constraint == ELayerConstraint::First && Pred[Idx].Num() == 0)
			{
				Layer[Idx] = 0;
			}
			else if (Nodes[Idx].Constraint == ELayerConstraint::Last && Succ[Idx].Num() == 0)
			{
				Layer[Idx] = MaxLayer;
			}
		}

		OutNumLayers = MaxLayer + 1;
		return Layer;
	}

	/** Number of inversions in Values (merge sort). Used to count crossings in O(E log E). */
	static int64 CountInversions(TArray<float>& Values, TArray<float>& Scratch, int32 Lo, int32 Hi)
	{
		if (Hi - Lo < 2)
		{
			return 0;
		}
		const int32 Mid = (Lo + Hi) / 2;
		int64 Count = CountInversions(Values, Scratch, Lo, Mid) + CountInversions(Values, Scratch, Mid, Hi);
		int32 I = Lo, J = Mid, K = Lo;
		while (I < Mid && J < Hi)
		{
			if (Values[J] < Values[I])
			{
				Count += Mid - I;
				Scratch[K++] = Values[J++];
			}
			else
			{
				Scratch[K++] = Values[I++];
			}
		}
		while (I < Mid) { Scratch[K++] = Values[I++]; }
		while (J < Hi) { Scratch[K++] = Values[J++]; }
		for (int32 Idx = Lo; Idx < Hi; ++Idx)
		{
			Values[Idx] = Scratch[Idx];
		}
		return Count;
	}

	/**
	 * Weighted isotonic regression (non-decreasing fit) with pool-adjacent-violators. O(n).
	 * This is the exact least-squares answer to "move these ordered nodes as little as possible
	 * from where they want to be, without overlapping".
	 */
	static void IsotonicRegression(const TArray<float>& Values, const TArray<float>& Weights, TArray<float>& Out)
	{
		struct FPool { double SumW; double SumWV; int32 Count; };
		TArray<FPool, TInlineAllocator<64>> Pools;
		for (int32 Idx = 0; Idx < Values.Num(); ++Idx)
		{
			Pools.Add({ Weights[Idx], Weights[Idx] * Values[Idx], 1 });
			while (Pools.Num() >= 2)
			{
				const FPool& A = Pools[Pools.Num() - 2];
				const FPool& B = Pools.Last();
				if (A.SumWV / A.SumW <= B.SumWV / B.SumW)
				{
					break;
				}
				const FPool Merged{ A.SumW + B.SumW, A.SumWV + B.SumWV, A.Count + B.Count };
				Pools.Pop(EAllowShrinking::No);
				Pools.Last() = Merged;
			}
		}
		Out.SetNumUninitialized(Values.Num());
		int32 Cursor = 0;
		for (const FPool& Pool : Pools)
		{
			const float Mean = static_cast<float>(Pool.SumWV / Pool.SumW);
			for (int32 Idx = 0; Idx < Pool.Count; ++Idx)
			{
				Out[Cursor++] = Mean;
			}
		}
	}

	/** Lower weighted median; with a dominant primary edge this is exactly that edge's target. */
	static float WeightedMedian(TArray<TPair<float, float>>& ValueWeight)
	{
		ValueWeight.Sort([](const TPair<float, float>& A, const TPair<float, float>& B) { return A.Key < B.Key; });
		float Total = 0.f;
		for (const TPair<float, float>& VW : ValueWeight)
		{
			Total += VW.Value;
		}
		float Acc = 0.f;
		for (const TPair<float, float>& VW : ValueWeight)
		{
			Acc += VW.Value;
			if (Acc >= Total * 0.5f)
			{
				return VW.Key;
			}
		}
		return ValueWeight.Last().Key;
	}

	/** Full layered layout of one node set. Returned positions are relative (arbitrary origin). */
	static FLOutput LayoutLayered(const TArray<FLNode>& InNodes, const TArray<FLEdge>& InEdges, const FLSettings& Settings)
	{
		FLOutput Out;
		const int32 NumReal = InNodes.Num();
		if (NumReal == 0)
		{
			return Out;
		}

		// 1. Cycle breaking. Pins swap with endpoints so straightening still targets the real pins.
		const TArray<bool> bReversed = FindBackEdges(InNodes, InEdges);
		TArray<FLEdge> Dag;
		Dag.Reserve(InEdges.Num());
		for (int32 EdgeIdx = 0; EdgeIdx < InEdges.Num(); ++EdgeIdx)
		{
			FLEdge Edge = InEdges[EdgeIdx];
			if (bReversed[EdgeIdx])
			{
				Swap(Edge.Src, Edge.Dst);
				Swap(Edge.SrcPin, Edge.DstPin);
				++Out.NumReversed;
			}
			Dag.Add(Edge);
		}

		// 2. Layering.
		int32 NumLayers = 0;
		const TArray<int32> RealLayers = AssignLayers(InNodes, Dag, Settings.Mode, NumLayers);

		// 3. Dummy nodes on long edges so ordering and alignment reserve a lane for the wire.
		TArray<FLNode> Nodes = InNodes;
		TArray<int32> NodeLayer = RealLayers;
		TArray<FLEdge> Segs;
		Segs.Reserve(Dag.Num());
		for (const FLEdge& Edge : Dag)
		{
			const int32 Span = NodeLayer[Edge.Dst] - NodeLayer[Edge.Src];
			if (!ensure(Span >= 1))
			{
				continue;
			}
			if (Span == 1)
			{
				Segs.Add(Edge);
				continue;
			}
			int32 Prev = Edge.Src;
			int32 PrevPin = Edge.SrcPin;
			const float Y0 = InNodes[Edge.Src].OrigPos.Y;
			const float Y1 = InNodes[Edge.Dst].OrigPos.Y;
			for (int32 Step = 1; Step < Span; ++Step)
			{
				FLNode Dummy;
				Dummy.bDummy = true;
				Dummy.PinOffsets.Add(0.f);
				Dummy.OrigPos = FVector2f(0.f, FMath::Lerp(Y0, Y1, static_cast<float>(Step) / Span));
				const int32 DummyIdx = Nodes.Add(Dummy);
				NodeLayer.Add(NodeLayer[Edge.Src] + Step);
				Segs.Add({ Prev, PrevPin, DummyIdx, 0, Edge.Weight });
				Prev = DummyIdx;
				PrevPin = 0;
			}
			Segs.Add({ Prev, PrevPin, Edge.Dst, Edge.DstPin, Edge.Weight });
		}
		Out.NumDummies = Nodes.Num() - NumReal;

		const int32 NumNodes = Nodes.Num();
		TArray<TArray<int32>> InSegs;
		TArray<TArray<int32>> OutSegs;
		InSegs.SetNum(NumNodes);
		OutSegs.SetNum(NumNodes);
		for (int32 SegIdx = 0; SegIdx < Segs.Num(); ++SegIdx)
		{
			OutSegs[Segs[SegIdx].Src].Add(SegIdx);
			InSegs[Segs[SegIdx].Dst].Add(SegIdx);
		}

		TArray<TArray<int32>> Layers;
		Layers.SetNum(NumLayers);
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			Layers[NodeLayer[Idx]].Add(Idx);
		}
		// Initial order follows the current on-screen order, so re-running the layout is stable.
		for (TArray<int32>& Layer : Layers)
		{
			Layer.StableSort([&](int32 A, int32 B) { return Nodes[A].OrigPos.Y < Nodes[B].OrigPos.Y; });
		}

		// Dummies only need room for a wire, not a full node gap.
		auto Gap = [&](int32 A, int32 B)
		{
			const int32 NumDummy = (Nodes[A].bDummy ? 1 : 0) + (Nodes[B].bDummy ? 1 : 0);
			return NumDummy == 0 ? Settings.VSpacing : (NumDummy == 1 ? Settings.VSpacing * 0.5f : Settings.VSpacing * 0.25f);
		};

		TArray<float> Y;
		Y.Init(0.f, NumNodes);
		auto StackLayer = [&](int32 LayerIdx)
		{
			float Cursor = 0.f;
			const TArray<int32>& Layer = Layers[LayerIdx];
			for (int32 Idx = 0; Idx < Layer.Num(); ++Idx)
			{
				Y[Layer[Idx]] = Cursor;
				Cursor += Nodes[Layer[Idx]].Size.Y + (Idx + 1 < Layer.Num() ? Gap(Layer[Idx], Layer[Idx + 1]) : 0.f);
			}
		};

		// 4. Crossing reduction (barycenter on real pin heights, alternating sweeps, keep the best).
		TArray<float> CrossValues;
		TArray<float> CrossScratch;
		auto CountCrossings = [&]() -> int64
		{
			int64 Total = 0;
			for (int32 LayerIdx = 0; LayerIdx + 1 < NumLayers; ++LayerIdx)
			{
				TArray<TPair<float, float>> Pairs;
				for (int32 Node : Layers[LayerIdx])
				{
					for (int32 SegIdx : OutSegs[Node])
					{
						const FLEdge& Seg = Segs[SegIdx];
						Pairs.Add({ Y[Seg.Src] + PinOffset(Nodes[Seg.Src], Seg.SrcPin), Y[Seg.Dst] + PinOffset(Nodes[Seg.Dst], Seg.DstPin) });
					}
				}
				Pairs.Sort([](const TPair<float, float>& A, const TPair<float, float>& B)
				{
					return A.Key != B.Key ? A.Key < B.Key : A.Value < B.Value;
				});
				CrossValues.Reset();
				for (const TPair<float, float>& Pair : Pairs)
				{
					CrossValues.Add(Pair.Value);
				}
				CrossScratch.SetNumUninitialized(CrossValues.Num());
				Total += CountInversions(CrossValues, CrossScratch, 0, CrossValues.Num());
			}
			return Total;
		};

		auto Reorder = [&](int32 LayerIdx, bool bUseIncoming)
		{
			TArray<int32>& Layer = Layers[LayerIdx];
			TArray<TPair<float, int32>> Keys;
			Keys.Reserve(Layer.Num());
			for (int32 Idx = 0; Idx < Layer.Num(); ++Idx)
			{
				const int32 Node = Layer[Idx];
				float Sum = 0.f;
				float WeightSum = 0.f;
				for (int32 SegIdx : (bUseIncoming ? InSegs[Node] : OutSegs[Node]))
				{
					const FLEdge& Seg = Segs[SegIdx];
					const int32 Other = bUseIncoming ? Seg.Src : Seg.Dst;
					const float OtherPinY = Y[Other] + PinOffset(Nodes[Other], bUseIncoming ? Seg.SrcPin : Seg.DstPin);
					const float OwnPin = PinOffset(Nodes[Node], bUseIncoming ? Seg.DstPin : Seg.SrcPin);
					Sum += Seg.Weight * (OtherPinY - OwnPin);
					WeightSum += Seg.Weight;
				}
				// Unconnected nodes keep their current slot as key, so they don't drift around.
				Keys.Add({ WeightSum > 0.f ? Sum / WeightSum : Y[Node], Idx });
			}
			Keys.StableSort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
			TArray<int32> NewOrder;
			NewOrder.Reserve(Layer.Num());
			for (const TPair<float, int32>& Key : Keys)
			{
				NewOrder.Add(Layer[Key.Value]);
			}
			Layer = MoveTemp(NewOrder);
			StackLayer(LayerIdx);
		};

		for (int32 LayerIdx = 0; LayerIdx < NumLayers; ++LayerIdx)
		{
			StackLayer(LayerIdx);
		}
		int64 BestCrossings = CountCrossings();
		Out.CrossingsBefore = BestCrossings;
		TArray<TArray<int32>> BestLayers = Layers;
		for (int32 Sweep = 0; Sweep < Settings.CrossingSweeps && BestCrossings > 0; ++Sweep)
		{
			if (Sweep % 2 == 0)
			{
				for (int32 LayerIdx = 1; LayerIdx < NumLayers; ++LayerIdx)
				{
					Reorder(LayerIdx, true);
				}
			}
			else
			{
				for (int32 LayerIdx = NumLayers - 2; LayerIdx >= 0; --LayerIdx)
				{
					Reorder(LayerIdx, false);
				}
			}
			const int64 Crossings = CountCrossings();
			if (Crossings < BestCrossings)
			{
				BestCrossings = Crossings;
				BestLayers = Layers;
			}
		}
		Layers = MoveTemp(BestLayers);
		for (int32 LayerIdx = 0; LayerIdx < NumLayers; ++LayerIdx)
		{
			StackLayer(LayerIdx);
		}
		Out.CrossingsAfter = BestCrossings;

		// 5. X: one column per layer, as wide as its widest node. Sink-anchored layouts right-align
		// so outputs line up towards their consumers; source-anchored layouts left-align inputs.
		TArray<float> X;
		X.Init(0.f, NumNodes);
		{
			float Cursor = 0.f;
			for (int32 LayerIdx = 0; LayerIdx < NumLayers; ++LayerIdx)
			{
				float Width = 0.f;
				for (int32 Node : Layers[LayerIdx])
				{
					Width = FMath::Max(Width, Nodes[Node].Size.X);
				}
				for (int32 Node : Layers[LayerIdx])
				{
					X[Node] = Settings.Mode == ELayeringMode::FromSinks ? Cursor + Width - Nodes[Node].Size.X : Cursor;
				}
				Cursor += Width + Settings.HSpacing;
			}
		}

		// 6. Y: pull each node so its pins line up with its neighbours' pins (weighted median, primary
		// edges dominate), then resolve overlaps in the layer with minimal movement (isotonic regression).
		// Alternating one-sided sweeps; the last sweep goes in the "natural" direction of the graph type
		// (from sources for exec flow, from sinks for data flow), which is the side that ends up straightest.
		TArray<float> Offsets, Targets, Weights, Fitted;
		TArray<TPair<float, float>> Candidates;
		auto AlignLayer = [&](int32 LayerIdx, bool bUseIncoming)
		{
			const TArray<int32>& Layer = Layers[LayerIdx];
			const int32 Count = Layer.Num();
			Offsets.SetNumUninitialized(Count);
			Targets.SetNumUninitialized(Count);
			Weights.SetNumUninitialized(Count);
			float Cursor = 0.f;
			for (int32 Idx = 0; Idx < Count; ++Idx)
			{
				Offsets[Idx] = Cursor;
				Cursor += Nodes[Layer[Idx]].Size.Y + (Idx + 1 < Count ? Gap(Layer[Idx], Layer[Idx + 1]) : 0.f);

				const int32 Node = Layer[Idx];
				Candidates.Reset();
				float MaxWeight = 0.f;
				for (int32 SegIdx : (bUseIncoming ? InSegs[Node] : OutSegs[Node]))
				{
					const FLEdge& Seg = Segs[SegIdx];
					const int32 Other = bUseIncoming ? Seg.Src : Seg.Dst;
					const float OtherPinY = Y[Other] + PinOffset(Nodes[Other], bUseIncoming ? Seg.SrcPin : Seg.DstPin);
					const float OwnPin = PinOffset(Nodes[Node], bUseIncoming ? Seg.DstPin : Seg.SrcPin);
					Candidates.Add({ OtherPinY - OwnPin, Seg.Weight });
					MaxWeight = FMath::Max(MaxWeight, Seg.Weight);
				}
				if (Candidates.Num() > 0)
				{
					Targets[Idx] = WeightedMedian(Candidates);
					Weights[Idx] = MaxWeight;
				}
				else
				{
					// Nothing to align to on this side: stay put, but yield easily to neighbours.
					Targets[Idx] = Y[Node];
					Weights[Idx] = 0.01f;
				}
				Targets[Idx] -= Offsets[Idx];
			}
			IsotonicRegression(Targets, Weights, Fitted);
			for (int32 Idx = 0; Idx < Count; ++Idx)
			{
				Y[Layer[Idx]] = Fitted[Idx] + Offsets[Idx];
			}
		};

		const bool bForwardLast = Settings.Mode == ELayeringMode::FromSources;
		const int32 NumSweeps = FMath::Max(1, Settings.AlignmentSweeps);
		for (int32 Sweep = 0; Sweep < NumSweeps; ++Sweep)
		{
			const bool bSameAsLast = ((NumSweeps - 1 - Sweep) % 2) == 0;
			const bool bForward = bSameAsLast ? bForwardLast : !bForwardLast;
			if (bForward)
			{
				for (int32 LayerIdx = 1; LayerIdx < NumLayers; ++LayerIdx)
				{
					AlignLayer(LayerIdx, true);
				}
			}
			else
			{
				for (int32 LayerIdx = NumLayers - 2; LayerIdx >= 0; --LayerIdx)
				{
					AlignLayer(LayerIdx, false);
				}
			}
		}

		Out.NumLayers = NumLayers;
		Out.Positions.SetNum(NumReal);
		Out.Layers.SetNum(NumReal);
		for (int32 Idx = 0; Idx < NumReal; ++Idx)
		{
			Out.Positions[Idx] = FVector2f(X[Idx], Y[Idx]);
			Out.Layers[Idx] = NodeLayer[Idx];
		}
		return Out;
	}

	/** A rigid group of input nodes that the outer layout treats as one node. */
	struct FBlock
	{
		TArray<int32> Members;
		TArray<FVector2f> MemberOffsets;
		TArray<int32> PinBase;
		FLNode Shape;
		int32 AnchorPriority = 0;
	};

	struct FUnionFind
	{
		TArray<int32> Parent;
		explicit FUnionFind(int32 Num)
		{
			Parent.SetNumUninitialized(Num);
			for (int32 Idx = 0; Idx < Num; ++Idx)
			{
				Parent[Idx] = Idx;
			}
		}
		int32 Find(int32 Idx)
		{
			while (Parent[Idx] != Idx)
			{
				Parent[Idx] = Parent[Parent[Idx]];
				Idx = Parent[Idx];
			}
			return Idx;
		}
		void Union(int32 A, int32 B)
		{
			A = Find(A);
			B = Find(B);
			if (A != B)
			{
				Parent[FMath::Max(A, B)] = FMath::Min(A, B);
			}
		}
	};

	static FLNode MakeLNode(const FLayoutNode& Node)
	{
		FLNode Out;
		Out.Size = Node.Size;
		Out.OrigPos = Node.OrigPos;
		Out.Constraint = Node.LayerConstraint;
		Out.PinOffsets.Reserve(Node.Pins.Num());
		for (const FLayoutPin& Pin : Node.Pins)
		{
			Out.PinOffsets.Add(Pin.OffsetY);
		}
		return Out;
	}

	static float EdgeWeight(const FLayoutEdge& Edge, const FLayoutParams& Params)
	{
		return Edge.bPrimary ? Params.PrimaryEdgeWeight : 1.f;
	}

	/** Turns members + their relative positions into a block whose AnchorLocal member keeps its original position. */
	static FBlock MakeBlock(const FLayoutInput& Input, const TArray<int32>& Members, const TArray<FVector2f>& RelPos, int32 AnchorLocal)
	{
		FBlock Block;
		Block.Members = Members;

		FBox2f Bounds(ForceInit);
		for (int32 Local = 0; Local < Members.Num(); ++Local)
		{
			Bounds += RelPos[Local];
			Bounds += RelPos[Local] + Input.Nodes[Members[Local]].Size;
		}

		Block.Shape.Size = Bounds.GetSize();
		for (int32 Local = 0; Local < Members.Num(); ++Local)
		{
			const FLayoutNode& Node = Input.Nodes[Members[Local]];
			const FVector2f Offset = RelPos[Local] - Bounds.Min;
			Block.MemberOffsets.Add(Offset);
			Block.PinBase.Add(Block.Shape.PinOffsets.Num());
			for (const FLayoutPin& Pin : Node.Pins)
			{
				Block.Shape.PinOffsets.Add(Offset.Y + Pin.OffsetY);
			}
			Block.AnchorPriority = FMath::Max(Block.AnchorPriority, Node.AnchorPriority);
		}

		const FLayoutNode& Anchor = Input.Nodes[Members[AnchorLocal]];
		Block.Shape.OrigPos = Anchor.OrigPos - Block.MemberOffsets[AnchorLocal];
		Block.Shape.Constraint = Anchor.LayerConstraint;
		return Block;
	}

	/** Lays out a satellite group (optionally with its owner as the single sink) and wraps it in a block. */
	static FBlock BuildSatelliteBlock(const FLayoutInput& Input, const TArray<FLayoutEdge>& Edges, const TArray<int32>& Members, int32 OwnerLocal)
	{
		const FLayoutParams& Params = Input.Params;

		TMap<int32, int32> LocalOf;
		TArray<FLNode> LNodes;
		for (int32 Local = 0; Local < Members.Num(); ++Local)
		{
			LocalOf.Add(Members[Local], Local);
			LNodes.Add(MakeLNode(Input.Nodes[Members[Local]]));
		}
		if (OwnerLocal != INDEX_NONE)
		{
			LNodes[OwnerLocal].Constraint = ELayerConstraint::Last;
		}

		TArray<FLEdge> LEdges;
		for (const FLayoutEdge& Edge : Edges)
		{
			const int32* Src = LocalOf.Find(Edge.SrcNode);
			const int32* Dst = LocalOf.Find(Edge.DstNode);
			if (Src && Dst)
			{
				LEdges.Add({ *Src, Edge.SrcPin, *Dst, Edge.DstPin, EdgeWeight(Edge, Params) });
			}
		}

		FLSettings Settings;
		Settings.Mode = ELayeringMode::FromSinks;
		Settings.HSpacing = Params.SatelliteHorizontalSpacing;
		Settings.VSpacing = Params.SatelliteVerticalSpacing;
		Settings.CrossingSweeps = Params.CrossingSweeps;
		Settings.AlignmentSweeps = Params.AlignmentSweeps;
		FLOutput Layout = LayoutLayered(LNodes, LEdges, Settings);

		int32 AnchorLocal = OwnerLocal;
		if (OwnerLocal != INDEX_NONE)
		{
			// Keep satellites below the owner's exec band so the incoming exec wire doesn't run through them.
			const float MinTop = Layout.Positions[OwnerLocal].Y + Input.Nodes[Members[OwnerLocal]].SatelliteMinTop;
			float SatTop = TNumericLimits<float>::Max();
			for (int32 Local = 0; Local < Members.Num(); ++Local)
			{
				if (Local != OwnerLocal)
				{
					SatTop = FMath::Min(SatTop, Layout.Positions[Local].Y);
				}
			}
			if (SatTop < MinTop)
			{
				for (int32 Local = 0; Local < Members.Num(); ++Local)
				{
					if (Local != OwnerLocal)
					{
						Layout.Positions[Local].Y += MinTop - SatTop;
					}
				}
			}
		}
		else
		{
			// Free satellite group: anchor the most important, then right-most (sink-most) node.
			AnchorLocal = 0;
			for (int32 Local = 1; Local < Members.Num(); ++Local)
			{
				const FLayoutNode& A = Input.Nodes[Members[Local]];
				const FLayoutNode& B = Input.Nodes[Members[AnchorLocal]];
				if (A.AnchorPriority > B.AnchorPriority || (A.AnchorPriority == B.AnchorPriority && Layout.Layers[Local] > Layout.Layers[AnchorLocal]))
				{
					AnchorLocal = Local;
				}
			}
		}

		return MakeBlock(Input, Members, Layout.Positions, AnchorLocal);
	}

	static FBlock BuildSingleBlock(const FLayoutInput& Input, int32 NodeIdx)
	{
		FBlock Block;
		Block.Members.Add(NodeIdx);
		Block.MemberOffsets.Add(FVector2f::ZeroVector);
		Block.PinBase.Add(0);
		Block.Shape = MakeLNode(Input.Nodes[NodeIdx]);
		Block.AnchorPriority = Input.Nodes[NodeIdx].AnchorPriority;
		return Block;
	}

	/**
	 * Groups satellites with their owners. Owner = the earliest (lowest layer) non-satellite node reachable
	 * downstream through satellites only. Spine nodes are processed in layer order and each claims all
	 * still-unowned satellites upstream of it (BFS), which yields exactly that minimum and guarantees every
	 * satellite is connected to its owner inside the block. O(V + E) after the spine layering.
	 */
	static void BuildBlocks(const FLayoutInput& Input, const TArray<FLayoutEdge>& Edges, TArray<FBlock>& OutBlocks, TArray<int32>& OutBlockOf, TArray<int32>& OutMemberIdx)
	{
		const int32 NumNodes = Input.Nodes.Num();
		OutBlockOf.Init(INDEX_NONE, NumNodes);
		OutMemberIdx.Init(INDEX_NONE, NumNodes);

		bool bAnySatellite = false;
		for (const FLayoutNode& Node : Input.Nodes)
		{
			bAnySatellite |= Node.bSatellite;
		}

		auto Register = [&](FBlock&& Block)
		{
			const int32 BlockIdx = OutBlocks.Num();
			for (int32 Local = 0; Local < Block.Members.Num(); ++Local)
			{
				OutBlockOf[Block.Members[Local]] = BlockIdx;
				OutMemberIdx[Block.Members[Local]] = Local;
			}
			OutBlocks.Add(MoveTemp(Block));
		};

		if (!bAnySatellite)
		{
			for (int32 Idx = 0; Idx < NumNodes; ++Idx)
			{
				Register(BuildSingleBlock(Input, Idx));
			}
			return;
		}

		// Rank the spine on its own so ownership follows the main flow order.
		TArray<int32> Spine;
		TMap<int32, int32> SpineLocal;
		TArray<FLNode> SpineNodes;
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			if (!Input.Nodes[Idx].bSatellite)
			{
				SpineLocal.Add(Idx, Spine.Num());
				Spine.Add(Idx);
				SpineNodes.Add(MakeLNode(Input.Nodes[Idx]));
			}
		}
		TArray<FLEdge> SpineEdges;
		for (const FLayoutEdge& Edge : Edges)
		{
			const int32* Src = SpineLocal.Find(Edge.SrcNode);
			const int32* Dst = SpineLocal.Find(Edge.DstNode);
			if (Src && Dst)
			{
				SpineEdges.Add({ *Src, Edge.SrcPin, *Dst, Edge.DstPin, 1.f });
			}
		}
		TArray<int32> SpineRank;
		if (Spine.Num() > 0)
		{
			const TArray<bool> bBack = FindBackEdges(SpineNodes, SpineEdges);
			for (int32 EdgeIdx = 0; EdgeIdx < SpineEdges.Num(); ++EdgeIdx)
			{
				if (bBack[EdgeIdx])
				{
					Swap(SpineEdges[EdgeIdx].Src, SpineEdges[EdgeIdx].Dst);
				}
			}
			int32 Unused = 0;
			SpineRank = AssignLayers(SpineNodes, SpineEdges, Input.Params.Mode, Unused);
		}

		TArray<int32> SpineOrder;
		for (int32 Local = 0; Local < Spine.Num(); ++Local)
		{
			SpineOrder.Add(Local);
		}
		SpineOrder.StableSort([&](int32 A, int32 B)
		{
			if (SpineRank[A] != SpineRank[B])
			{
				return SpineRank[A] < SpineRank[B];
			}
			return Input.Nodes[Spine[A]].OrigPos.Y < Input.Nodes[Spine[B]].OrigPos.Y;
		});

		TArray<TArray<int32>> InEdges;
		InEdges.SetNum(NumNodes);
		for (int32 EdgeIdx = 0; EdgeIdx < Edges.Num(); ++EdgeIdx)
		{
			InEdges[Edges[EdgeIdx].DstNode].Add(EdgeIdx);
		}

		TArray<int32> Owner;
		Owner.Init(INDEX_NONE, NumNodes);
		TArray<TArray<int32>> Owned;
		Owned.SetNum(NumNodes);
		TArray<int32> Queue;
		for (int32 Local : SpineOrder)
		{
			const int32 SpineNode = Spine[Local];
			Queue.Reset();
			Queue.Add(SpineNode);
			for (int32 Head = 0; Head < Queue.Num(); ++Head)
			{
				for (int32 EdgeIdx : InEdges[Queue[Head]])
				{
					const int32 Up = Edges[EdgeIdx].SrcNode;
					if (Input.Nodes[Up].bSatellite && Owner[Up] == INDEX_NONE)
					{
						Owner[Up] = SpineNode;
						Owned[SpineNode].Add(Up);
						Queue.Add(Up);
					}
				}
			}
		}

		for (int32 SpineNode : Spine)
		{
			if (Owned[SpineNode].Num() == 0)
			{
				Register(BuildSingleBlock(Input, SpineNode));
				continue;
			}
			TArray<int32> Members;
			Members.Add(SpineNode);
			Owned[SpineNode].Sort();
			Members.Append(Owned[SpineNode]);
			Register(BuildSatelliteBlock(Input, Edges, Members, 0));
		}

		// Satellites that never reach the spine form their own groups (by connectivity).
		FUnionFind FreeSets(NumNodes);
		for (const FLayoutEdge& Edge : Edges)
		{
			if (Input.Nodes[Edge.SrcNode].bSatellite && Input.Nodes[Edge.DstNode].bSatellite
				&& Owner[Edge.SrcNode] == INDEX_NONE && Owner[Edge.DstNode] == INDEX_NONE)
			{
				FreeSets.Union(Edge.SrcNode, Edge.DstNode);
			}
		}
		TMap<int32, TArray<int32>> FreeGroups;
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			if (Input.Nodes[Idx].bSatellite && Owner[Idx] == INDEX_NONE)
			{
				FreeGroups.FindOrAdd(FreeSets.Find(Idx)).Add(Idx);
			}
		}
		FreeGroups.KeySort([](int32 A, int32 B) { return A < B; });
		for (TPair<int32, TArray<int32>>& Group : FreeGroups)
		{
			if (Group.Value.Num() == 1)
			{
				Register(BuildSingleBlock(Input, Group.Value[0]));
			}
			else
			{
				Register(BuildSatelliteBlock(Input, Edges, Group.Value, INDEX_NONE));
			}
		}
	}

	/** Signed gaps between two boxes (negative means they overlap on that axis). */
	static float GapX(const FBox2f& A, const FBox2f& B) { return FMath::Max(B.Min.X - A.Max.X, A.Min.X - B.Max.X); }
	static float GapY(const FBox2f& A, const FBox2f& B) { return FMath::Max(B.Min.Y - A.Max.Y, A.Min.Y - B.Max.Y); }

	/**
	 * Smallest vertical shift (up or down, ties go down) that keeps every rect at least the spacing
	 * away from every obstacle. Each conflicting (rect, obstacle) pair forbids an open interval of shifts;
	 * we merge the intervals and pick the free value closest to zero. O(R * O + K log K).
	 */
	static float FindFreeVerticalShift(TConstArrayView<FBox2f> Rects, TConstArrayView<FBox2f> Obstacles, float HSpacing, float VSpacing)
	{
		constexpr float Eps = 0.01f;
		TArray<TPair<float, float>> Forbidden;
		for (const FBox2f& Rect : Rects)
		{
			for (const FBox2f& Obstacle : Obstacles)
			{
				if (GapX(Rect, Obstacle) < HSpacing - Eps)
				{
					Forbidden.Add({ Obstacle.Min.Y - VSpacing - Rect.Max.Y, Obstacle.Max.Y + VSpacing - Rect.Min.Y });
				}
			}
		}
		if (Forbidden.Num() == 0)
		{
			return 0.f;
		}
		Forbidden.Sort([](const TPair<float, float>& A, const TPair<float, float>& B) { return A.Key < B.Key; });

		TArray<TPair<float, float>> Merged;
		for (const TPair<float, float>& Interval : Forbidden)
		{
			if (Merged.Num() > 0 && Interval.Key < Merged.Last().Value)
			{
				Merged.Last().Value = FMath::Max(Merged.Last().Value, Interval.Value);
			}
			else
			{
				Merged.Add(Interval);
			}
		}
		for (const TPair<float, float>& Interval : Merged)
		{
			if (Interval.Key < 0.f && 0.f < Interval.Value)
			{
				return FMath::Abs(Interval.Key) < Interval.Value ? Interval.Key : Interval.Value;
			}
		}
		return 0.f;
	}
} // namespace LayoutPrivate

/** The layered pipeline over all input nodes as one level. FLayoutInput::Groups is ignored. */
static FLayoutResult RunFlat(const FLayoutInput& Input)
{
	using namespace LayoutPrivate;

	const FLayoutParams& Params = Input.Params;
	const int32 NumNodes = Input.Nodes.Num();

	FLayoutResult Result;
	Result.Positions.SetNum(NumNodes);
	for (int32 Idx = 0; Idx < NumNodes; ++Idx)
	{
		Result.Positions[Idx] = Input.Nodes[Idx].OrigPos;
	}
	if (NumNodes == 0)
	{
		return Result;
	}

	// Self loops carry no layout information; invalid edges are dropped rather than trusted.
	TArray<FLayoutEdge> Edges;
	Edges.Reserve(Input.Edges.Num());
	for (const FLayoutEdge& Edge : Input.Edges)
	{
		if (Input.Nodes.IsValidIndex(Edge.SrcNode) && Input.Nodes.IsValidIndex(Edge.DstNode) && Edge.SrcNode != Edge.DstNode)
		{
			Edges.Add(Edge);
		}
	}

	TArray<FBlock> Blocks;
	TArray<int32> BlockOf;
	TArray<int32> MemberIdx;
	BuildBlocks(Input, Edges, Blocks, BlockOf, MemberIdx);
	Result.Stats.NumBlocks = Blocks.Num();

	auto BlockPin = [&](int32 Node, int32 Pin)
	{
		return Input.Nodes[Node].Pins.IsValidIndex(Pin) ? Blocks[BlockOf[Node]].PinBase[MemberIdx[Node]] + Pin : INDEX_NONE;
	};

	TArray<FLEdge> BlockEdges;
	FUnionFind Islands(Blocks.Num());
	for (const FLayoutEdge& Edge : Edges)
	{
		const int32 SrcBlock = BlockOf[Edge.SrcNode];
		const int32 DstBlock = BlockOf[Edge.DstNode];
		if (SrcBlock != DstBlock)
		{
			BlockEdges.Add({ SrcBlock, BlockPin(Edge.SrcNode, Edge.SrcPin), DstBlock, BlockPin(Edge.DstNode, Edge.DstPin), EdgeWeight(Edge, Params) });
			Islands.Union(SrcBlock, DstBlock);
		}
	}

	TMap<int32, TArray<int32>> IslandBlocks;
	for (int32 BlockIdx = 0; BlockIdx < Blocks.Num(); ++BlockIdx)
	{
		IslandBlocks.FindOrAdd(Islands.Find(BlockIdx)).Add(BlockIdx);
	}
	IslandBlocks.KeySort([](int32 A, int32 B) { return A < B; });
	Result.Stats.NumIslands = IslandBlocks.Num();

	FLSettings Settings;
	Settings.Mode = Params.Mode;
	Settings.HSpacing = Params.HorizontalSpacing;
	Settings.VSpacing = Params.VerticalSpacing;
	Settings.CrossingSweeps = Params.CrossingSweeps;
	Settings.AlignmentSweeps = Params.AlignmentSweeps;

	struct FIsland
	{
		TArray<int32> Blocks;
		TArray<FVector2f> BlockPos;
		int32 AnchorPriority = 0;
		int32 NumMembers = 0;
		float OrigTop = 0.f;
	};
	TArray<FIsland> IslandList;

	for (TPair<int32, TArray<int32>>& Pair : IslandBlocks)
	{
		FIsland& Island = IslandList.AddDefaulted_GetRef();
		Island.Blocks = Pair.Value;

		TMap<int32, int32> LocalOf;
		TArray<FLNode> LNodes;
		for (int32 Local = 0; Local < Island.Blocks.Num(); ++Local)
		{
			LocalOf.Add(Island.Blocks[Local], Local);
			LNodes.Add(Blocks[Island.Blocks[Local]].Shape);
		}
		TArray<FLEdge> LEdges;
		for (const FLEdge& Edge : BlockEdges)
		{
			if (const int32* Src = LocalOf.Find(Edge.Src))
			{
				LEdges.Add({ *Src, Edge.SrcPin, LocalOf.FindChecked(Edge.Dst), Edge.DstPin, Edge.Weight });
			}
		}

		const FLOutput Layout = LayoutLayered(LNodes, LEdges, Settings);
		Result.Stats.MaxLayers = FMath::Max(Result.Stats.MaxLayers, Layout.NumLayers);
		Result.Stats.NumDummies += Layout.NumDummies;
		Result.Stats.NumReversedEdges += Layout.NumReversed;
		Result.Stats.CrossingsBefore += Layout.CrossingsBefore;
		Result.Stats.CrossingsAfter += Layout.CrossingsAfter;

		// Anchor: highest priority; then the block at the flow's origin (first layer for exec flow,
		// last layer for data flow); then the top-most one.
		int32 AnchorLocal = 0;
		Island.OrigTop = TNumericLimits<float>::Max();
		for (int32 Local = 0; Local < Island.Blocks.Num(); ++Local)
		{
			const FBlock& Block = Blocks[Island.Blocks[Local]];
			Island.NumMembers += Block.Members.Num();
			Island.OrigTop = FMath::Min(Island.OrigTop, Block.Shape.OrigPos.Y);

			const FBlock& Best = Blocks[Island.Blocks[AnchorLocal]];
			if (Block.AnchorPriority != Best.AnchorPriority)
			{
				if (Block.AnchorPriority > Best.AnchorPriority)
				{
					AnchorLocal = Local;
				}
				continue;
			}
			const int32 LayerA = Layout.Layers[Local];
			const int32 LayerB = Layout.Layers[AnchorLocal];
			if (LayerA != LayerB)
			{
				const bool bBetter = Params.Mode == ELayeringMode::FromSources ? LayerA < LayerB : LayerA > LayerB;
				if (bBetter)
				{
					AnchorLocal = Local;
				}
				continue;
			}
			if (Block.Shape.OrigPos.Y < Best.Shape.OrigPos.Y)
			{
				AnchorLocal = Local;
			}
		}
		Island.AnchorPriority = Blocks[Island.Blocks[AnchorLocal]].AnchorPriority;

		const FVector2f Translation = Blocks[Island.Blocks[AnchorLocal]].Shape.OrigPos - Layout.Positions[AnchorLocal];
		for (int32 Local = 0; Local < Island.Blocks.Num(); ++Local)
		{
			Island.BlockPos.Add(Layout.Positions[Local] + Translation);
		}
	}

	// The most important island is placed first so it keeps its anchor exactly; the rest follow
	// top-to-bottom and get nudged out of the way of what's already placed.
	TArray<int32> IslandOrder;
	for (int32 Idx = 0; Idx < IslandList.Num(); ++Idx)
	{
		IslandOrder.Add(Idx);
	}
	int32 Primary = 0;
	for (int32 Idx = 1; Idx < IslandList.Num(); ++Idx)
	{
		const FIsland& A = IslandList[Idx];
		const FIsland& B = IslandList[Primary];
		if (A.AnchorPriority > B.AnchorPriority || (A.AnchorPriority == B.AnchorPriority && A.NumMembers > B.NumMembers))
		{
			Primary = Idx;
		}
	}
	IslandOrder.StableSort([&](int32 A, int32 B)
	{
		if ((A == Primary) != (B == Primary))
		{
			return A == Primary;
		}
		return IslandList[A].OrigTop < IslandList[B].OrigTop;
	});

	// Node rectangles of an island at its current BlockPos (blocks have empty regions, so use members).
	auto GatherRects = [&](const FIsland& Island, TArray<FBox2f>& OutRects, FBox2f& OutBounds)
	{
		OutBounds = FBox2f(ForceInit);
		for (int32 Local = 0; Local < Island.Blocks.Num(); ++Local)
		{
			const FBlock& Block = Blocks[Island.Blocks[Local]];
			for (int32 Member = 0; Member < Block.Members.Num(); ++Member)
			{
				const FVector2f Min = Island.BlockPos[Local] + Block.MemberOffsets[Member];
				const FBox2f Rect(Min, Min + Input.Nodes[Block.Members[Member]].Size);
				OutRects.Add(Rect);
				OutBounds += Rect;
			}
		}
	};
	auto ShiftIsland = [](FIsland& Island, const FVector2f& Delta)
	{
		for (FVector2f& Pos : Island.BlockPos)
		{
			Pos += Delta;
		}
	};

	TArray<FBox2f> Rects;
	if (Params.bPackIslands && IslandList.Num() > 1)
	{
		// Pack the other islands into rows under the primary one, in reading order of where they are now
		// (each island is still anchored at its original spot). Rows wrap at a width that keeps the packed
		// block roughly square. Shelf packing is O(I log I), and re-running it on its own output reproduces
		// the same order and positions, so pressing L twice is stable.
		const float Gap = Params.IslandSpacing;
		TArray<FBox2f> Bounds;
		Bounds.SetNum(IslandList.Num());
		for (int32 Idx = 0; Idx < IslandList.Num(); ++Idx)
		{
			Rects.Reset();
			GatherRects(IslandList[Idx], Rects, Bounds[Idx]);
		}

		TArray<int32> Others(IslandOrder);
		Others.RemoveAt(0); // IslandOrder starts with the primary island.
		Others.StableSort([&](int32 A, int32 B)
		{
			return Bounds[A].Min.Y != Bounds[B].Min.Y ? Bounds[A].Min.Y < Bounds[B].Min.Y : Bounds[A].Min.X < Bounds[B].Min.X;
		});

		const FBox2f& PrimaryBounds = Bounds[Primary];
		double PackedArea = 0.0;
		float WidestOther = 0.f;
		for (int32 Idx : Others)
		{
			const FVector2f Size = Bounds[Idx].GetSize();
			PackedArea += static_cast<double>(Size.X + Gap) * (Size.Y + Gap);
			WidestOther = FMath::Max(WidestOther, Size.X);
		}
		const float RowWidth = FMath::Max3(PrimaryBounds.GetSize().X, WidestOther, static_cast<float>(FMath::Sqrt(PackedArea)));

		float CursorX = PrimaryBounds.Min.X;
		float RowTop = PrimaryBounds.Max.Y + Gap;
		float RowHeight = 0.f;
		for (int32 Idx : Others)
		{
			const FVector2f Size = Bounds[Idx].GetSize();
			if (CursorX > PrimaryBounds.Min.X && CursorX + Size.X > PrimaryBounds.Min.X + RowWidth)
			{
				RowTop += RowHeight + Gap;
				CursorX = PrimaryBounds.Min.X;
				RowHeight = 0.f;
			}
			ShiftIsland(IslandList[Idx], FVector2f(CursorX, RowTop) - Bounds[Idx].Min);
			CursorX += Size.X + Gap;
			RowHeight = FMath::Max(RowHeight, Size.Y);
		}

		// The packed block moves as one piece to clear unselected nodes, so the grid stays intact.
		Rects.Reset();
		for (const FIsland& Island : IslandList)
		{
			FBox2f Unused;
			GatherRects(Island, Rects, Unused);
		}
		const float Shift = FindFreeVerticalShift(Rects, Input.Obstacles, Params.HorizontalSpacing, Params.VerticalSpacing);
		for (FIsland& Island : IslandList)
		{
			ShiftIsland(Island, FVector2f(0.f, Shift));
		}
	}
	else
	{
		TArray<FBox2f> Placed = Input.Obstacles;
		for (int32 IslandIdx : IslandOrder)
		{
			FIsland& Island = IslandList[IslandIdx];
			Rects.Reset();
			FBox2f Bounds;
			GatherRects(Island, Rects, Bounds);

			const float Shift = FindFreeVerticalShift(Rects, Placed, Params.HorizontalSpacing, Params.VerticalSpacing);
			ShiftIsland(Island, FVector2f(0.f, Shift));
			// Later islands avoid this island's bounds as a whole, so islands never interleave.
			Placed.Add(Bounds.ShiftBy(FVector2f(0.f, Shift)));
		}
	}

	for (const FIsland& Island : IslandList)
	{
		for (int32 Local = 0; Local < Island.Blocks.Num(); ++Local)
		{
			const FBlock& Block = Blocks[Island.Blocks[Local]];
			for (int32 Member = 0; Member < Block.Members.Num(); ++Member)
			{
				Result.Positions[Block.Members[Member]] = Island.BlockPos[Local] + Block.MemberOffsets[Member];
			}
		}
	}
	return Result;
}

/**
 * Groups are collapsed bottom-up. Each group's direct members (its own nodes plus its already collapsed child groups)
 * are laid out by RunFlat, and the result becomes one rigid unit of the enclosing level: sized to the group's frame
 * (members' bounds plus padding) and carrying every member pin, so wires into a group are straightened like wires into
 * any node. The top level is laid out with the real obstacles and then expanded back into node positions.
 * Every node takes part in exactly one RunFlat, so the total cost stays close to a single flat run.
 */
static FLayoutResult RunGrouped(const FLayoutInput& Input)
{
	const int32 NumNodes = Input.Nodes.Num();
	const int32 NumGroups = Input.Groups.Num();

	FLayoutResult Result;
	Result.Positions.SetNum(NumNodes);
	for (int32 Idx = 0; Idx < NumNodes; ++Idx)
	{
		Result.Positions[Idx] = Input.Nodes[Idx].OrigPos;
	}
	Result.GroupBounds.Init(FBox2f(ForceInit), NumGroups);
	if (NumNodes == 0)
	{
		return Result;
	}

	// Bad references put a node or group at the top level; a parent cycle is cut where the walk closes it.
	TArray<int32> NodeGroup;
	NodeGroup.SetNumUninitialized(NumNodes);
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		NodeGroup[Node] = Input.Groups.IsValidIndex(Input.Nodes[Node].Group) ? Input.Nodes[Node].Group : INDEX_NONE;
	}
	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumGroups);
	for (int32 Group = 0; Group < NumGroups; ++Group)
	{
		const int32 Declared = Input.Groups[Group].Parent;
		Parent[Group] = (Input.Groups.IsValidIndex(Declared) && Declared != Group) ? Declared : INDEX_NONE;
	}
	{
		enum EState : uint8 { Unvisited, OnPath, Done };
		TArray<uint8> State;
		State.Init(Unvisited, NumGroups);
		TArray<int32> Path;
		for (int32 Start = 0; Start < NumGroups; ++Start)
		{
			Path.Reset();
			int32 Cur = Start;
			while (Cur != INDEX_NONE && State[Cur] == Unvisited)
			{
				State[Cur] = OnPath;
				Path.Add(Cur);
				Cur = Parent[Cur];
			}
			if (Cur != INDEX_NONE && State[Cur] == OnPath)
			{
				Parent[Path.Last()] = INDEX_NONE;
			}
			for (int32 Group : Path)
			{
				State[Group] = Done;
			}
		}
	}
	TArray<int32> Depth;
	Depth.SetNumUninitialized(NumGroups);
	for (int32 Group = 0; Group < NumGroups; ++Group)
	{
		Depth[Group] = 0;
		for (int32 Ancestor = Parent[Group]; Ancestor != INDEX_NONE; Ancestor = Parent[Ancestor])
		{
			++Depth[Group];
		}
	}

	// Nodes and non-empty groups in each group's subtree, in index order so everything below is deterministic.
	TArray<TArray<int32>> SubtreeNodes;
	TArray<TArray<int32>> SubtreeGroups;
	SubtreeNodes.SetNum(NumGroups);
	SubtreeGroups.SetNum(NumGroups);
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		for (int32 Group = NodeGroup[Node]; Group != INDEX_NONE; Group = Parent[Group])
		{
			SubtreeNodes[Group].Add(Node);
		}
	}
	TArray<int32> Order;
	for (int32 Group = 0; Group < NumGroups; ++Group)
	{
		if (SubtreeNodes[Group].Num() > 0)
		{
			Order.Add(Group);
			for (int32 Ancestor = Group; Ancestor != INDEX_NONE; Ancestor = Parent[Ancestor])
			{
				SubtreeGroups[Ancestor].Add(Group);
			}
			Result.Stats.MaxGroupDepth = FMath::Max(Result.Stats.MaxGroupDepth, Depth[Group] + 1);
		}
	}
	Order.StableSort([&Depth](int32 A, int32 B) { return Depth[A] > Depth[B]; });
	Result.Stats.NumGroups = Order.Num();

	// Each edge is laid out at the deepest group containing both ends (slot 0 = top level). On its way there it enters
	// or leaves every group below that level, which the boundary rule and the satellite clearance need to know.
	auto DepthOf = [&Depth](int32 Group) { return Group == INDEX_NONE ? -1 : Depth[Group]; };
	auto CommonGroup = [&](int32 A, int32 B)
	{
		while (DepthOf(A) > DepthOf(B)) { A = Parent[A]; }
		while (DepthOf(B) > DepthOf(A)) { B = Parent[B]; }
		while (A != B) { A = Parent[A]; B = Parent[B]; }
		return A;
	};
	TArray<TArray<int32>> LevelEdges;
	LevelEdges.SetNum(NumGroups + 1);
	TArray<bool> NodeEntered, NodeLeft, GroupEntered, GroupLeft;
	NodeEntered.Init(false, NumNodes);
	NodeLeft.Init(false, NumNodes);
	GroupEntered.Init(false, NumGroups);
	GroupLeft.Init(false, NumGroups);
	TArray<TArray<int32>> PrimaryEntries; // per group: nodes a primary wire enters from outside the group
	PrimaryEntries.SetNum(NumGroups);
	for (int32 EdgeIdx = 0; EdgeIdx < Input.Edges.Num(); ++EdgeIdx)
	{
		const FLayoutEdge& Edge = Input.Edges[EdgeIdx];
		if (!Input.Nodes.IsValidIndex(Edge.SrcNode) || !Input.Nodes.IsValidIndex(Edge.DstNode) || Edge.SrcNode == Edge.DstNode)
		{
			continue;
		}
		const int32 Level = CommonGroup(NodeGroup[Edge.SrcNode], NodeGroup[Edge.DstNode]);
		LevelEdges[Level + 1].Add(EdgeIdx);
		// In each group strictly inside Level, the unit holding the end (the node itself, then each enclosing child group)
		// is wired to something outside that group.
		int32 Child = INDEX_NONE;
		for (int32 Group = NodeGroup[Edge.DstNode]; Group != Level; Child = Group, Group = Parent[Group])
		{
			if (Child == INDEX_NONE)
			{
				NodeEntered[Edge.DstNode] = true;
			}
			else
			{
				GroupEntered[Child] = true;
			}
			if (Edge.bPrimary)
			{
				PrimaryEntries[Group].AddUnique(Edge.DstNode);
			}
		}
		Child = INDEX_NONE;
		for (int32 Group = NodeGroup[Edge.SrcNode]; Group != Level; Child = Group, Group = Parent[Group])
		{
			if (Child == INDEX_NONE)
			{
				NodeLeft[Edge.SrcNode] = true;
			}
			else
			{
				GroupLeft[Child] = true;
			}
		}
	}

	TArray<FLayoutNode> GroupShape;   // a collapsed group, as one node of its parent level
	TArray<FVector2f> NodeOffset;     // node position relative to the frame of the outermost collapsed group holding it
	TArray<int32> NodePinBase;        // index of the node's first pin in that group's shape
	TArray<FBox2f> GroupFrame;        // frame relative to the frame of the outermost collapsed group holding it (or itself)
	GroupShape.SetNum(NumGroups);
	NodeOffset.Init(FVector2f::ZeroVector, NumNodes);
	NodePinBase.Init(0, NumNodes);
	GroupFrame.Init(FBox2f(ForceInit), NumGroups);

	// One level: Level's own nodes first (input order), then its collapsed child groups, as the units of a flat run.
	auto LayoutLevel = [&](int32 Level, const TArray<int32>& UnitNodes, const TArray<int32>& UnitGroups, const TArray<FBox2f>& Obstacles)
	{
		FLayoutInput Sub;
		Sub.Params = Input.Params;
		Sub.Obstacles = Obstacles;
		TMap<int32, int32> NodeUnit;
		TMap<int32, int32> GroupUnit;
		for (int32 Node : UnitNodes)
		{
			NodeUnit.Add(Node, Sub.Nodes.Num());
			Sub.Nodes.Add_GetRef(Input.Nodes[Node]).Group = INDEX_NONE;
		}
		for (int32 Group : UnitGroups)
		{
			GroupUnit.Add(Group, Sub.Nodes.Num());
			Sub.Nodes.Add(GroupShape[Group]);
		}

		// Where an input pin lives at this level: on its node, or on the child group that holds the node.
		auto Locate = [&](int32 Node, int32 Pin, int32& OutUnit, int32& OutPin)
		{
			if (const int32* Unit = NodeUnit.Find(Node))
			{
				OutUnit = *Unit;
				OutPin = Pin;
				return;
			}
			int32 Group = NodeGroup[Node];
			while (Parent[Group] != Level)
			{
				Group = Parent[Group];
			}
			OutUnit = GroupUnit.FindChecked(Group);
			OutPin = Input.Nodes[Node].Pins.IsValidIndex(Pin) ? NodePinBase[Node] + Pin : INDEX_NONE;
		};
		TArray<bool> HasIn, HasOut;
		HasIn.Init(false, Sub.Nodes.Num());
		HasOut.Init(false, Sub.Nodes.Num());
		for (int32 EdgeIdx : LevelEdges[Level + 1])
		{
			const FLayoutEdge& Edge = Input.Edges[EdgeIdx];
			FLayoutEdge& SubEdge = Sub.Edges.AddDefaulted_GetRef();
			Locate(Edge.SrcNode, Edge.SrcPin, SubEdge.SrcNode, SubEdge.SrcPin);
			Locate(Edge.DstNode, Edge.DstPin, SubEdge.DstNode, SubEdge.DstPin);
			SubEdge.bPrimary = Edge.bPrimary;
			HasOut[SubEdge.SrcNode] = true;
			HasIn[SubEdge.DstNode] = true;
		}

		// Boundary rule: a unit wired from outside the group (and from nothing inside) goes to the group's first layer,
		// one wired to outside (and to nothing inside) to its last, so outside wires don't run across the group.
		// AssignLayers only honours First/Last in exactly those cases; a constraint set by the adapter wins.
		auto ApplyBoundary = [](FLayoutNode& Unit, bool bEntered, bool bLeft, bool bHasIn, bool bHasOut)
		{
			if (Unit.LayerConstraint != ELayerConstraint::None)
			{
				return;
			}
			if (bEntered && !bHasIn)
			{
				Unit.LayerConstraint = ELayerConstraint::First;
			}
			else if (bLeft && !bHasOut)
			{
				Unit.LayerConstraint = ELayerConstraint::Last;
			}
		};
		for (int32 Local = 0; Local < UnitNodes.Num(); ++Local)
		{
			ApplyBoundary(Sub.Nodes[Local], NodeEntered[UnitNodes[Local]], NodeLeft[UnitNodes[Local]], HasIn[Local], HasOut[Local]);
		}
		for (int32 Idx = 0; Idx < UnitGroups.Num(); ++Idx)
		{
			const int32 Local = UnitNodes.Num() + Idx;
			ApplyBoundary(Sub.Nodes[Local], GroupEntered[UnitGroups[Idx]], GroupLeft[UnitGroups[Idx]], HasIn[Local], HasOut[Local]);
		}

		FLayoutResult SubResult = RunFlat(Sub);
		const FLayoutStats& Stats = SubResult.Stats;
		Result.Stats.NumBlocks += Stats.NumBlocks;
		Result.Stats.MaxLayers = FMath::Max(Result.Stats.MaxLayers, Stats.MaxLayers);
		Result.Stats.NumDummies += Stats.NumDummies;
		Result.Stats.NumReversedEdges += Stats.NumReversedEdges;
		Result.Stats.CrossingsBefore += Stats.CrossingsBefore;
		Result.Stats.CrossingsAfter += Stats.CrossingsAfter;
		return SubResult;
	};

	for (int32 Group : Order)
	{
		TArray<int32> UnitNodes;
		TArray<int32> UnitGroups;
		for (int32 Node : SubtreeNodes[Group])
		{
			if (NodeGroup[Node] == Group)
			{
				UnitNodes.Add(Node);
			}
		}
		for (int32 Child : SubtreeGroups[Group])
		{
			if (Parent[Child] == Group)
			{
				UnitGroups.Add(Child);
			}
		}
		const FLayoutResult Sub = LayoutLevel(Group, UnitNodes, UnitGroups, {});

		// Frame = the units' bounds grown by the padding (negative padding counts as none).
		const FVector2f PadMin = Input.Groups[Group].PaddingMin.ComponentMax(FVector2f::ZeroVector);
		const FVector2f PadMax = Input.Groups[Group].PaddingMax.ComponentMax(FVector2f::ZeroVector);
		FBox2f Content(ForceInit);
		for (int32 Local = 0; Local < Sub.Positions.Num(); ++Local)
		{
			const FVector2f Size = Local < UnitNodes.Num() ? Input.Nodes[UnitNodes[Local]].Size : GroupShape[UnitGroups[Local - UnitNodes.Num()]].Size;
			Content += FBox2f(Sub.Positions[Local], Sub.Positions[Local] + Size);
		}
		const FVector2f FrameMin = Content.Min - PadMin;

		// The inner run kept its anchor at its original position, so the frame's corner is where the group "was".
		FLayoutNode Shape;
		Shape.Size = Content.GetSize() + PadMin + PadMax;
		Shape.OrigPos = FrameMin;
		Shape.AnchorPriority = TNumericLimits<int32>::Lowest();
		bool bAllSatellites = true;
		bool bAnyFirst = false;
		bool bAnyLast = false;
		auto AddMember = [&](const FLayoutNode& Member)
		{
			Shape.AnchorPriority = FMath::Max(Shape.AnchorPriority, Member.AnchorPriority);
			bAllSatellites &= Member.bSatellite;
			bAnyFirst |= Member.LayerConstraint == ELayerConstraint::First;
			bAnyLast |= Member.LayerConstraint == ELayerConstraint::Last;
		};
		for (int32 Local = 0; Local < UnitNodes.Num(); ++Local)
		{
			const int32 Node = UnitNodes[Local];
			NodeOffset[Node] = Sub.Positions[Local] - FrameMin;
			NodePinBase[Node] = Shape.Pins.Num();
			for (const FLayoutPin& Pin : Input.Nodes[Node].Pins)
			{
				Shape.Pins.Add({ Pin.Side, NodeOffset[Node].Y + Pin.OffsetY });
			}
			AddMember(Input.Nodes[Node]);
		}
		for (int32 Idx = 0; Idx < UnitGroups.Num(); ++Idx)
		{
			const int32 Child = UnitGroups[Idx];
			const FVector2f Offset = Sub.Positions[UnitNodes.Num() + Idx] - FrameMin;
			const int32 PinBase = Shape.Pins.Num();
			for (const FLayoutPin& Pin : GroupShape[Child].Pins)
			{
				Shape.Pins.Add({ Pin.Side, Offset.Y + Pin.OffsetY });
			}
			for (int32 Node : SubtreeNodes[Child])
			{
				NodeOffset[Node] += Offset;
				NodePinBase[Node] += PinBase;
			}
			for (int32 Inner : SubtreeGroups[Child])
			{
				GroupFrame[Inner] = GroupFrame[Inner].ShiftBy(Offset);
			}
			AddMember(GroupShape[Child]);
		}
		Shape.bSatellite = bAllSatellites;
		Shape.LayerConstraint = bAnyFirst == bAnyLast ? ELayerConstraint::None : (bAnyFirst ? ELayerConstraint::First : ELayerConstraint::Last);
		// Satellites hanging off the group stay clear of the main-flow wires entering it, as they do for a single node.
		for (int32 Node : PrimaryEntries[Group])
		{
			if (Input.Nodes[Node].SatelliteMinTop > 0.f)
			{
				Shape.SatelliteMinTop = FMath::Max(Shape.SatelliteMinTop, NodeOffset[Node].Y + Input.Nodes[Node].SatelliteMinTop);
			}
		}
		GroupFrame[Group] = FBox2f(FVector2f::ZeroVector, Shape.Size);
		GroupShape[Group] = MoveTemp(Shape);
	}

	TArray<int32> TopNodes;
	TArray<int32> TopGroups;
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		if (NodeGroup[Node] == INDEX_NONE)
		{
			TopNodes.Add(Node);
		}
	}
	for (int32 Group = 0; Group < NumGroups; ++Group)
	{
		if (Parent[Group] == INDEX_NONE && SubtreeNodes[Group].Num() > 0)
		{
			TopGroups.Add(Group);
		}
	}
	const FLayoutResult Top = LayoutLevel(INDEX_NONE, TopNodes, TopGroups, Input.Obstacles);
	Result.Stats.NumIslands = Top.Stats.NumIslands;
	for (int32 Local = 0; Local < TopNodes.Num(); ++Local)
	{
		Result.Positions[TopNodes[Local]] = Top.Positions[Local];
	}
	for (int32 Idx = 0; Idx < TopGroups.Num(); ++Idx)
	{
		const FVector2f Pos = Top.Positions[TopNodes.Num() + Idx];
		for (int32 Node : SubtreeNodes[TopGroups[Idx]])
		{
			Result.Positions[Node] = Pos + NodeOffset[Node];
		}
		for (int32 Inner : SubtreeGroups[TopGroups[Idx]])
		{
			Result.GroupBounds[Inner] = GroupFrame[Inner].ShiftBy(Pos);
		}
	}
	return Result;
}

FLayoutResult FGraphLayoutEngine::Run(const FLayoutInput& Input)
{
	const double StartTime = FPlatformTime::Seconds();
	FLayoutResult Result = Input.Groups.Num() > 0 ? RunGrouped(Input) : RunFlat(Input);
	Result.Stats.Milliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	return Result;
}

float FGraphLayoutEngine::FindFreeVerticalShift(TConstArrayView<FBox2f> Rects, TConstArrayView<FBox2f> Obstacles, float HorizontalSpacing, float VerticalSpacing)
{
	return LayoutPrivate::FindFreeVerticalShift(Rects, Obstacles, HorizontalSpacing, VerticalSpacing);
}
} // namespace UE::GraphAutoLayout
