// Copyright 2026 Jot. All Rights Reserved.

#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphLayoutBlueprintTestUtils.h"
#include "GraphLayoutCommentTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_ExecutionSequence.h"
#include "Kismet/KismetSystemLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

// Keep Together mode in Blueprint graphs. New Blueprints get default "ghost" event nodes near the origin
// (Kismet2.cpp, AddDefaultEventNode), so every scene here is built far away from it.

namespace UE::GraphAutoLayout::Tests
{
	static UK2Node_CustomEvent* AddGroupTestEvent(UEdGraph& Graph, const TCHAR* Name, FIntPoint Pos)
	{
		FGraphNodeCreator<UK2Node_CustomEvent> Creator(Graph);
		UK2Node_CustomEvent* Event = Creator.CreateNode(false);
		Event->CustomFunctionName = Name;
		Event->NodePosX = Pos.X;
		Event->NodePosY = Pos.Y;
		Creator.Finalize();
		return Event;
	}

	static UK2Node_CallFunction* AddPrint(UEdGraph& Graph, FIntPoint Pos)
	{
		return AddCall(Graph, UKismetSystemLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), Pos);
	}

	static void LinkExec(UEdGraphNode* From, UEdGraphNode* To)
	{
		Link(From, UEdGraphSchema_K2::PN_Then, To, UEdGraphSchema_K2::PN_Execute);
	}

	/** Sequence node with NumOutputs exec outputs (it is created with two). */
	static UK2Node_ExecutionSequence* AddSequence(UEdGraph& Graph, FIntPoint Pos, int32 NumOutputs)
	{
		FGraphNodeCreator<UK2Node_ExecutionSequence> Creator(Graph);
		UK2Node_ExecutionSequence* Sequence = Creator.CreateNode(false);
		Sequence->NodePosX = Pos.X;
		Sequence->NodePosY = Pos.Y;
		Creator.Finalize();
		// Both exported from BlueprintGraph (K2Node_ExecutionSequence.h); AddInputPin itself is not.
		for (int32 Count = 2; Count < NumOutputs; ++Count)
		{
			Sequence->InsertPinIntoExecutionNode(Sequence->GetThenPinGivenIndex(Count - 1), EPinInsertPosition::After);
		}
		return Sequence;
	}

	static FBox2f BoundsOf(const FGraphAutoLayoutRunResult& Result, const UEdGraphNode* Node)
	{
		if (const FBox2f* Bounds = Result.NodeBounds.Find(Node))
		{
			return *Bounds;
		}
		const FBox2f* Obstacle = Result.ObstacleBounds.Find(Node);
		return Obstacle ? *Obstacle : FBox2f(ForceInit);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCommentGroupBlueprintTest, "GraphAutoLayout.Integration.CommentGroups.Blueprint", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutCommentGroupBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	UPackage* Package = nullptr;
	UEdGraph* Graph = MakeTestBlueprintGraph(TEXT("BP_CommentGroupTest"), Package);
	if (!TestNotNull(TEXT("Event graph"), Graph) || !TestNotNull(TEXT("GEditor"), GEditor))
	{
		return false;
	}

	// A sequence fans out to X, Y and Z (top to bottom). The comment holds X and Z but not Y, the middle output.
	const FIntPoint Origin(20000, 20000);
	UK2Node_CustomEvent* Event = AddGroupTestEvent(*Graph, TEXT("CommentGroupEvent"), Origin);
	UK2Node_ExecutionSequence* Sequence = AddSequence(*Graph, Origin + FIntPoint(400, 0), 3);
	UK2Node_CallFunction* X = AddPrint(*Graph, Origin + FIntPoint(1000, -700));
	UK2Node_CallFunction* Z = AddPrint(*Graph, Origin + FIntPoint(1000, -300));
	UK2Node_CallFunction* Y = AddPrint(*Graph, Origin + FIntPoint(1000, 1200));
	LinkExec(Event, Sequence);
	UEdGraphNode* Outputs[] = { X, Y, Z };
	for (int32 Idx = 0; Idx < 3; ++Idx)
	{
		Sequence->GetThenPinGivenIndex(Idx)->MakeLinkTo(Outputs[Idx]->FindPinChecked(UEdGraphSchema_K2::PN_Execute, EGPD_Input));
	}
	UEdGraphNode_Comment* Comment = AddComment(*Graph, Origin + FIntPoint(950, -800), FIntPoint(700, 1100));

	const TArray<UEdGraphNode*> Selection = { Event, Sequence, X, Y, Z };
	const TMap<UEdGraphNode*, FIntPoint> Before = PositionsOf(Selection);
	const FCommentState CommentBefore = StateOf(*Comment);
	const float H = Settings->HorizontalSpacing;
	const float V = Settings->VerticalSpacing;

	// Control: Resize To Fit stacks the three outputs in one column, so the refitted comment covers Y.
	{
		TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::ResizeToFit);
		const FGraphAutoLayoutRunResult Plain = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
		TestTrue(TEXT("Control: Resize To Fit covers the middle output"), CommentBox(*Comment).IsInside(BoundsOf(Plain, Y).GetCenter()));
		TestTrue(TEXT("Control: undo"), GEditor->UndoTransaction());
	}

	TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("One comment group"), Result.NumCommentGroups, 1);
	TestEqual(TEXT("Comment resized"), Result.NumCommentsResized, 1);
	const FBox2f Box = CommentBox(*Comment);
	TestTrue(TEXT("Comment wraps X"), WrapsNode(Box, BoundsOf(Result, X), Settings->CommentPadding, CommentTitleHeight(*Comment)));
	TestTrue(TEXT("Comment wraps Z"), WrapsNode(Box, BoundsOf(Result, Z), Settings->CommentPadding, CommentTitleHeight(*Comment)));
	TestFalse(TEXT("Comment keeps clear of Y"), TooClose(Box, BoundsOf(Result, Y), H, V));
	TestFalse(TEXT("Comment keeps clear of the sequence"), TooClose(Box, BoundsOf(Result, Sequence), H, V));
	TestFalse(TEXT("Comment keeps clear of the event"), TooClose(Box, BoundsOf(Result, Event), H, V));
	TestEqual(TEXT("No overlapping nodes"), CountNodeOverlaps(Result, H, V), 0);

	// Pressing L again on the tidy result changes nothing (and so adds no undo step).
	const TMap<UEdGraphNode*, FIntPoint> After = PositionsOf(Selection);
	const FCommentState CommentAfter = StateOf(*Comment);
	const FGraphAutoLayoutRunResult Again = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestEqual(TEXT("Second layout moves nothing"), Again.NumMoved, 0);
	TestEqual(TEXT("Second layout leaves the comment alone"), Again.NumCommentsResized, 0);

	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	TestTrue(TEXT("Undo restores every node"), SamePositions(PositionsOf(Selection), Before));
	TestTrue(TEXT("Undo restores the comment"), StateOf(*Comment) == CommentBefore);
	TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
	TestTrue(TEXT("Redo moves the nodes again"), SamePositions(PositionsOf(Selection), After));
	TestTrue(TEXT("Redo refits the comment"), StateOf(*Comment) == CommentAfter);

	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCommentGroupPartialTest, "GraphAutoLayout.Integration.CommentGroups.PartialAndSelected", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutCommentGroupPartialTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
	UPackage* Package = nullptr;
	UEdGraph* Graph = MakeTestBlueprintGraph(TEXT("BP_CommentPartialTest"), Package);
	if (!TestNotNull(TEXT("Event graph"), Graph) || !TestNotNull(TEXT("GEditor"), GEditor))
	{
		return false;
	}
	const float Padding = Settings->CommentPadding;
	const float H = Settings->HorizontalSpacing;

	// 1. Partly selected comment: the selected node moves as a group, the unselected one stays, the comment wraps both.
	{
		const FIntPoint Origin(20000, 30000);
		UK2Node_CustomEvent* Event = AddGroupTestEvent(*Graph, TEXT("PartialEvent"), Origin);
		UK2Node_CallFunction* X = AddPrint(*Graph, Origin + FIntPoint(900, 200));
		UK2Node_CallFunction* Y = AddPrint(*Graph, Origin + FIntPoint(1800, -200));
		UK2Node_CallFunction* Unselected = AddPrint(*Graph, Origin + FIntPoint(900, 800));
		LinkExec(Event, X);
		LinkExec(X, Y);
		UEdGraphNode_Comment* Comment = AddComment(*Graph, Origin + FIntPoint(850, 100), FIntPoint(600, 1200));
		const FIntPoint UnselectedBefore(Unselected->NodePosX, Unselected->NodePosY);

		const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Event, X, Y }, nullptr);
		TestTrue(TEXT("Partial: layout succeeded"), Result.bSucceeded);
		TestEqual(TEXT("Partial: one comment group"), Result.NumCommentGroups, 1);
		TestEqual(TEXT("Partial: unselected node stays put"), FIntPoint(Unselected->NodePosX, Unselected->NodePosY), UnselectedBefore);
		TestTrue(TEXT("Partial: comment wraps the laid-out node"), WrapsNode(CommentBox(*Comment), BoundsOf(Result, X), Padding, CommentTitleHeight(*Comment)));
		TestTrue(TEXT("Partial: comment still wraps the unselected node"), WrapsNode(CommentBox(*Comment), BoundsOf(Result, Unselected), Padding, CommentTitleHeight(*Comment)));
	}

	// 2. Only the comment is selected: everything inside it is laid out, as if it had been dragged. Undo restores all.
	{
		const FIntPoint Origin(20000, 40000);
		UK2Node_CallFunction* First = AddPrint(*Graph, Origin + FIntPoint(600, 400));
		UK2Node_CallFunction* Second = AddPrint(*Graph, Origin);
		LinkExec(First, Second);
		UEdGraphNode_Comment* Comment = AddComment(*Graph, Origin - FIntPoint(100, 100), FIntPoint(1300, 1000));
		const TArray<UEdGraphNode*> Contents = { First, Second };
		const TMap<UEdGraphNode*, FIntPoint> Before = PositionsOf(Contents);
		const FCommentState CommentBefore = StateOf(*Comment);

		const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Comment }, nullptr);
		TestTrue(TEXT("Selected comment: layout succeeded"), Result.bSucceeded);
		TestEqual(TEXT("Selected comment: its two nodes were laid out"), Result.NumLaidOut, 2);
		TestTrue(TEXT("Selected comment: second node runs right of the first"), Second->NodePosX >= BoundsOf(Result, First).Max.X + H - 1.f);
		TestEqual(TEXT("Selected comment: exec wire is level"), Second->NodePosY, First->NodePosY);
		TestTrue(TEXT("Selected comment: wraps the first node"), WrapsNode(CommentBox(*Comment), BoundsOf(Result, First), Padding, CommentTitleHeight(*Comment)));
		TestTrue(TEXT("Selected comment: wraps the second node"), WrapsNode(CommentBox(*Comment), BoundsOf(Result, Second), Padding, CommentTitleHeight(*Comment)));
		TestTrue(TEXT("Selected comment: undo"), GEditor->UndoTransaction());
		TestTrue(TEXT("Selected comment: undo restores the pulled-in nodes"), SamePositions(PositionsOf(Contents), Before));
		TestTrue(TEXT("Selected comment: undo restores the comment"), StateOf(*Comment) == CommentBefore);
	}

	// 3. A plain "Comment"-mode box doesn't own its nodes, so selecting it alone lays out nothing.
	{
		const FIntPoint Origin(20000, 50000);
		UK2Node_CallFunction* Inside = AddPrint(*Graph, Origin + FIntPoint(100, 100));
		UEdGraphNode_Comment* Comment = AddComment(*Graph, Origin, FIntPoint(600, 600));
		Comment->MoveMode = ECommentBoxMode::NoGroupMovement;
		const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Comment }, nullptr);
		TestFalse(TEXT("Plain comment: nothing to lay out"), Result.bSucceeded);
		TestEqual(TEXT("Plain comment: its node is untouched"), FIntPoint(Inside->NodePosX, Inside->NodePosY), Origin + FIntPoint(100, 100));
	}

	// 4. An outer comment holding only an unselected node and an inner comment with the selected node: the outer one
	//    still wraps both its unselected node and the inner comment.
	{
		const FIntPoint Origin(20000, 60000);
		UK2Node_CustomEvent* Event = AddGroupTestEvent(*Graph, TEXT("NestedPartialEvent"), Origin);
		UK2Node_CallFunction* Selected = AddPrint(*Graph, Origin + FIntPoint(900, 0));
		UK2Node_CallFunction* Unselected = AddPrint(*Graph, Origin + FIntPoint(900, 1000));
		LinkExec(Event, Selected);
		UEdGraphNode_Comment* Outer = AddComment(*Graph, Origin + FIntPoint(800, -200), FIntPoint(800, 1700));
		UEdGraphNode_Comment* Inner = AddComment(*Graph, Origin + FIntPoint(850, -100), FIntPoint(500, 500));

		const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Event, Selected }, nullptr);
		TestTrue(TEXT("Nested partial: layout succeeded"), Result.bSucceeded);
		TestEqual(TEXT("Nested partial: two comment groups"), Result.NumCommentGroups, 2);
		TestTrue(TEXT("Nested partial: inner comment wraps the selected node"), WrapsNode(CommentBox(*Inner), BoundsOf(Result, Selected), Padding, CommentTitleHeight(*Inner)));
		TestTrue(TEXT("Nested partial: outer comment wraps the unselected node"), WrapsNode(CommentBox(*Outer), BoundsOf(Result, Unselected), Padding, CommentTitleHeight(*Outer)));
		TestTrue(TEXT("Nested partial: outer comment wraps the inner comment"), ContainsBox(CommentBox(*Outer), CommentBox(*Inner)));
	}

	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCommentGroupNestedTest, "GraphAutoLayout.Integration.CommentGroups.Nested", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutCommentGroupNestedTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
	UPackage* Package = nullptr;
	UEdGraph* Graph = MakeTestBlueprintGraph(TEXT("BP_CommentNestedTest"), Package);
	if (!TestNotNull(TEXT("Event graph"), Graph))
	{
		return false;
	}
	const float Padding = Settings->CommentPadding;
	const float H = Settings->HorizontalSpacing;
	const float V = Settings->VerticalSpacing;

	// E -> A -> B -> C; the outer comment holds A, B and C, the inner one holds B.
	const FIntPoint Origin(20000, 70000);
	UK2Node_CustomEvent* Event = AddGroupTestEvent(*Graph, TEXT("NestedEvent"), Origin);
	UK2Node_CallFunction* A = AddPrint(*Graph, Origin + FIntPoint(700, 300));
	UK2Node_CallFunction* B = AddPrint(*Graph, Origin + FIntPoint(1400, -300));
	UK2Node_CallFunction* C = AddPrint(*Graph, Origin + FIntPoint(2100, 300));
	LinkExec(Event, A);
	LinkExec(A, B);
	LinkExec(B, C);
	UEdGraphNode_Comment* Outer = AddComment(*Graph, Origin + FIntPoint(650, -400), FIntPoint(2000, 1200));
	UEdGraphNode_Comment* Inner = AddComment(*Graph, Origin + FIntPoint(1350, -350), FIntPoint(500, 450));

	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Event, A, B, C }, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Two comment groups"), Result.NumCommentGroups, 2);
	TestTrue(TEXT("Inner comment wraps B"), WrapsNode(CommentBox(*Inner), BoundsOf(Result, B), Padding, CommentTitleHeight(*Inner)));
	TestFalse(TEXT("Inner comment keeps clear of A"), TooClose(CommentBox(*Inner), BoundsOf(Result, A), H, V));
	TestFalse(TEXT("Inner comment keeps clear of C"), TooClose(CommentBox(*Inner), BoundsOf(Result, C), H, V));
	TestTrue(TEXT("Outer comment wraps A"), WrapsNode(CommentBox(*Outer), BoundsOf(Result, A), Padding, CommentTitleHeight(*Outer)));
	TestTrue(TEXT("Outer comment wraps C"), WrapsNode(CommentBox(*Outer), BoundsOf(Result, C), Padding, CommentTitleHeight(*Outer)));
	TestTrue(TEXT("Outer comment wraps the inner comment"), ContainsBox(CommentBox(*Outer), CommentBox(*Inner)));
	TestFalse(TEXT("Outer comment keeps clear of the event"), TooClose(CommentBox(*Outer), BoundsOf(Result, Event), H, V));
	TestEqual(TEXT("No overlapping nodes"), CountNodeOverlaps(Result, H, V), 0);

	// Two identical boxes around one node: the later one counts as nested in the earlier one, and both wrap the node.
	const FIntPoint Twin(20000, 76000);
	UK2Node_CallFunction* Lone = AddPrint(*Graph, Twin + FIntPoint(100, 100));
	UEdGraphNode_Comment* First = AddComment(*Graph, Twin, FIntPoint(500, 500));
	UEdGraphNode_Comment* Second = AddComment(*Graph, Twin, FIntPoint(500, 500));
	const FGraphAutoLayoutRunResult Twins = FGraphAutoLayoutRunner::LayoutNodes(Graph, { Lone }, nullptr);
	TestEqual(TEXT("Identical boxes: two comment groups"), Twins.NumCommentGroups, 2);
	TestTrue(TEXT("Identical boxes: the later box wraps the node"), WrapsNode(CommentBox(*Second), BoundsOf(Twins, Lone), Padding, CommentTitleHeight(*Second)));
	TestTrue(TEXT("Identical boxes: the earlier box wraps the later one"), ContainsBox(CommentBox(*First), CommentBox(*Second)));
	TestFalse(TEXT("Identical boxes: they no longer coincide"), StateOf(*First) == StateOf(*Second));

	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
