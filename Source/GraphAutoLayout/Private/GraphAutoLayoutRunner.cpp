// Copyright 2026 Jot. All Rights Reserved.

#include "GraphAutoLayoutRunner.h"

#include "Adapters/AnimGraphLayoutAdapter.h"
#include "Adapters/BlueprintLayoutAdapter.h"
#include "Adapters/MaterialLayoutAdapter.h"
#include "Adapters/NiagaraScratchPadLayoutAdapter.h"
#include "Adapters/PCGLayoutAdapter.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "GraphAutoLayoutLog.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphLayoutEngine.h"
#include "GraphNodeMeasurer.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayout"

namespace UE::GraphAutoLayout
{
	const TArray<TUniquePtr<IGraphLayoutAdapter>>& GetGraphLayoutAdapters()
	{
		static const TArray<TUniquePtr<IGraphLayoutAdapter>> Adapters = []()
		{
			TArray<TUniquePtr<IGraphLayoutAdapter>> Result;
			// Anim schemas derive from the K2 schema, so they must be matched before plain Blueprints.
			Result.Add(MakeUnique<FAnimGraphLayoutAdapter>());
			Result.Add(MakeUnique<FBlueprintLayoutAdapter>());
			Result.Add(MakeUnique<FMaterialLayoutAdapter>());
			Result.Add(MakeUnique<FNiagaraScratchPadLayoutAdapter>());
			Result.Add(MakeUnique<FPCGLayoutAdapter>());
			return Result;
		}();
		return Adapters;
	}

	const IGraphLayoutAdapter* FindGraphLayoutAdapter(const UEdGraph& Graph)
	{
		for (const TUniquePtr<IGraphLayoutAdapter>& Adapter : GetGraphLayoutAdapters())
		{
			if (Adapter->Supports(Graph))
			{
				return Adapter.Get();
			}
		}
		return nullptr;
	}

	namespace RunnerPrivate
	{
		static bool IsCommentNode(const UEdGraphNode& Node)
		{
			return Node.IsA<UEdGraphNode_Comment>();
		}

		static FVector2f NodePos(const UEdGraphNode& Node)
		{
			return FVector2f(static_cast<float>(Node.NodePosX), static_cast<float>(Node.NodePosY));
		}

		/** Finds an enabled adapter that may run here; fills Result.AdapterName/Message either way. */
		static const IGraphLayoutAdapter* ResolveAdapter(const UEdGraph& Graph, const SGraphPanel* Panel, FGraphAutoLayoutRunResult& Result)
		{
			const IGraphLayoutAdapter* Adapter = FindGraphLayoutAdapter(Graph);
			if (!Adapter)
			{
				const UEdGraphSchema* Schema = Graph.GetSchema();
				Result.Message = FText::Format(LOCTEXT("Unsupported", "Auto layout doesn't support this graph type ({0})."),
					FText::FromString(Schema ? Schema->GetClass()->GetName() : TEXT("no schema")));
				return nullptr;
			}
			Result.AdapterName = Adapter->GetName();
			if (!Adapter->IsEnabled())
			{
				Result.Message = FText::Format(LOCTEXT("Disabled", "Auto layout is disabled for {0} graphs in Editor Preferences."), FText::FromName(Adapter->GetName()));
				return nullptr;
			}
			if (Panel && !Adapter->CheckHost(*Panel, Result.Message))
			{
				return nullptr;
			}
			return Adapter;
		}

		static FBox2f CommentRect(const UEdGraphNode_Comment& Comment)
		{
			const FVector2f Min = NodePos(Comment);
			return FBox2f(Min, Min + FVector2f(static_cast<float>(Comment.NodeWidth), static_cast<float>(Comment.NodeHeight)));
		}

		/** Group Movement comments drag the nodes inside them along (ECommentBoxMode, EdGraphNode_Comment.h); plain "Comment" mode doesn't. */
		static bool IsGroupComment(const UEdGraphNode_Comment& Comment)
		{
			return Comment.MoveMode == ECommentBoxMode::GroupMovement;
		}

		/** Room a comment's title bar needs above its contents (an estimate: the real height depends on the widget and text). */
		static float CommentTitleHeight(const UEdGraphNode_Comment& Comment)
		{
			return Comment.FontSize * 1.5f + 12.f;
		}

		/** Moves and resizes a comment inside the current transaction. Returns false if it was already there. */
		static bool SetCommentRect(const IGraphLayoutAdapter& Adapter, UEdGraphNode_Comment& Comment, int32 X, int32 Y, int32 Width, int32 Height)
		{
			if (Comment.NodePosX == X && Comment.NodePosY == Y && Comment.NodeWidth == Width && Comment.NodeHeight == Height)
			{
				return false;
			}
			Adapter.PreModifyComment(Comment);
			Comment.Modify();
			Comment.NodePosX = X;
			Comment.NodePosY = Y;
			// ResizeNode is virtual: material comments sync their UMaterialExpressionComment and flag the editor dirty.
			Comment.ResizeNode(FVector2f(static_cast<float>(Width), static_cast<float>(Height)));
			return true;
		}

		struct FCommentToFit
		{
			UEdGraphNode_Comment* Comment = nullptr;
			TArray<const UEdGraphNode*> Contents;
		};

		/**
		 * Comments that contain at least one laid-out node. Contents = nodes whose centre was inside the comment
		 * before the layout, plus whatever the comment widget recorded as being under it. Unselected contents
		 * are kept so the resized comment still wraps them. With bPlainCommentsOnly, Group Movement comments are
		 * skipped (Keep Together handles those).
		 */
		static TArray<FCommentToFit> CollectCommentsToFit(UEdGraph& Graph, const TSet<const UEdGraphNode*>& Selected, const TMap<const UEdGraphNode*, FBox2f>& OrigBounds, bool bPlainCommentsOnly)
		{
			TArray<FCommentToFit> Out;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
				if (!Comment || (bPlainCommentsOnly && IsGroupComment(*Comment)))
				{
					continue;
				}
				const FBox2f Rect = CommentRect(*Comment);
				const FCommentNodeSet& UnderComment = Comment->GetNodesUnderComment();

				FCommentToFit Fit;
				Fit.Comment = Comment;
				bool bHasSelected = false;
				for (const TPair<const UEdGraphNode*, FBox2f>& Pair : OrigBounds)
				{
					bool bInside = Rect.IsInside(Pair.Value.GetCenter());
					for (const TObjectPtr<UObject>& Object : UnderComment)
					{
						bInside |= Object == Pair.Key;
					}
					if (bInside)
					{
						Fit.Contents.Add(Pair.Key);
						bHasSelected |= Selected.Contains(Pair.Key);
					}
				}
				if (bHasSelected)
				{
					Out.Add(MoveTemp(Fit));
				}
			}
			return Out;
		}

		/** A Group Movement comment in Keep Together mode, and where it sits in the comment tree. */
		struct FCommentGroup
		{
			UEdGraphNode_Comment* Comment = nullptr;
			FBox2f Rect = FBox2f(ForceInit);
			double Area = 0.0;
			/** Enclosing comment (index in the comment list), or INDEX_NONE. */
			int32 Parent = INDEX_NONE;
			int32 Depth = 0;
			/** Index in FLayoutInput::Groups once the comment holds laid-out nodes, directly or through nested comments. */
			int32 LayoutGroup = INDEX_NONE;
			/** Unselected nodes for which this is the innermost comment; it keeps wrapping them. */
			TArray<const UEdGraphNode*> Unselected;
		};

		/** Whether comment A encloses comment B: larger, or the same size and earlier in the graph (keeps identical boxes acyclic). */
		static bool IsOuterComment(const TArray<FCommentGroup>& Comments, int32 A, int32 B)
		{
			return Comments[A].Area != Comments[B].Area ? Comments[A].Area > Comments[B].Area : A < B;
		}

		/** Innermost comment whose box contains Point and that encloses comment Than (any comment if INDEX_NONE). */
		static int32 FindInnermostComment(const TArray<FCommentGroup>& Comments, const FVector2f& Point, int32 Than = INDEX_NONE)
		{
			int32 Best = INDEX_NONE;
			for (int32 Idx = 0; Idx < Comments.Num(); ++Idx)
			{
				if ((Than == INDEX_NONE || IsOuterComment(Comments, Idx, Than)) && Comments[Idx].Rect.IsInside(Point)
					&& (Best == INDEX_NONE || IsOuterComment(Comments, Best, Idx)))
				{
					Best = Idx;
				}
			}
			return Best;
		}

		/**
		 * Keep Together: builds the tree of Group Movement comments and turns each comment that holds laid-out nodes, directly
		 * or through nested comments, into a layout group. A node (laid out or not) belongs to the innermost comment containing
		 * its centre; a comment's parent is the innermost enclosing comment containing its centre. The engine's cached
		 * GetNodesUnderComment() isn't used: it is only refreshed when the comment is selected or resized
		 * (SGraphNodeComment.cpp, HandleSelection), so it can be stale. O(C^2 + N*C) for C comments and N nodes.
		 */
		static TArray<FCommentGroup> BuildCommentGroups(UEdGraph& Graph, const TMap<const UEdGraphNode*, FBox2f>& OrigBounds,
			const TMap<const UEdGraphNode*, int32>& NodeIndex, float Padding, FLayoutInput& Input)
		{
			TArray<FCommentGroup> Comments;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node);
				if (Comment && IsGroupComment(*Comment))
				{
					FCommentGroup& Group = Comments.AddDefaulted_GetRef();
					Group.Comment = Comment;
					Group.Rect = CommentRect(*Comment);
					Group.Area = static_cast<double>(Comment->NodeWidth) * static_cast<double>(Comment->NodeHeight);
				}
			}
			for (int32 Idx = 0; Idx < Comments.Num(); ++Idx)
			{
				Comments[Idx].Parent = FindInnermostComment(Comments, Comments[Idx].Rect.GetCenter(), Idx);
			}
			for (FCommentGroup& Group : Comments)
			{
				for (int32 Ancestor = Group.Parent; Ancestor != INDEX_NONE; Ancestor = Comments[Ancestor].Parent)
				{
					++Group.Depth;
				}
			}

			TArray<int32> NodeComment;
			NodeComment.Init(INDEX_NONE, Input.Nodes.Num());
			TArray<bool> bHoldsLaidOut;
			bHoldsLaidOut.Init(false, Comments.Num());
			for (const TPair<const UEdGraphNode*, FBox2f>& Pair : OrigBounds)
			{
				const int32 Innermost = FindInnermostComment(Comments, Pair.Value.GetCenter());
				if (Innermost == INDEX_NONE)
				{
					continue;
				}
				if (const int32* Laid = NodeIndex.Find(Pair.Key))
				{
					NodeComment[*Laid] = Innermost;
					for (int32 Ancestor = Innermost; Ancestor != INDEX_NONE && !bHoldsLaidOut[Ancestor]; Ancestor = Comments[Ancestor].Parent)
					{
						bHoldsLaidOut[Ancestor] = true;
					}
				}
				else
				{
					Comments[Innermost].Unselected.Add(Pair.Key);
				}
			}

			for (int32 Idx = 0; Idx < Comments.Num(); ++Idx)
			{
				if (bHoldsLaidOut[Idx])
				{
					Comments[Idx].LayoutGroup = Input.Groups.Num();
					FLayoutGroup& LayoutGroup = Input.Groups.AddDefaulted_GetRef();
					LayoutGroup.PaddingMin = FVector2f(Padding, Padding + CommentTitleHeight(*Comments[Idx].Comment));
					LayoutGroup.PaddingMax = FVector2f(Padding, Padding);
				}
			}
			for (const FCommentGroup& Group : Comments)
			{
				// Every comment enclosing one that holds laid-out nodes holds them too, so the parent is a group as well.
				if (Group.LayoutGroup != INDEX_NONE && Group.Parent != INDEX_NONE)
				{
					Input.Groups[Group.LayoutGroup].Parent = Comments[Group.Parent].LayoutGroup;
				}
			}
			for (int32 Idx = 0; Idx < NodeComment.Num(); ++Idx)
			{
				if (NodeComment[Idx] != INDEX_NONE)
				{
					Input.Nodes[Idx].Group = Comments[NodeComment[Idx]].LayoutGroup;
				}
			}
			return Comments;
		}

		/**
		 * Keep Together: each group comment becomes its group's frame, grown to keep wrapping what it held that didn't move
		 * (unselected nodes, nested comments that aren't groups). Deepest first, so a comment wraps its children's final
		 * boxes. Returns the number of comments that changed.
		 */
		static int32 FitGroupComments(const IGraphLayoutAdapter& Adapter, const TArray<FCommentGroup>& Comments, const FLayoutResult& Layout,
			const TMap<const UEdGraphNode*, FBox2f>& ObstacleBounds, float Padding)
		{
			TArray<FBox2f> Final;
			TArray<int32> Order;
			for (int32 Idx = 0; Idx < Comments.Num(); ++Idx)
			{
				Final.Add(Comments[Idx].Rect);
				if (Comments[Idx].LayoutGroup != INDEX_NONE)
				{
					Order.Add(Idx);
				}
			}
			Order.StableSort([&Comments](int32 A, int32 B) { return Comments[A].Depth > Comments[B].Depth; });

			int32 NumChanged = 0;
			for (int32 Idx : Order)
			{
				const FCommentGroup& Group = Comments[Idx];
				const FVector2f GrowMin(Padding, Padding + CommentTitleHeight(*Group.Comment));
				const FVector2f GrowMax(Padding, Padding);
				FBox2f Rect = Layout.GroupBounds.IsValidIndex(Group.LayoutGroup) ? Layout.GroupBounds[Group.LayoutGroup] : FBox2f(ForceInit);
				for (const UEdGraphNode* Node : Group.Unselected)
				{
					const FBox2f& Bounds = ObstacleBounds.FindChecked(Node);
					Rect += FBox2f(Bounds.Min - GrowMin, Bounds.Max + GrowMax);
				}
				for (int32 Child = 0; Child < Comments.Num(); ++Child)
				{
					if (Comments[Child].Parent == Idx)
					{
						Rect += FBox2f(Final[Child].Min - GrowMin, Final[Child].Max + GrowMax);
					}
				}
				if (!Rect.bIsValid)
				{
					continue;
				}
				// Round outwards so the comment never cuts into what it wraps.
				const int32 X = FMath::FloorToInt(Rect.Min.X);
				const int32 Y = FMath::FloorToInt(Rect.Min.Y);
				const int32 Width = FMath::CeilToInt(Rect.Max.X) - X;
				const int32 Height = FMath::CeilToInt(Rect.Max.Y) - Y;
				Final[Idx] = FBox2f(FVector2f(static_cast<float>(X), static_cast<float>(Y)), FVector2f(static_cast<float>(X + Width), static_cast<float>(Y + Height)));
				NumChanged += SetCommentRect(Adapter, *Group.Comment, X, Y, Width, Height) ? 1 : 0;
			}
			return NumChanged;
		}
	}
}

bool FGraphAutoLayoutRunner::CanLayout(const UEdGraph* Graph, const SGraphPanel* Panel)
{
	FGraphAutoLayoutRunResult Unused;
	return Graph && UE::GraphAutoLayout::RunnerPrivate::ResolveAdapter(*Graph, Panel, Unused) != nullptr;
}

FGraphAutoLayoutRunResult FGraphAutoLayoutRunner::LayoutNodes(UEdGraph* Graph, const TArray<UEdGraphNode*>& Nodes, const SGraphPanel* Panel)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::RunnerPrivate;

	FGraphAutoLayoutRunResult Result;
	if (!Graph)
	{
		Result.Message = LOCTEXT("NoGraph", "No graph.");
		return Result;
	}
	const IGraphLayoutAdapter* Adapter = ResolveAdapter(*Graph, Panel, Result);
	if (!Adapter)
	{
		GRAPHAUTOLAYOUT_DEBUG(TEXT("%s"), *Result.Message.ToString());
		return Result;
	}

	const UGraphAutoLayoutSettings* Settings = GetDefault<UGraphAutoLayoutSettings>();
	const bool bKeepTogether = Settings->CommentMode == EGraphAutoLayoutCommentMode::KeepTogether;
	const FGraphNodeMeasurer Measurer(Panel);

	// Collect the nodes we lay out; comments are containers, not flow nodes.
	TArray<UEdGraphNode*> Selected;
	TSet<const UEdGraphNode*> SelectedSet;
	TArray<const UEdGraphNode_Comment*> SelectedGroupComments;
	for (UEdGraphNode* Node : Nodes)
	{
		if (!Node || SelectedSet.Contains(Node))
		{
			continue;
		}
		if (Node->GetGraph() != Graph)
		{
			GRAPHAUTOLAYOUT_DEBUG(TEXT("Skipping %s: not in graph %s"), *Node->GetName(), *Graph->GetName());
			continue;
		}
		if (IsCommentNode(*Node))
		{
			const UEdGraphNode_Comment& Comment = *CastChecked<UEdGraphNode_Comment>(Node);
			if (bKeepTogether && IsGroupComment(Comment))
			{
				SelectedGroupComments.AddUnique(&Comment);
			}
			else
			{
				GRAPHAUTOLAYOUT_DEBUG(TEXT("Skipping %s: comment nodes are not laid out"), *Node->GetName());
			}
			continue;
		}
		Selected.Add(Node);
		SelectedSet.Add(Node);
	}
	// Keep Together: a selected comment brings everything inside it along, the way dragging it does.
	// Each node is measured once, however many comments are selected (Ctrl+A selects them all).
	if (SelectedGroupComments.Num() > 0)
	{
		TArray<FBox2f> Rects;
		for (const UEdGraphNode_Comment* Comment : SelectedGroupComments)
		{
			Rects.Add(CommentRect(*Comment));
		}
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node || IsCommentNode(*Node) || SelectedSet.Contains(Node))
			{
				continue;
			}
			const FVector2f Centre = NodePos(*Node) + Measurer.Measure(*Node).Size * 0.5f;
			if (Rects.ContainsByPredicate([&Centre](const FBox2f& Rect) { return Rect.IsInside(Centre); }))
			{
				Selected.Add(Node);
				SelectedSet.Add(Node);
			}
		}
	}
	if (Selected.Num() == 0)
	{
		Result.Message = LOCTEXT("NothingToLayout", "Nothing to lay out: select some nodes first.");
		return Result;
	}

	FLayoutInput Input;
	Input.Params.Mode = Adapter->GetLayeringMode();
	Input.Params.HorizontalSpacing = Settings->HorizontalSpacing;
	Input.Params.VerticalSpacing = Settings->VerticalSpacing;
	Input.Params.SatelliteHorizontalSpacing = Settings->PureNodeHorizontalSpacing;
	Input.Params.SatelliteVerticalSpacing = Settings->PureNodeVerticalSpacing;
	Input.Params.bPackIslands = Settings->bCompactDisconnectedGroups;
	Input.Params.IslandSpacing = Settings->GroupSpacing;

	TMap<const UEdGraphNode*, int32> NodeIndex;
	TMap<const UEdGraphPin*, int32> PinIndex;
	TMap<const UEdGraphNode*, FBox2f> OrigBounds;
	TArray<FMeasuredNode> Measured;
	int32 NumEstimated = 0;
	for (UEdGraphNode* Node : Selected)
	{
		FMeasuredNode& M = Measured.Add_GetRef(Measurer.Measure(*Node));
		if (M.SizeSource != EMeasureSource::Widget || M.PinSource != EMeasureSource::Widget)
		{
			++NumEstimated;
			if (Panel)
			{
				GRAPHAUTOLAYOUT_DEBUG(TEXT("  %s: size from %s, pins from %s"), *Node->GetName(), LexToString(M.SizeSource), LexToString(M.PinSource));
			}
		}

		FLayoutNode& LayoutNode = Input.Nodes.AddDefaulted_GetRef();
		LayoutNode.Size = M.Size;
		LayoutNode.OrigPos = NodePos(*Node);
		for (const FMeasuredPin& MeasuredPin : M.Pins)
		{
			PinIndex.Add(MeasuredPin.Pin, LayoutNode.Pins.Num());
			LayoutNode.Pins.Add({ MeasuredPin.Pin->Direction == EGPD_Input ? EPinSide::Input : EPinSide::Output, MeasuredPin.OffsetY });
		}
		Adapter->ConfigureNode(*Node, M, LayoutNode);
		NodeIndex.Add(Node, Input.Nodes.Num() - 1);
		OrigBounds.Add(Node, FBox2f(LayoutNode.OrigPos, LayoutNode.OrigPos + M.Size));
	}

	// Edges: only links whose both ends are selected take part in the layout.
	for (int32 SrcIdx = 0; SrcIdx < Selected.Num(); ++SrcIdx)
	{
		for (const FMeasuredPin& MeasuredPin : Measured[SrcIdx].Pins)
		{
			if (MeasuredPin.Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* Linked : MeasuredPin.Pin->LinkedTo)
			{
				const UEdGraphNode* DstNode = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
				const int32* DstIdx = DstNode ? NodeIndex.Find(DstNode) : nullptr;
				if (!DstIdx)
				{
					continue;
				}
				const int32* DstPin = PinIndex.Find(Linked);
				Input.Edges.Add({ SrcIdx, PinIndex.FindChecked(MeasuredPin.Pin), *DstIdx, DstPin ? *DstPin : INDEX_NONE,
					Adapter->IsPrimaryLink(*MeasuredPin.Pin, *Linked) });
			}
		}
	}

	// Unselected nodes are never moved, so they become obstacles.
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && !SelectedSet.Contains(Node) && !IsCommentNode(*Node))
		{
			const FVector2f Pos = NodePos(*Node);
			const FBox2f Bounds(Pos, Pos + Measurer.Measure(*Node).Size);
			Input.Obstacles.Add(Bounds);
			Result.ObstacleBounds.Add(Node, Bounds);
			OrigBounds.Add(Node, Bounds);
		}
	}

	TArray<FCommentToFit> CommentsToFit;
	TArray<FCommentGroup> CommentGroups;
	if (bKeepTogether)
	{
		CommentGroups = BuildCommentGroups(*Graph, OrigBounds, NodeIndex, Settings->CommentPadding, Input);
		// Comments in plain "Comment" mode don't own their nodes: they are only refitted, as in Resize To Fit.
		CommentsToFit = CollectCommentsToFit(*Graph, SelectedSet, OrigBounds, /*bPlainCommentsOnly*/ true);
	}
	else if (Settings->CommentMode == EGraphAutoLayoutCommentMode::ResizeToFit)
	{
		CommentsToFit = CollectCommentsToFit(*Graph, SelectedSet, OrigBounds, /*bPlainCommentsOnly*/ false);
	}

	const FLayoutResult Layout = FGraphLayoutEngine::Run(Input);
	const FLayoutStats& Stats = Layout.Stats;
	GRAPHAUTOLAYOUT_DEBUG(TEXT("Adapter %s (%s zoom): %d nodes (%d not measured live), %d edges, %d obstacles, %d comment groups (depth %d), %d comments to fit -> %d islands, %d blocks, %d layers, %d dummies, %d reversed, crossings %lld -> %lld, %.2f ms"),
		*Adapter->GetName().ToString(), !Panel ? TEXT("no panel") : (Measurer.IsFullDetail() ? TEXT("full detail") : TEXT("low detail")),
		Input.Nodes.Num(), NumEstimated, Input.Edges.Num(), Input.Obstacles.Num(), Stats.NumGroups, Stats.MaxGroupDepth, CommentsToFit.Num(),
		Stats.NumIslands, Stats.NumBlocks, Stats.MaxLayers, Stats.NumDummies, Stats.NumReversedEdges,
		Stats.CrossingsBefore, Stats.CrossingsAfter, Stats.Milliseconds);

	FScopedTransaction Transaction(LOCTEXT("LayoutNodesTransaction", "Auto Layout Nodes"));
	TArray<UEdGraphNode*> MovedNodes;
	for (int32 Idx = 0; Idx < Selected.Num(); ++Idx)
	{
		UEdGraphNode* Node = Selected[Idx];
		const int32 NewX = FMath::RoundToInt(Layout.Positions[Idx].X);
		const int32 NewY = FMath::RoundToInt(Layout.Positions[Idx].Y);
		const FVector2f NewPos(static_cast<float>(NewX), static_cast<float>(NewY));
		Result.NodeBounds.Add(Node, FBox2f(NewPos, NewPos + Input.Nodes[Idx].Size));
		if (Node->NodePosX == NewX && Node->NodePosY == NewY)
		{
			continue;
		}
		Node->Modify();
		Node->NodePosX = NewX;
		Node->NodePosY = NewY;
		Adapter->PostMoveNode(*Node);
		MovedNodes.Add(Node);
	}

	// Keep Together: each group comment becomes its group's frame (which the layout kept clear of every other node).
	if (bKeepTogether)
	{
		Result.NumCommentGroups = Stats.NumGroups;
		Result.NumCommentsResized += FitGroupComments(*Adapter, CommentGroups, Layout, Result.ObstacleBounds, Settings->CommentPadding);
	}

	// Wrap each affected comment around its (possibly moved) contents again; the title bar needs extra room on top.
	for (const FCommentToFit& Fit : CommentsToFit)
	{
		FBox2f ContentBounds(ForceInit);
		for (const UEdGraphNode* Content : Fit.Contents)
		{
			const FBox2f* Bounds = Result.NodeBounds.Find(Content);
			ContentBounds += Bounds ? *Bounds : Result.ObstacleBounds.FindChecked(Content);
		}
		const float Padding = Settings->CommentPadding;
		const float TitleHeight = CommentTitleHeight(*Fit.Comment);
		const int32 NewX = FMath::RoundToInt(ContentBounds.Min.X - Padding);
		const int32 NewY = FMath::RoundToInt(ContentBounds.Min.Y - Padding - TitleHeight);
		const int32 NewWidth = FMath::RoundToInt(ContentBounds.Max.X + Padding) - NewX;
		const int32 NewHeight = FMath::RoundToInt(ContentBounds.Max.Y + Padding) - NewY;
		Result.NumCommentsResized += SetCommentRect(*Adapter, *Fit.Comment, NewX, NewY, NewWidth, NewHeight) ? 1 : 0;
	}

	Result.NumMoved = MovedNodes.Num();
	if (Result.NumMoved == 0 && Result.NumCommentsResized == 0)
	{
		Transaction.Cancel();
	}
	else
	{
		Adapter->PostLayout(*Graph, MovedNodes);
	}

	Result.bSucceeded = true;
	Result.NumLaidOut = Selected.Num();
	Result.Message = FText::Format(LOCTEXT("Done", "Laid out {0} nodes ({1} moved, {2} comments resized)."), Result.NumLaidOut, Result.NumMoved, Result.NumCommentsResized);
	return Result;
}

FGraphAutoLayoutRunResult FGraphAutoLayoutRunner::PlaceNewNode(UEdGraph* Graph, UEdGraphNode* NewNode, const SGraphPanel* Panel)
{
	using namespace UE::GraphAutoLayout;
	using namespace UE::GraphAutoLayout::RunnerPrivate;

	FGraphAutoLayoutRunResult Result;
	if (!Graph || !NewNode || NewNode->GetGraph() != Graph || IsCommentNode(*NewNode))
	{
		Result.Message = LOCTEXT("PlaceInvalid", "Nothing to place.");
		return Result;
	}
	const IGraphLayoutAdapter* Adapter = ResolveAdapter(*Graph, Panel, Result);
	if (!Adapter)
	{
		return Result;
	}

	const UGraphAutoLayoutSettings* Settings = GetDefault<UGraphAutoLayoutSettings>();
	const FGraphNodeMeasurer Measurer(Panel);
	const FMeasuredNode NewMeasured = Measurer.Measure(*NewNode);

	// Align to one wire: a main-flow (exec / parameter map) wire if there is one, else the first wire in pin order.
	const FMeasuredPin* OwnPin = nullptr;
	UEdGraphPin* OtherPin = nullptr;
	bool bOwnPinIsPrimary = false;
	for (const FMeasuredPin& MeasuredPin : NewMeasured.Pins)
	{
		for (UEdGraphPin* Linked : MeasuredPin.Pin->LinkedTo)
		{
			const UEdGraphNode* Other = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
			if (!Other || Other == NewNode || IsCommentNode(*Other))
			{
				continue;
			}
			const bool bPrimary = MeasuredPin.Pin->Direction == EGPD_Output
				? Adapter->IsPrimaryLink(*MeasuredPin.Pin, *Linked)
				: Adapter->IsPrimaryLink(*Linked, *MeasuredPin.Pin);
			if (!OwnPin || (bPrimary && !bOwnPinIsPrimary))
			{
				OwnPin = &MeasuredPin;
				OtherPin = Linked;
				bOwnPinIsPrimary = bPrimary;
			}
		}
	}
	if (!OwnPin)
	{
		Result.Message = LOCTEXT("PlaceNotConnected", "New node isn't wired to anything; left where it was dropped.");
		return Result;
	}

	UEdGraphNode* Other = OtherPin->GetOwningNode();
	const FMeasuredNode OtherMeasured = Measurer.Measure(*Other);
	float OtherPinY = OtherMeasured.Size.Y * 0.5f;
	for (const FMeasuredPin& MeasuredPin : OtherMeasured.Pins)
	{
		if (MeasuredPin.Pin == OtherPin)
		{
			OtherPinY = MeasuredPin.OffsetY;
		}
	}

	// Consumers go to the right of what feeds them, producers to the left, with the wire level.
	const FVector2f OtherPos = NodePos(*Other);
	const float X = OwnPin->Pin->Direction == EGPD_Input
		? OtherPos.X + OtherMeasured.Size.X + Settings->HorizontalSpacing
		: OtherPos.X - Settings->HorizontalSpacing - NewMeasured.Size.X;
	const float Y = OtherPos.Y + OtherPinY - OwnPin->OffsetY;
	const FBox2f Wanted(FVector2f(X, Y), FVector2f(X, Y) + NewMeasured.Size);

	TArray<FBox2f> Obstacles;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node != NewNode && !IsCommentNode(*Node))
		{
			const FVector2f Pos = NodePos(*Node);
			const FBox2f Bounds(Pos, Pos + Measurer.Measure(*Node).Size);
			Obstacles.Add(Bounds);
			Result.ObstacleBounds.Add(Node, Bounds);
		}
	}
	const float Shift = FGraphLayoutEngine::FindFreeVerticalShift(MakeArrayView(&Wanted, 1), Obstacles, Settings->HorizontalSpacing, Settings->VerticalSpacing);

	const int32 NewX = FMath::RoundToInt(Wanted.Min.X);
	const int32 NewY = FMath::RoundToInt(Wanted.Min.Y + Shift);
	const FVector2f NewPos(static_cast<float>(NewX), static_cast<float>(NewY));
	Result.NodeBounds.Add(NewNode, FBox2f(NewPos, NewPos + NewMeasured.Size));
	Result.bSucceeded = true;
	Result.NumLaidOut = 1;
	if (NewNode->NodePosX == NewX && NewNode->NodePosY == NewY)
	{
		return Result;
	}

	// Its own undo step, so one Ctrl+Z puts the node back where it was dropped.
	FScopedTransaction Transaction(LOCTEXT("PlaceNodeTransaction", "Auto Place Node"));
	NewNode->Modify();
	NewNode->NodePosX = NewX;
	NewNode->NodePosY = NewY;
	Adapter->PostMoveNode(*NewNode);
	Adapter->PostLayout(*Graph, { NewNode });
	Result.NumMoved = 1;
	GRAPHAUTOLAYOUT_DEBUG(TEXT("Placed %s next to %s (shift %.0f)"), *NewNode->GetName(), *Other->GetName(), Shift);
	return Result;
}

#undef LOCTEXT_NAMESPACE
