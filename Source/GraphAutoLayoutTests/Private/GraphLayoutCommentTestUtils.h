// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphLayoutTestUtils.h"

namespace UE::GraphAutoLayout::Tests
{
	/** A comment in Group Movement mode (Unreal's default), set explicitly so editor preferences can't change it. */
	inline UEdGraphNode_Comment* AddComment(UEdGraph& Graph, FIntPoint Pos, FIntPoint Size)
	{
		FGraphNodeCreator<UEdGraphNode_Comment> Creator(Graph);
		UEdGraphNode_Comment* Comment = Creator.CreateNode(false);
		Creator.Finalize();
		Comment->NodePosX = Pos.X;
		Comment->NodePosY = Pos.Y;
		Comment->NodeWidth = Size.X;
		Comment->NodeHeight = Size.Y;
		// PostPlacedNewNode copies the editor's default move mode (EdGraphNode_Comment.cpp), which a user may have changed.
		Comment->MoveMode = ECommentBoxMode::GroupMovement;
		return Comment;
	}

	inline FBox2f CommentBox(const UEdGraphNode_Comment& Comment)
	{
		const FVector2f Min(static_cast<float>(Comment.NodePosX), static_cast<float>(Comment.NodePosY));
		return FBox2f(Min, Min + FVector2f(static_cast<float>(Comment.NodeWidth), static_cast<float>(Comment.NodeHeight)));
	}

	struct FCommentState
	{
		int32 X, Y, W, H;
		bool operator==(const FCommentState& O) const { return X == O.X && Y == O.Y && W == O.W && H == O.H; }
	};

	inline FCommentState StateOf(const UEdGraphNode_Comment& Comment)
	{
		return { Comment.NodePosX, Comment.NodePosY, Comment.NodeWidth, Comment.NodeHeight };
	}

	/** Restores a settings value when the test ends, whatever happens. */
	template <typename T>
	struct TSettingOverride
	{
		T& Value;
		T Old;
		TSettingOverride(T& InValue, T NewValue) : Value(InValue), Old(InValue) { Value = NewValue; }
		~TSettingOverride() { Value = Old; }
	};

	/** Title bar height the runner reserves above a comment's contents (same estimate as GraphAutoLayoutRunner.cpp). */
	inline float CommentTitleHeight(const UEdGraphNode_Comment& Comment)
	{
		return Comment.FontSize * 1.5f + 12.f;
	}

	/** Comment contains Node with at least Padding on every side and the title bar above it (1 unit of rounding slack). */
	inline bool WrapsNode(const FBox2f& Comment, const FBox2f& Node, float Padding, float TitleHeight)
	{
		constexpr float Slack = 1.f;
		return Comment.Min.X <= Node.Min.X - Padding + Slack && Comment.Max.X >= Node.Max.X + Padding - Slack
			&& Comment.Min.Y <= Node.Min.Y - Padding - TitleHeight + Slack && Comment.Max.Y >= Node.Max.Y + Padding - Slack;
	}

	inline bool ContainsBox(const FBox2f& Outer, const FBox2f& Inner)
	{
		constexpr float Slack = 1.f;
		return Inner.Min.X >= Outer.Min.X - Slack && Inner.Min.Y >= Outer.Min.Y - Slack
			&& Inner.Max.X <= Outer.Max.X + Slack && Inner.Max.Y <= Outer.Max.Y + Slack;
	}

	/** Closer than the layout spacing on both axes (1 unit of rounding slack). */
	inline bool TooClose(const FBox2f& A, const FBox2f& B, float HSpacing, float VSpacing)
	{
		return GapX(A, B) < HSpacing - 1.f && GapY(A, B) < VSpacing - 1.f;
	}

	/** Laid-out nodes that are closer than the spacing to each other or to an obstacle (unselected node). */
	inline int32 CountNodeOverlaps(const FGraphAutoLayoutRunResult& Result, float HSpacing, float VSpacing)
	{
		TArray<FBox2f> Laid;
		Result.NodeBounds.GenerateValueArray(Laid);
		int32 Count = 0;
		for (int32 A = 0; A < Laid.Num(); ++A)
		{
			for (int32 B = A + 1; B < Laid.Num(); ++B)
			{
				Count += TooClose(Laid[A], Laid[B], HSpacing, VSpacing) ? 1 : 0;
			}
			for (const TPair<const UEdGraphNode*, FBox2f>& Obstacle : Result.ObstacleBounds)
			{
				Count += TooClose(Laid[A], Obstacle.Value, HSpacing, VSpacing) ? 1 : 0;
			}
		}
		return Count;
	}

	inline TMap<UEdGraphNode*, FIntPoint> PositionsOf(const TArray<UEdGraphNode*>& Nodes)
	{
		TMap<UEdGraphNode*, FIntPoint> Positions;
		for (UEdGraphNode* Node : Nodes)
		{
			Positions.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
		}
		return Positions;
	}

	inline bool SamePositions(const TMap<UEdGraphNode*, FIntPoint>& A, const TMap<UEdGraphNode*, FIntPoint>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (const TPair<UEdGraphNode*, FIntPoint>& Pair : A)
		{
			const FIntPoint* Other = B.Find(Pair.Key);
			if (!Other || *Other != Pair.Value)
			{
				return false;
			}
		}
		return true;
	}
}
