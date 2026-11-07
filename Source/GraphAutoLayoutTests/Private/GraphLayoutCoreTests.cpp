// Copyright 2026 Jot. All Rights Reserved.

#include "GraphLayoutEngine.h"
#include "GraphLayoutTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace UE::GraphAutoLayout;
using namespace UE::GraphAutoLayout::Tests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreEmptyTest, "GraphAutoLayout.Core.EmptyGraph", TestFlags)
bool FGraphLayoutCoreEmptyTest::RunTest(const FString& Parameters)
{
	const FLayoutResult Result = FGraphLayoutEngine::Run(FLayoutInput());
	TestEqual(TEXT("No positions for empty input"), Result.Positions.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreSingleNodeTest, "GraphAutoLayout.Core.SingleNode", TestFlags)
bool FGraphLayoutCoreSingleNodeTest::RunTest(const FString& Parameters)
{
	FLayoutInput Input;
	Input.Nodes.Add(MakeNode(FVector2f(123.f, -456.f), FVector2f(200.f, 80.f), 1, 1));
	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestTrue(TEXT("Single node stays where it is"), Result.Positions[0].Equals(FVector2f(123.f, -456.f), 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreStraightChainTest, "GraphAutoLayout.Core.StraightPrimaryChain", TestFlags)
bool FGraphLayoutCoreStraightChainTest::RunTest(const FString& Parameters)
{
	// A messy exec chain must end up on one horizontal line (all pins at the same offset).
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	for (int32 Idx = 0; Idx < 6; ++Idx)
	{
		Input.Nodes.Add(MakeNode(FVector2f(Idx * 37.f, (Idx % 2) * 300.f - Idx * 50.f), FVector2f(150.f + Idx * 10.f, 100.f), 1, 1));
		if (Idx > 0)
		{
			Connect(Input, Idx - 1, 0, Idx, 0, true);
		}
	}
	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	for (int32 Idx = 1; Idx < 6; ++Idx)
	{
		TestEqual(FString::Printf(TEXT("Node %d is level with node 0"), Idx), Result.Positions[Idx].Y, Result.Positions[0].Y, 0.5f);
	}
	TestEqual(TEXT("Edges flow left to right"), CountBackwardEdges(Input, Result), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreNoOverlapTest, "GraphAutoLayout.Core.NoOverlapRandom", TestFlags)
bool FGraphLayoutCoreNoOverlapTest::RunTest(const FString& Parameters)
{
	for (int32 Seed = 1; Seed <= 6; ++Seed)
	{
		for (ELayeringMode Mode : { ELayeringMode::FromSources, ELayeringMode::FromSinks })
		{
			const FLayoutInput Input = MakeRandomDag(60, Seed, Mode);
			const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
			const int32 Overlaps = CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing);
			TestEqual(FString::Printf(TEXT("Seed %d mode %d: overlaps"), Seed, static_cast<int32>(Mode)), Overlaps, 0);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreDirectionTest, "GraphAutoLayout.Core.FlowDirection", TestFlags)
bool FGraphLayoutCoreDirectionTest::RunTest(const FString& Parameters)
{
	// Random DAGs: every edge must point right in both modes.
	for (int32 Seed = 10; Seed < 14; ++Seed)
	{
		for (ELayeringMode Mode : { ELayeringMode::FromSources, ELayeringMode::FromSinks })
		{
			const FLayoutInput Input = MakeRandomDag(80, Seed, Mode);
			const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
			TestEqual(FString::Printf(TEXT("Seed %d mode %d: backward edges"), Seed, static_cast<int32>(Mode)), CountBackwardEdges(Input, Result), 0);
		}
	}

	// Material-like fan-in: everything ends left of the sink, and the sink is the right-most node.
	FLayoutInput Material;
	Material.Params.Mode = ELayeringMode::FromSinks;
	const int32 Sink = Material.Nodes.Add(MakeNode(FVector2f(0.f, 0.f), FVector2f(250.f, 400.f), 12, 0));
	for (int32 Idx = 0; Idx < 12; ++Idx)
	{
		const int32 Producer = Material.Nodes.Add(MakeNode(FVector2f(-300.f, Idx * 10.f), FVector2f(160.f, 80.f), 2, 1));
		Connect(Material, Producer, 0, Sink, Idx);
		// Shared inputs feeding several producers (multi-consumer nodes).
		if (Idx % 3 == 0)
		{
			const int32 Shared = Material.Nodes.Add(MakeNode(FVector2f(-600.f, Idx * 10.f), FVector2f(120.f, 60.f), 0, 1));
			Connect(Material, Shared, 0, Producer, 0);
			Connect(Material, Shared, 0, Sink, (Idx + 5) % 12);
		}
	}
	const FLayoutResult Result = FGraphLayoutEngine::Run(Material);
	TestEqual(TEXT("Material: backward edges"), CountBackwardEdges(Material, Result), 0);
	for (int32 Idx = 0; Idx < Material.Nodes.Num(); ++Idx)
	{
		if (Idx != Sink)
		{
			TestTrue(FString::Printf(TEXT("Node %d is left of the sink"), Idx), Result.Positions[Idx].X + Material.Nodes[Idx].Size.X <= Result.Positions[Sink].X);
		}
	}
	TestEqual(TEXT("Material: overlaps"), CountOverlaps(*this, Material, Result, Material.Params.HorizontalSpacing, Material.Params.VerticalSpacing), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreCycleTest, "GraphAutoLayout.Core.Cycles", TestFlags)
bool FGraphLayoutCoreCycleTest::RunTest(const FString& Parameters)
{
	for (ELayeringMode Mode : { ELayeringMode::FromSources, ELayeringMode::FromSinks })
	{
		FLayoutInput Input;
		Input.Params.Mode = Mode;
		// Ring of 5, a 2-cycle, a self loop and a nested loop sharing nodes with the ring.
		for (int32 Idx = 0; Idx < 9; ++Idx)
		{
			Input.Nodes.Add(MakeNode(FVector2f(Idx * 50.f, Idx * 20.f), FVector2f(140.f, 90.f), 2, 2));
		}
		for (int32 Idx = 0; Idx < 5; ++Idx)
		{
			Connect(Input, Idx, 0, (Idx + 1) % 5, 0, true);
		}
		Connect(Input, 5, 0, 6, 0);
		Connect(Input, 6, 0, 5, 0);
		Connect(Input, 7, 0, 7, 0);
		Connect(Input, 3, 1, 1, 1);
		Connect(Input, 6, 1, 8, 1);
		Connect(Input, 8, 1, 2, 1);

		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		TestEqual(TEXT("All nodes positioned"), Result.Positions.Num(), Input.Nodes.Num());
		TestTrue(TEXT("Some edges were reversed"), Result.Stats.NumReversedEdges > 0);
		TestEqual(FString::Printf(TEXT("Mode %d: overlaps"), static_cast<int32>(Mode)), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreIslandsTest, "GraphAutoLayout.Core.Islands", TestFlags)
bool FGraphLayoutCoreIslandsTest::RunTest(const FString& Parameters)
{
	// Three disconnected chains that start out on top of each other.
	FLayoutInput Input;
	TArray<TArray<int32>> Islands;
	for (int32 IslandIdx = 0; IslandIdx < 3; ++IslandIdx)
	{
		TArray<int32>& Members = Islands.AddDefaulted_GetRef();
		for (int32 Idx = 0; Idx < 4; ++Idx)
		{
			const int32 Node = Input.Nodes.Add(MakeNode(FVector2f(Idx * 60.f, IslandIdx * 10.f + Idx * 5.f), FVector2f(180.f, 100.f + IslandIdx * 20.f), 1, 1));
			if (Idx > 0)
			{
				Connect(Input, Members.Last(), 0, Node, 0, true);
			}
			Members.Add(Node);
		}
	}
	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestEqual(TEXT("Three islands detected"), Result.Stats.NumIslands, 3);

	TArray<FBox2f> Bounds;
	for (const TArray<int32>& Members : Islands)
	{
		FBox2f Box(ForceInit);
		for (int32 Node : Members)
		{
			Box += NodeBox(Input, Result, Node);
		}
		Bounds.Add(Box);
	}
	for (int32 A = 0; A < Bounds.Num(); ++A)
	{
		for (int32 B = A + 1; B < Bounds.Num(); ++B)
		{
			const bool bSeparated = GapX(Bounds[A], Bounds[B]) >= Input.Params.HorizontalSpacing - 0.5f || GapY(Bounds[A], Bounds[B]) >= Input.Params.VerticalSpacing - 0.5f;
			TestTrue(FString::Printf(TEXT("Island %d and %d don't overlap"), A, B), bSeparated);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreIslandPackingTest, "GraphAutoLayout.Core.IslandPacking", TestFlags)
bool FGraphLayoutCoreIslandPackingTest::RunTest(const FString& Parameters)
{
	// A connected main chain plus 10 unconnected nodes scattered far away.
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	for (int32 Idx = 0; Idx < 4; ++Idx)
	{
		Input.Nodes.Add(MakeNode(FVector2f(Idx * 250.f, 0.f), FVector2f(200.f, 100.f), 1, 1));
		if (Idx > 0)
		{
			Connect(Input, Idx - 1, 0, Idx, 0, true);
		}
	}
	Input.Nodes[0].AnchorPriority = 10;
	FRandomStream Rand(42);
	TArray<int32> Loose;
	FBox2f OrigBounds(ForceInit);
	for (int32 Idx = 0; Idx < 10; ++Idx)
	{
		const FVector2f Pos(Rand.FRandRange(-5000.f, 5000.f), Rand.FRandRange(-5000.f, 5000.f));
		const FVector2f Size(Rand.FRandRange(100.f, 300.f), Rand.FRandRange(50.f, 150.f));
		Loose.Add(Input.Nodes.Add(MakeNode(Pos, Size, 1, 1)));
	}
	for (const FLayoutNode& Node : Input.Nodes)
	{
		OrigBounds += Node.OrigPos;
		OrigBounds += Node.OrigPos + Node.Size;
	}

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestEqual(TEXT("11 islands"), Result.Stats.NumIslands, 11);
	TestTrue(TEXT("Main chain anchor kept"), Result.Positions[0].Equals(Input.Nodes[0].OrigPos, 0.5f));
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);

	FBox2f MainBounds(ForceInit);
	for (int32 Idx = 0; Idx < 4; ++Idx)
	{
		MainBounds += NodeBox(Input, Result, Idx);
	}
	FBox2f PackedBounds(ForceInit);
	FBox2f AllBounds = MainBounds;
	for (int32 Idx : Loose)
	{
		const FBox2f Box = NodeBox(Input, Result, Idx);
		TestTrue(TEXT("Loose node packed below the main chain"), Box.Min.Y >= MainBounds.Max.Y + Input.Params.IslandSpacing - 0.5f);
		PackedBounds += Box;
		AllBounds += Box;
	}
	const float OrigArea = OrigBounds.GetArea();
	const float NewArea = AllBounds.GetArea();
	AddInfo(FString::Printf(TEXT("Bounds area %.0f -> %.0f"), OrigArea, NewArea));
	TestTrue(TEXT("Selection is much more compact"), NewArea < OrigArea * 0.05f);
	const FVector2f PackedSize = PackedBounds.GetSize();
	TestTrue(TEXT("Packed block is roughly square (aspect <= 4)"), FMath::Max(PackedSize.X, PackedSize.Y) <= 4.f * FMath::Min(PackedSize.X, PackedSize.Y));

	// Reading order: originally top-to-bottom (then left-to-right) becomes row-major in the grid.
	TArray<int32> Order = Loose;
	Order.StableSort([&](int32 A, int32 B)
	{
		const FVector2f& PA = Input.Nodes[A].OrigPos;
		const FVector2f& PB = Input.Nodes[B].OrigPos;
		return PA.Y != PB.Y ? PA.Y < PB.Y : PA.X < PB.X;
	});
	for (int32 Idx = 1; Idx < Order.Num(); ++Idx)
	{
		const FVector2f& Prev = Result.Positions[Order[Idx - 1]];
		const FVector2f& Cur = Result.Positions[Order[Idx]];
		const bool bSameRowAfter = FMath::IsNearlyEqual(Prev.Y, Cur.Y, 0.5f) && Cur.X > Prev.X;
		const bool bLaterRow = Cur.Y > Prev.Y + 0.5f;
		TestTrue(FString::Printf(TEXT("Loose node %d follows reading order"), Idx), bSameRowAfter || bLaterRow);
	}

	// Pressing L again on the result changes nothing.
	FLayoutInput Again = Input;
	for (int32 Idx = 0; Idx < Again.Nodes.Num(); ++Idx)
	{
		Again.Nodes[Idx].OrigPos = Result.Positions[Idx];
	}
	const FLayoutResult Second = FGraphLayoutEngine::Run(Again);
	int32 Moved = 0;
	for (int32 Idx = 0; Idx < Second.Positions.Num(); ++Idx)
	{
		Moved += Second.Positions[Idx].Equals(Result.Positions[Idx], 0.5f) ? 0 : 1;
	}
	TestEqual(TEXT("Second layout is stable"), Moved, 0);

	// With packing off, unconnected nodes that don't overlap stay where they were.
	FLayoutInput Unpacked = Input;
	Unpacked.Params.bPackIslands = false;
	const FLayoutResult Kept = FGraphLayoutEngine::Run(Unpacked);
	for (int32 Idx : Loose)
	{
		TestTrue(TEXT("Packing off: loose node stays"), Kept.Positions[Idx].Equals(Input.Nodes[Idx].OrigPos, 0.5f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreFreeShiftTest, "GraphAutoLayout.Core.FreeVerticalShift", TestFlags)
bool FGraphLayoutCoreFreeShiftTest::RunTest(const FString& Parameters)
{
	const float H = 80.f;
	const float V = 40.f;
	const FBox2f Rect(FVector2f(0.f, 0.f), FVector2f(100.f, 50.f));

	TestEqual(TEXT("No obstacles: no shift"), FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Rect, 1), {}, H, V), 0.f);

	// Far enough to the side: no shift.
	const FBox2f Side(FVector2f(200.f, 0.f), FVector2f(300.f, 50.f));
	TestEqual(TEXT("Obstacle a full gap to the right: no shift"), FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Rect, 1), MakeArrayView(&Side, 1), H, V), 0.f);

	// Overlapping obstacle sitting a bit low: moving up is shorter.
	const FBox2f Low(FVector2f(0.f, 30.f), FVector2f(100.f, 200.f));
	TestEqual(TEXT("Moves up when that's shorter"), FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Rect, 1), MakeArrayView(&Low, 1), H, V), 30.f - V - 50.f);

	// Overlapping obstacle sitting a bit high: moving down is shorter.
	const FBox2f High(FVector2f(0.f, -200.f), FVector2f(100.f, 10.f));
	TestEqual(TEXT("Moves down when that's shorter"), FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Rect, 1), MakeArrayView(&High, 1), H, V), 10.f + V);

	// Two obstacles leaving a gap too small: skips past both.
	const TArray<FBox2f> Stack = { FBox2f(FVector2f(0.f, -100.f), FVector2f(100.f, 20.f)), FBox2f(FVector2f(0.f, 60.f), FVector2f(100.f, 300.f)) };
	const float Shift = FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Rect, 1), Stack, H, V);
	const FBox2f Moved = Rect.ShiftBy(FVector2f(0.f, Shift));
	bool bClear = true;
	for (const FBox2f& Obstacle : Stack)
	{
		bClear &= GapX(Moved, Obstacle) >= H - 0.5f || GapY(Moved, Obstacle) >= V - 0.5f;
	}
	TestTrue(TEXT("Clear of both obstacles after the shift"), bClear);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreAnchorTest, "GraphAutoLayout.Core.Anchor", TestFlags)
bool FGraphLayoutCoreAnchorTest::RunTest(const FString& Parameters)
{
	// Explicit priority wins.
	{
		FLayoutInput Input = MakeRandomDag(40, 99, ELayeringMode::FromSinks);
		Input.Nodes[17].AnchorPriority = 100;
		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		TestTrue(TEXT("Priority anchor keeps its position"), Result.Positions[17].Equals(Input.Nodes[17].OrigPos, 0.5f));
	}
	// Without priorities: the sink of a data graph, the source of an exec graph.
	{
		FLayoutInput Input;
		Input.Params.Mode = ELayeringMode::FromSinks;
		const int32 Sink = Input.Nodes.Add(MakeNode(FVector2f(1000.f, 500.f), FVector2f(200.f, 300.f), 3, 0));
		for (int32 Idx = 0; Idx < 3; ++Idx)
		{
			const int32 Producer = Input.Nodes.Add(MakeNode(FVector2f(Idx * 100.f, Idx * 100.f), FVector2f(120.f, 60.f), 1, 1));
			Connect(Input, Producer, 0, Sink, Idx);
		}
		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		TestTrue(TEXT("Sink keeps its position"), Result.Positions[Sink].Equals(Input.Nodes[Sink].OrigPos, 0.5f));
	}
	{
		FLayoutInput Input;
		Input.Params.Mode = ELayeringMode::FromSources;
		const int32 Source = Input.Nodes.Add(MakeNode(FVector2f(-800.f, 250.f), FVector2f(200.f, 80.f), 0, 2));
		for (int32 Idx = 0; Idx < 2; ++Idx)
		{
			const int32 Next = Input.Nodes.Add(MakeNode(FVector2f(Idx * 300.f, Idx * -200.f), FVector2f(150.f, 80.f), 1, 0));
			Connect(Input, Source, Idx, Next, 0, true);
		}
		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		TestTrue(TEXT("Source keeps its position"), Result.Positions[Source].Equals(Input.Nodes[Source].OrigPos, 0.5f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreObstacleTest, "GraphAutoLayout.Core.Obstacles", TestFlags)
bool FGraphLayoutCoreObstacleTest::RunTest(const FString& Parameters)
{
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	for (int32 Idx = 0; Idx < 5; ++Idx)
	{
		Input.Nodes.Add(MakeNode(FVector2f(Idx * 10.f, 0.f), FVector2f(200.f, 120.f), 1, 1));
		if (Idx > 0)
		{
			Connect(Input, Idx - 1, 0, Idx, 0, true);
		}
	}
	// Unselected nodes sitting right where the chain wants to go.
	Input.Obstacles.Add(FBox2f(FVector2f(300.f, -20.f), FVector2f(500.f, 100.f)));
	Input.Obstacles.Add(FBox2f(FVector2f(700.f, 150.f), FVector2f(900.f, 260.f)));

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	for (int32 Idx = 0; Idx < Input.Nodes.Num(); ++Idx)
	{
		const FBox2f Box = NodeBox(Input, Result, Idx);
		for (const FBox2f& Obstacle : Input.Obstacles)
		{
			const bool bClear = GapX(Box, Obstacle) >= Input.Params.HorizontalSpacing - 0.5f || GapY(Box, Obstacle) >= Input.Params.VerticalSpacing - 0.5f;
			TestTrue(FString::Printf(TEXT("Node %d clear of obstacle"), Idx), bClear);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreSatelliteTest, "GraphAutoLayout.Core.Satellites", TestFlags)
bool FGraphLayoutCoreSatelliteTest::RunTest(const FString& Parameters)
{
	// Blueprint-like: exec chain of 4 with pure trees feeding data pins (pin 0 = exec in, 1..2 = data in).
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	TArray<int32> Exec;
	for (int32 Idx = 0; Idx < 4; ++Idx)
	{
		FLayoutNode Node = MakeNode(FVector2f(Idx * 30.f, Idx * 40.f), FVector2f(220.f, 110.f), 3, 1);
		Node.SatelliteMinTop = PinTop + PinRow * 0.5f;
		const int32 NodeIdx = Input.Nodes.Add(Node);
		if (Exec.Num() > 0)
		{
			Connect(Input, Exec.Last(), 0, NodeIdx, 0, true);
		}
		Exec.Add(NodeIdx);
	}
	Input.Nodes[Exec[0]].AnchorPriority = 10;

	TArray<int32> Satellites;
	auto AddPure = [&](FVector2f Pos)
	{
		FLayoutNode Node = MakeNode(Pos, FVector2f(140.f, 60.f), 1, 1);
		Node.bSatellite = true;
		const int32 Idx = Input.Nodes.Add(Node);
		Satellites.Add(Idx);
		return Idx;
	};
	for (int32 ExecIdx = 1; ExecIdx < 4; ++ExecIdx)
	{
		const int32 A = AddPure(FVector2f(0.f, 500.f + ExecIdx * 10.f));
		const int32 B = AddPure(FVector2f(0.f, 600.f + ExecIdx * 10.f));
		const int32 C = AddPure(FVector2f(0.f, 700.f + ExecIdx * 10.f));
		Connect(Input, A, 0, Exec[ExecIdx], 1);
		Connect(Input, B, 0, A, 0);
		Connect(Input, C, 0, Exec[ExecIdx], 2);
	}
	// A shared pure node feeding two exec nodes is owned by the earlier one.
	const int32 Shared = AddPure(FVector2f(0.f, 900.f));
	Connect(Input, Shared, 0, Exec[2], 2);
	Connect(Input, Shared, 0, Exec[3], 1);

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);

	for (int32 Idx = 1; Idx < Exec.Num(); ++Idx)
	{
		TestTrue(FString::Printf(TEXT("Exec %d right of exec %d"), Idx, Idx - 1), Result.Positions[Exec[Idx]].X > Result.Positions[Exec[Idx - 1]].X + Input.Nodes[Exec[Idx - 1]].Size.X);
		TestEqual(FString::Printf(TEXT("Exec %d level with exec 0"), Idx), Result.Positions[Exec[Idx]].Y, Result.Positions[Exec[0]].Y, 0.5f);
	}
	TestTrue(TEXT("Anchor exec node keeps its position"), Result.Positions[Exec[0]].Equals(Input.Nodes[Exec[0]].OrigPos, 0.5f));
	for (const FLayoutEdge& Edge : Input.Edges)
	{
		if (Input.Nodes[Edge.SrcNode].bSatellite)
		{
			TestTrue(TEXT("Satellite left of its consumer"), Result.Positions[Edge.SrcNode].X + Input.Nodes[Edge.SrcNode].Size.X < Result.Positions[Edge.DstNode].X);
		}
	}
	// Satellites stay below their owner's exec band.
	for (int32 ExecIdx = 1; ExecIdx < 4; ++ExecIdx)
	{
		for (const FLayoutEdge& Edge : Input.Edges)
		{
			if (Edge.DstNode == Exec[ExecIdx] && Input.Nodes[Edge.SrcNode].bSatellite)
			{
				TestTrue(TEXT("Satellite below exec band"), Result.Positions[Edge.SrcNode].Y >= Result.Positions[Exec[ExecIdx]].Y + PinTop + PinRow * 0.5f - 0.5f);
			}
		}
	}
	const float MinH = FMath::Min(Input.Params.HorizontalSpacing, Input.Params.SatelliteHorizontalSpacing);
	const float MinV = FMath::Min(Input.Params.VerticalSpacing, Input.Params.SatelliteVerticalSpacing);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, MinH, MinV), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCoreDeterminismTest, "GraphAutoLayout.Core.Determinism", TestFlags)
bool FGraphLayoutCoreDeterminismTest::RunTest(const FString& Parameters)
{
	const FLayoutInput Input = MakeRandomDag(120, 7, ELayeringMode::FromSinks);
	const FLayoutResult A = FGraphLayoutEngine::Run(Input);
	const FLayoutResult B = FGraphLayoutEngine::Run(Input);
	bool bSame = A.Positions.Num() == B.Positions.Num();
	for (int32 Idx = 0; bSame && Idx < A.Positions.Num(); ++Idx)
	{
		bSame = A.Positions[Idx] == B.Positions[Idx];
	}
	TestTrue(TEXT("Same input gives identical output"), bSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCorePerformanceTest, "GraphAutoLayout.Core.Performance", TestFlags)
bool FGraphLayoutCorePerformanceTest::RunTest(const FString& Parameters)
{
	for (int32 NumNodes : { 500, 2000 })
	{
		for (ELayeringMode Mode : { ELayeringMode::FromSources, ELayeringMode::FromSinks })
		{
			const FLayoutInput Input = MakeRandomDag(NumNodes, 1234 + NumNodes, Mode);
			const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
			AddInfo(FString::Printf(TEXT("%d nodes, %d edges, mode %d: %.2f ms (layers %d, dummies %d, crossings %lld -> %lld)"),
				NumNodes, Input.Edges.Num(), static_cast<int32>(Mode), Result.Stats.Milliseconds, Result.Stats.MaxLayers,
				Result.Stats.NumDummies, Result.Stats.CrossingsBefore, Result.Stats.CrossingsAfter));
			if (NumNodes == 500)
			{
				// Generous bound so the test isn't flaky on a busy machine; expected time is far lower.
				TestTrue(TEXT("500 nodes lay out in under 250 ms"), Result.Stats.Milliseconds < 250.0);
				TestEqual(TEXT("500 nodes: overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
