// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GraphLayoutTypes.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

namespace UE::GraphAutoLayout::Tests
{
	inline constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	constexpr float PinTop = 30.f;
	constexpr float PinRow = 24.f;

	/** Node with NumIn input pins followed by NumOut output pins, rows laid out like a typical graph node. */
	inline FLayoutNode MakeNode(FVector2f Pos, FVector2f Size, int32 NumIn, int32 NumOut)
	{
		FLayoutNode Node;
		Node.OrigPos = Pos;
		Node.Size = Size;
		for (int32 Idx = 0; Idx < NumIn; ++Idx)
		{
			Node.Pins.Add({ EPinSide::Input, PinTop + Idx * PinRow });
		}
		for (int32 Idx = 0; Idx < NumOut; ++Idx)
		{
			Node.Pins.Add({ EPinSide::Output, PinTop + Idx * PinRow });
		}
		return Node;
	}

	/** Index of the K-th output pin (outputs follow inputs). */
	inline int32 OutPin(const FLayoutNode& Node, int32 K)
	{
		int32 NumIn = 0;
		for (const FLayoutPin& Pin : Node.Pins)
		{
			NumIn += Pin.Side == EPinSide::Input ? 1 : 0;
		}
		return NumIn + K;
	}

	inline void Connect(FLayoutInput& Input, int32 Src, int32 SrcOut, int32 Dst, int32 DstIn, bool bPrimary = false)
	{
		Input.Edges.Add({ Src, OutPin(Input.Nodes[Src], SrcOut), Dst, DstIn, bPrimary });
	}

	inline FBox2f NodeBox(const FLayoutInput& Input, const FLayoutResult& Result, int32 Idx)
	{
		return FBox2f(Result.Positions[Idx], Result.Positions[Idx] + Input.Nodes[Idx].Size);
	}

	inline float GapX(const FBox2f& A, const FBox2f& B) { return FMath::Max(B.Min.X - A.Max.X, A.Min.X - B.Max.X); }
	inline float GapY(const FBox2f& A, const FBox2f& B) { return FMath::Max(B.Min.Y - A.Max.Y, A.Min.Y - B.Max.Y); }

	/** Boxes must be separated by at least the spacing on one axis. Returns the number of violating pairs. */
	inline int32 CountOverlaps(FAutomationTestBase& Test, const FLayoutInput& Input, const FLayoutResult& Result, float HSpacing, float VSpacing)
	{
		constexpr float Tolerance = 0.5f;
		int32 Violations = 0;
		for (int32 A = 0; A < Input.Nodes.Num(); ++A)
		{
			const FBox2f BoxA = NodeBox(Input, Result, A);
			for (int32 B = A + 1; B < Input.Nodes.Num(); ++B)
			{
				const FBox2f BoxB = NodeBox(Input, Result, B);
				if (GapX(BoxA, BoxB) < HSpacing - Tolerance && GapY(BoxA, BoxB) < VSpacing - Tolerance)
				{
					if (Violations < 5)
					{
						Test.AddError(FString::Printf(TEXT("Nodes %d and %d too close: A=(%s)-(%s) B=(%s)-(%s)"), A, B,
							*BoxA.Min.ToString(), *BoxA.Max.ToString(), *BoxB.Min.ToString(), *BoxB.Max.ToString()));
					}
					++Violations;
				}
			}
		}
		return Violations;
	}

	/** Every edge leaves its source on the right and enters its destination further right. */
	inline int32 CountBackwardEdges(const FLayoutInput& Input, const FLayoutResult& Result)
	{
		int32 Backward = 0;
		for (const FLayoutEdge& Edge : Input.Edges)
		{
			const float SrcRight = Result.Positions[Edge.SrcNode].X + Input.Nodes[Edge.SrcNode].Size.X;
			if (SrcRight >= Result.Positions[Edge.DstNode].X)
			{
				++Backward;
			}
		}
		return Backward;
	}

	/** Random layered DAG with random node sizes; edges always go from a lower to a higher rank. */
	inline FLayoutInput MakeRandomDag(int32 NumNodes, int32 Seed, ELayeringMode Mode)
	{
		FRandomStream Rand(Seed);
		FLayoutInput Input;
		Input.Params.Mode = Mode;
		const int32 NumRanks = FMath::Max(1, FMath::RoundToInt(FMath::Sqrt(static_cast<float>(NumNodes))));
		TArray<int32> Rank;
		TArray<int32> NumInputs;
		TArray<int32> NumOutputs;
		for (int32 Idx = 0; Idx < NumNodes; ++Idx)
		{
			const int32 NumIn = Rand.RandRange(1, 4);
			const int32 NumOut = Rand.RandRange(1, 2);
			const FVector2f Size(Rand.FRandRange(80.f, 320.f), PinTop + FMath::Max(NumIn, NumOut) * PinRow + Rand.FRandRange(0.f, 60.f));
			const FVector2f Pos(Rand.FRandRange(-3000.f, 3000.f), Rand.FRandRange(-3000.f, 3000.f));
			Input.Nodes.Add(MakeNode(Pos, Size, NumIn, NumOut));
			Rank.Add(Rand.RandRange(0, NumRanks - 1));
			NumInputs.Add(NumIn);
			NumOutputs.Add(NumOut);
		}
		for (int32 Dst = 0; Dst < NumNodes; ++Dst)
		{
			for (int32 Attempt = 0; Attempt < 2; ++Attempt)
			{
				const int32 Src = Rand.RandRange(0, NumNodes - 1);
				if (Rank[Src] < Rank[Dst])
				{
					Connect(Input, Src, Rand.RandRange(0, NumOutputs[Src] - 1), Dst, Rand.RandRange(0, NumInputs[Dst] - 1));
				}
			}
		}
		return Input;
	}
}
