// Copyright 2026 Jot. All Rights Reserved.

#include "GraphLayoutEngine.h"
#include "GraphLayoutTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::GraphAutoLayout::Tests
{
	/** Group with comment-like padding: 30 around, plus a 30 tall title bar on top. */
	static FLayoutGroup MakeCommentGroup(int32 Parent)
	{
		FLayoutGroup Group;
		Group.Parent = Parent;
		Group.PaddingMin = FVector2f(30.f, 60.f);
		Group.PaddingMax = FVector2f(30.f, 30.f);
		return Group;
	}

	/** Whether Node belongs to Group, directly or through nested groups. Expects a well-formed group tree. */
	static bool IsGroupMember(const FLayoutInput& Input, int32 Node, int32 Group)
	{
		int32 Steps = 0;
		for (int32 G = Input.Nodes[Node].Group; Input.Groups.IsValidIndex(G) && Steps++ <= Input.Groups.Num(); G = Input.Groups[G].Parent)
		{
			if (G == Group)
			{
				return true;
			}
		}
		return false;
	}

	/** Whether Ancestor strictly encloses Group. */
	static bool IsGroupAncestor(const FLayoutInput& Input, int32 Ancestor, int32 Group)
	{
		int32 Steps = 0;
		for (int32 G = Input.Groups[Group].Parent; Input.Groups.IsValidIndex(G) && Steps++ <= Input.Groups.Num(); G = Input.Groups[G].Parent)
		{
			if (G == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Group invariants: each member sits inside its frame minus the padding, every other node keeps the spacing from the
	 * frame, a child frame sits inside its parent's frame minus the padding, and unrelated frames keep the spacing from
	 * each other. Groups without members must have an invalid frame. Returns the number of violations.
	 */
	static int32 CheckGroups(FAutomationTestBase& Test, const FLayoutInput& Input, const FLayoutResult& Result, float HSpacing, float VSpacing)
	{
		constexpr float Tolerance = 0.5f;
		int32 Violations = 0;
		auto Fail = [&](const FString& What)
		{
			if (Violations++ < 5)
			{
				Test.AddError(What);
			}
		};
		if (Result.GroupBounds.Num() != Input.Groups.Num())
		{
			Fail(FString::Printf(TEXT("%d group bounds for %d groups"), Result.GroupBounds.Num(), Input.Groups.Num()));
			return Violations;
		}
		auto Inner = [&](int32 G)
		{
			const FBox2f& Frame = Result.GroupBounds[G];
			return FBox2f(Frame.Min + Input.Groups[G].PaddingMin, Frame.Max - Input.Groups[G].PaddingMax);
		};
		auto Contains = [&](const FBox2f& Outer, const FBox2f& Box)
		{
			return Box.Min.X >= Outer.Min.X - Tolerance && Box.Min.Y >= Outer.Min.Y - Tolerance
				&& Box.Max.X <= Outer.Max.X + Tolerance && Box.Max.Y <= Outer.Max.Y + Tolerance;
		};
		auto TooClose = [&](const FBox2f& A, const FBox2f& B)
		{
			return GapX(A, B) < HSpacing - Tolerance && GapY(A, B) < VSpacing - Tolerance;
		};

		for (int32 G = 0; G < Input.Groups.Num(); ++G)
		{
			bool bHasMembers = false;
			for (int32 Node = 0; Node < Input.Nodes.Num(); ++Node)
			{
				bHasMembers |= IsGroupMember(Input, Node, G);
			}
			const FBox2f& Frame = Result.GroupBounds[G];
			if (!bHasMembers)
			{
				if (Frame.bIsValid)
				{
					Fail(FString::Printf(TEXT("Group %d has no members but a frame"), G));
				}
				continue;
			}
			if (!Frame.bIsValid)
			{
				Fail(FString::Printf(TEXT("Group %d has members but no frame"), G));
				continue;
			}
			for (int32 Node = 0; Node < Input.Nodes.Num(); ++Node)
			{
				const FBox2f Box = NodeBox(Input, Result, Node);
				if (IsGroupMember(Input, Node, G))
				{
					if (!Contains(Inner(G), Box))
					{
						Fail(FString::Printf(TEXT("Node %d (%s)-(%s) not inside group %d (%s)-(%s)"), Node, *Box.Min.ToString(), *Box.Max.ToString(), G, *Frame.Min.ToString(), *Frame.Max.ToString()));
					}
				}
				else if (TooClose(Frame, Box))
				{
					Fail(FString::Printf(TEXT("Node %d (%s)-(%s) within spacing of group %d (%s)-(%s)"), Node, *Box.Min.ToString(), *Box.Max.ToString(), G, *Frame.Min.ToString(), *Frame.Max.ToString()));
				}
			}
			for (int32 Other = G + 1; Other < Input.Groups.Num(); ++Other)
			{
				const FBox2f& OtherFrame = Result.GroupBounds[Other];
				if (!OtherFrame.bIsValid)
				{
					continue;
				}
				if (IsGroupAncestor(Input, G, Other))
				{
					if (!Contains(Inner(G), OtherFrame))
					{
						Fail(FString::Printf(TEXT("Group %d not inside its ancestor %d"), Other, G));
					}
				}
				else if (IsGroupAncestor(Input, Other, G))
				{
					if (!Contains(Inner(Other), Frame))
					{
						Fail(FString::Printf(TEXT("Group %d not inside its ancestor %d"), G, Other));
					}
				}
				else if (TooClose(Frame, OtherFrame))
				{
					Fail(FString::Printf(TEXT("Unrelated groups %d and %d within spacing"), G, Other));
				}
			}
		}
		return Violations;
	}

	/** NumGroups groups (each nested in a random earlier one half the time); each node joins a random group with MemberChance. */
	static void AddRandomGroups(FLayoutInput& Input, int32 Seed, int32 NumGroups, float MemberChance)
	{
		FRandomStream Rand(Seed);
		for (int32 G = 0; G < NumGroups; ++G)
		{
			FLayoutGroup Group;
			Group.Parent = (G > 0 && Rand.FRand() < 0.5f) ? Rand.RandRange(0, G - 1) : INDEX_NONE;
			Group.PaddingMin = FVector2f(Rand.FRandRange(0.f, 40.f), Rand.FRandRange(20.f, 60.f));
			Group.PaddingMax = FVector2f(Rand.FRandRange(0.f, 40.f), Rand.FRandRange(0.f, 40.f));
			Input.Groups.Add(Group);
		}
		for (FLayoutNode& Node : Input.Nodes)
		{
			if (Rand.FRand() < MemberChance)
			{
				Node.Group = Rand.RandRange(0, NumGroups - 1);
			}
		}
	}

	/** Primary (exec-like) chain of Count nodes left to right; returns their indices. */
	static TArray<int32> AddChain(FLayoutInput& Input, int32 Count, FVector2f Start)
	{
		TArray<int32> Chain;
		for (int32 Idx = 0; Idx < Count; ++Idx)
		{
			const int32 Node = Input.Nodes.Add(MakeNode(Start + FVector2f(Idx * 300.f, (Idx % 2) * 120.f), FVector2f(180.f, 80.f), 1, 1));
			if (Chain.Num() > 0)
			{
				Connect(Input, Chain.Last(), 0, Node, 0, true);
			}
			Chain.Add(Node);
		}
		return Chain;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupNoInterleaveTest, "GraphAutoLayout.Core.Groups.NoInterleave", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupNoInterleaveTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// Material-like: X, Y and Z feed the sink's three inputs top to bottom; only X and Z are in the group.
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSinks;
	const int32 Sink = Input.Nodes.Add(MakeNode(FVector2f(1000.f, 0.f), FVector2f(200.f, 140.f), 3, 0));
	const int32 X = Input.Nodes.Add(MakeNode(FVector2f(600.f, -200.f), FVector2f(180.f, 80.f), 1, 1));
	const int32 Y = Input.Nodes.Add(MakeNode(FVector2f(600.f, 0.f), FVector2f(180.f, 80.f), 1, 1));
	const int32 Z = Input.Nodes.Add(MakeNode(FVector2f(600.f, 200.f), FVector2f(180.f, 80.f), 1, 1));
	const int32 XIn = Input.Nodes.Add(MakeNode(FVector2f(200.f, -200.f), FVector2f(150.f, 60.f), 0, 1));
	const int32 ZIn = Input.Nodes.Add(MakeNode(FVector2f(200.f, 200.f), FVector2f(150.f, 60.f), 0, 1));
	Input.Nodes[Sink].AnchorPriority = 100;
	Connect(Input, X, 0, Sink, 0);
	Connect(Input, Y, 0, Sink, 1);
	Connect(Input, Z, 0, Sink, 2);
	Connect(Input, XIn, 0, X, 0);
	Connect(Input, ZIn, 0, Z, 0);

	const FLayoutResult Plain = FGraphLayoutEngine::Run(Input);
	const FBox2f PlainXZ = NodeBox(Input, Plain, X) + NodeBox(Input, Plain, Z);
	TestTrue(TEXT("Control: without the group, Y lands between X and Z"), PlainXZ.Intersect(NodeBox(Input, Plain, Y)));

	Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
	Input.Nodes[X].Group = 0;
	Input.Nodes[Z].Group = 0;
	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	const FBox2f XZ = NodeBox(Input, Result, X) + NodeBox(Input, Result, Z);
	TestFalse(TEXT("Y is no longer between X and Z"), XZ.Intersect(NodeBox(Input, Result, Y)));
	TestEqual(TEXT("Group invariants"), CheckGroups(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestEqual(TEXT("One group counted"), Result.Stats.NumGroups, 1);
	TestTrue(TEXT("Sink (anchor) keeps its position"), Result.Positions[Sink].Equals(Input.Nodes[Sink].OrigPos, 0.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupSplitTest, "GraphAutoLayout.Core.Groups.SplitGroup", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupSplitTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// A -> B -> C -> D -> E with {B, D} grouped: the chain runs out of the group and back in (a cycle through the block).
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	const TArray<int32> Chain = AddChain(Input, 5, FVector2f(0.f, 0.f));
	Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
	Input.Nodes[Chain[1]].Group = 0;
	Input.Nodes[Chain[3]].Group = 0;

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestEqual(TEXT("Group invariants (C stays outside the group)"), CheckGroups(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupNestedTest, "GraphAutoLayout.Core.Groups.Nested", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupNestedTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// A -> B -> C -> D -> E plus a lone F. Outer group {B, D, F} holds an inner group {C}. B is the anchor.
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	const TArray<int32> Chain = AddChain(Input, 5, FVector2f(0.f, 0.f));
	const int32 Lone = Input.Nodes.Add(MakeNode(FVector2f(500.f, 900.f), FVector2f(160.f, 70.f), 1, 1));
	Input.Groups.Add(MakeCommentGroup(INDEX_NONE)); // 0: outer
	Input.Groups.Add(MakeCommentGroup(0));          // 1: inner
	Input.Nodes[Chain[1]].Group = 0;
	Input.Nodes[Chain[3]].Group = 0;
	Input.Nodes[Lone].Group = 0;
	Input.Nodes[Chain[2]].Group = 1;
	Input.Nodes[Chain[1]].AnchorPriority = 10;

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestEqual(TEXT("Group invariants"), CheckGroups(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestTrue(TEXT("Anchor inside a group keeps its position"), Result.Positions[Chain[1]].Equals(Input.Nodes[Chain[1]].OrigPos, 0.5f));
	TestEqual(TEXT("Two groups counted"), Result.Stats.NumGroups, 2);
	TestEqual(TEXT("Nesting depth"), Result.Stats.MaxGroupDepth, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupBoundaryTest, "GraphAutoLayout.Core.Groups.Boundary", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupBoundaryTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// Exec flow: inside the group G1 -> G2 -> G3, and G1 -> K where K feeds a node outside. K moves to the group's right
	// edge (G3's column) so its outgoing wire doesn't cross the group.
	{
		FLayoutInput Input;
		Input.Params.Mode = ELayeringMode::FromSources;
		const int32 Source = Input.Nodes.Add(MakeNode(FVector2f(-400.f, 0.f), FVector2f(150.f, 70.f), 0, 1));
		const int32 G1 = Input.Nodes.Add(MakeNode(FVector2f(0.f, 0.f), FVector2f(180.f, 80.f), 1, 2));
		const int32 G2 = Input.Nodes.Add(MakeNode(FVector2f(300.f, 0.f), FVector2f(180.f, 80.f), 1, 1));
		const int32 G3 = Input.Nodes.Add(MakeNode(FVector2f(600.f, 0.f), FVector2f(200.f, 80.f), 1, 1));
		const int32 K = Input.Nodes.Add(MakeNode(FVector2f(300.f, 200.f), FVector2f(140.f, 60.f), 1, 1));
		const int32 Target = Input.Nodes.Add(MakeNode(FVector2f(1200.f, 200.f), FVector2f(150.f, 70.f), 1, 0));
		Input.Nodes[Source].AnchorPriority = 10;
		Connect(Input, Source, 0, G1, 0, true);
		Connect(Input, G1, 0, G2, 0, true);
		Connect(Input, G2, 0, G3, 0, true);
		Connect(Input, G1, 1, K, 0);
		Connect(Input, K, 0, Target, 0);
		Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
		for (int32 Member : { G1, G2, G3, K })
		{
			Input.Nodes[Member].Group = 0;
		}
		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		TestEqual(TEXT("Exit node K sits in the group's last column"), Result.Positions[K].X, Result.Positions[G3].X, 0.5f);
		TestEqual(TEXT("Exec flow: group invariants"), CheckGroups(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	}

	// Data flow: inside the group H1 -> H2 -> H3, and J -> H3 where J is fed from outside. J moves to the group's left
	// edge (H1's column, right-aligned like every sink-anchored column) so its incoming wire doesn't cross the group.
	{
		FLayoutInput Input;
		Input.Params.Mode = ELayeringMode::FromSinks;
		const int32 H1 = Input.Nodes.Add(MakeNode(FVector2f(0.f, 0.f), FVector2f(180.f, 80.f), 1, 1));
		const int32 H2 = Input.Nodes.Add(MakeNode(FVector2f(300.f, 0.f), FVector2f(180.f, 80.f), 1, 1));
		const int32 H3 = Input.Nodes.Add(MakeNode(FVector2f(600.f, 0.f), FVector2f(200.f, 100.f), 2, 1));
		const int32 J = Input.Nodes.Add(MakeNode(FVector2f(300.f, 200.f), FVector2f(140.f, 60.f), 1, 1));
		const int32 Sink = Input.Nodes.Add(MakeNode(FVector2f(1000.f, 0.f), FVector2f(150.f, 70.f), 1, 0));
		const int32 Feed = Input.Nodes.Add(MakeNode(FVector2f(-500.f, 300.f), FVector2f(150.f, 60.f), 0, 1));
		Input.Nodes[Sink].AnchorPriority = 100;
		Connect(Input, H1, 0, H2, 0);
		Connect(Input, H2, 0, H3, 0);
		Connect(Input, J, 0, H3, 1);
		Connect(Input, H3, 0, Sink, 0);
		Connect(Input, Feed, 0, J, 0);
		Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
		for (int32 Member : { H1, H2, H3, J })
		{
			Input.Nodes[Member].Group = 0;
		}
		const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
		const float RightJ = Result.Positions[J].X + Input.Nodes[J].Size.X;
		const float RightH1 = Result.Positions[H1].X + Input.Nodes[H1].Size.X;
		TestEqual(TEXT("Entry node J sits in the group's first column"), RightJ, RightH1, 0.5f);
		TestEqual(TEXT("Data flow: group invariants"), CheckGroups(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupSatelliteTest, "GraphAutoLayout.Core.Groups.Satellites", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupSatelliteTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// Blueprint-like: event E -> X (exec), X in a group. Pure P outside the group and pure Q inside it both feed X.
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	const int32 Event = Input.Nodes.Add(MakeNode(FVector2f(0.f, 0.f), FVector2f(160.f, 70.f), 0, 1));
	FLayoutNode XNode = MakeNode(FVector2f(400.f, 0.f), FVector2f(220.f, 110.f), 3, 1);
	XNode.SatelliteMinTop = PinTop + PinRow * 0.5f;
	const int32 X = Input.Nodes.Add(XNode);
	FLayoutNode Pure = MakeNode(FVector2f(0.f, 400.f), FVector2f(140.f, 60.f), 0, 1);
	Pure.bSatellite = true;
	const int32 P = Input.Nodes.Add(Pure);
	Pure.OrigPos = FVector2f(0.f, 600.f);
	const int32 Q = Input.Nodes.Add(Pure);
	Input.Nodes[Event].AnchorPriority = 10;
	Connect(Input, Event, 0, X, 0, true);
	Connect(Input, P, 0, X, 1);
	Connect(Input, Q, 0, X, 2);
	Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
	Input.Nodes[X].Group = 0;
	Input.Nodes[Q].Group = 0;

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	const float MinH = FMath::Min(Input.Params.HorizontalSpacing, Input.Params.SatelliteHorizontalSpacing);
	const float MinV = FMath::Min(Input.Params.VerticalSpacing, Input.Params.SatelliteVerticalSpacing);
	const FBox2f Frame = Result.GroupBounds.IsValidIndex(0) ? Result.GroupBounds[0] : FBox2f(ForceInit);
	TestTrue(TEXT("Outside pure node hangs left of the group"), Frame.bIsValid
		&& Result.Positions[P].X + Input.Nodes[P].Size.X <= Frame.Min.X - (Input.Params.SatelliteHorizontalSpacing - 0.5f));
	TestTrue(TEXT("Outside pure node stays below the exec wire into the group"), Result.Positions[P].Y >= Result.Positions[X].Y + XNode.SatelliteMinTop - 0.5f);
	TestEqual(TEXT("Group invariants"), CheckGroups(*this, Input, Result, MinH, MinV), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, MinH, MinV), 0);
	TestTrue(TEXT("Event (anchor) keeps its position"), Result.Positions[Event].Equals(Input.Nodes[Event].OrigPos, 0.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupRandomTest, "GraphAutoLayout.Core.Groups.Random", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupRandomTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	for (int32 Seed = 1; Seed <= 5; ++Seed)
	{
		FLayoutInput Input = MakeRandomDag(150, Seed, Seed % 2 ? ELayeringMode::FromSources : ELayeringMode::FromSinks);
		AddRandomGroups(Input, Seed, 8, 0.6f);
		const FLayoutResult A = FGraphLayoutEngine::Run(Input);
		const FLayoutResult B = FGraphLayoutEngine::Run(Input);
		TestEqual(FString::Printf(TEXT("Seed %d: group invariants"), Seed), CheckGroups(*this, Input, A, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
		TestEqual(FString::Printf(TEXT("Seed %d: overlaps"), Seed), CountOverlaps(*this, Input, A, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
		TestTrue(FString::Printf(TEXT("Seed %d: deterministic"), Seed), A.Positions == B.Positions && A.GroupBounds == B.GroupBounds);
	}

	FLayoutInput Big = MakeRandomDag(500, 99, ELayeringMode::FromSources);
	AddRandomGroups(Big, 99, 40, 0.7f);
	const FLayoutResult Result = FGraphLayoutEngine::Run(Big);
	AddInfo(FString::Printf(TEXT("500 nodes, 40 groups (%d with members, depth %d): %.2f ms"), Result.Stats.NumGroups, Result.Stats.MaxGroupDepth, Result.Stats.Milliseconds));
	// Generous bound so the test isn't flaky on a busy machine.
	TestTrue(TEXT("500 nodes with 40 groups lay out in under 250 ms"), Result.Stats.Milliseconds < 250.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutGroupValidationTest, "GraphAutoLayout.Core.Groups.Validation", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutGroupValidationTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::Tests;

	// Broken input: groups 0 and 1 are each other's parent, group 2 points at a missing parent, group 3 has no members,
	// and one node names a missing group. None of it may crash or hang.
	FLayoutInput Input;
	Input.Params.Mode = ELayeringMode::FromSources;
	const TArray<int32> Chain = AddChain(Input, 6, FVector2f(0.f, 0.f));
	Input.Groups.Add(MakeCommentGroup(1));
	Input.Groups.Add(MakeCommentGroup(0));
	Input.Groups.Add(MakeCommentGroup(7));
	Input.Groups.Add(MakeCommentGroup(INDEX_NONE));
	Input.Nodes[Chain[1]].Group = 0;
	Input.Nodes[Chain[2]].Group = 1;
	Input.Nodes[Chain[3]].Group = 2;
	Input.Nodes[Chain[4]].Group = 99;

	const FLayoutResult Result = FGraphLayoutEngine::Run(Input);
	TestEqual(TEXT("One bounds entry per group"), Result.GroupBounds.Num(), 4);
	TestFalse(TEXT("Group without members has no frame"), Result.GroupBounds.IsValidIndex(3) && Result.GroupBounds[3].bIsValid);

	// How the engine resolves it: the cycle is cut where the walk from group 0 closes it (group 1 becomes top level),
	// the missing parent and the missing group mean top level.
	FLayoutInput Resolved = Input;
	Resolved.Groups[1].Parent = INDEX_NONE;
	Resolved.Groups[2].Parent = INDEX_NONE;
	Resolved.Nodes[Chain[4]].Group = INDEX_NONE;
	TestEqual(TEXT("Group invariants after resolving"), CheckGroups(*this, Resolved, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	TestEqual(TEXT("No overlaps"), CountOverlaps(*this, Input, Result, Input.Params.HorizontalSpacing, Input.Params.VerticalSpacing), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
