// Copyright 2026 Jot. All Rights Reserved.

#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphLayoutBlueprintTestUtils.h"
#include "GraphLayoutCommentTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutCommentTest, "GraphAutoLayout.Integration.CommentResize", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutCommentTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::ResizeToFit);

	UPackage* Package = nullptr;
	UEdGraph* Graph = MakeTestBlueprintGraph(TEXT("BP_CommentTest"), Package);
	if (!TestNotNull(TEXT("Event graph"), Graph))
	{
		return false;
	}

	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode(false);
	Event->CustomFunctionName = TEXT("CommentTestEvent");
	EventCreator.Finalize();
	const FName PrintString = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString);
	UK2Node_CallFunction* PrintA = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(600, 700));
	UK2Node_CallFunction* PrintB = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(1500, -400));
	Link(Event, UEdGraphSchema_K2::PN_Then, PrintA, UEdGraphSchema_K2::PN_Execute);
	Link(PrintA, UEdGraphSchema_K2::PN_Then, PrintB, UEdGraphSchema_K2::PN_Execute);

	// Wraps PrintA only (its centre is inside), and one far away containing nothing.
	UEdGraphNode_Comment* AroundA = AddComment(*Graph, FIntPoint(550, 620), FIntPoint(500, 400));
	UEdGraphNode_Comment* Elsewhere = AddComment(*Graph, FIntPoint(9000, 9000), FIntPoint(300, 200));
	const FCommentState AroundABefore = StateOf(*AroundA);
	const FCommentState ElsewhereBefore = StateOf(*Elsewhere);

	const TArray<UEdGraphNode*> Selection = { Event, PrintA, PrintB };
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("One comment resized"), Result.NumCommentsResized, 1);
	TestTrue(TEXT("Unrelated comment untouched"), StateOf(*Elsewhere) == ElsewhereBefore);

	const FBox2f Comment = CommentBox(*AroundA);
	const FBox2f Content = Result.NodeBounds.FindChecked(PrintA);
	const float Padding = Settings->CommentPadding;
	TestTrue(TEXT("Comment wraps its node on the left"), Comment.Min.X <= Content.Min.X - Padding + 1.f);
	TestTrue(TEXT("Comment wraps its node on the right"), Comment.Max.X >= Content.Max.X + Padding - 1.f);
	TestTrue(TEXT("Comment leaves room for its title"), Comment.Min.Y <= Content.Min.Y - Padding - 1.f);
	TestTrue(TEXT("Comment wraps its node at the bottom"), Comment.Max.Y >= Content.Max.Y + Padding - 1.f);
	TestFalse(TEXT("Comment doesn't swallow the event node"), Comment.IsInside(Result.NodeBounds.FindChecked(Event).GetCenter()));

	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestTrue(TEXT("Comment restored by undo"), StateOf(*AroundA) == AroundABefore);
		TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
	}

	// Ignore mode leaves comments alone even when their nodes move.
	{
		TSettingOverride<EGraphAutoLayoutCommentMode> Ignore(Settings->CommentMode, EGraphAutoLayoutCommentMode::Ignore);
		const FCommentState Before = StateOf(*AroundA);
		PrintA->NodePosY += 900;
		const FGraphAutoLayoutRunResult Ignored = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
		TestEqual(TEXT("Ignore mode: no comments resized"), Ignored.NumCommentsResized, 0);
		TestTrue(TEXT("Ignore mode: comment untouched"), StateOf(*AroundA) == Before);
	}

	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutAutoPlaceTest, "GraphAutoLayout.Integration.AutoPlace", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutAutoPlaceTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	const UGraphAutoLayoutSettings* Settings = GetDefault<UGraphAutoLayoutSettings>();
	UPackage* Package = nullptr;
	UEdGraph* Graph = MakeTestBlueprintGraph(TEXT("BP_AutoPlaceTest"), Package);
	if (!TestNotNull(TEXT("Event graph"), Graph))
	{
		return false;
	}

	const FName PrintString = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString);
	// New Blueprints get default "ghost" event nodes near the origin (Kismet2.cpp, AddDefaultEventNode), so work far away.
	const FIntPoint Origin(20000, 20000);
	UK2Node_CallFunction* Existing = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, Origin);

	// A consumer dropped far away, wired to the existing node's exec output.
	UK2Node_CallFunction* Consumer = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, Origin + FIntPoint(3000, 2000));
	Link(Existing, UEdGraphSchema_K2::PN_Then, Consumer, UEdGraphSchema_K2::PN_Execute);
	const FGraphAutoLayoutRunResult Placed = FGraphAutoLayoutRunner::PlaceNewNode(Graph, Consumer, nullptr);
	TestTrue(TEXT("Consumer placed"), Placed.bSucceeded && Placed.NumMoved == 1);
	const FBox2f ExistingBox = Placed.ObstacleBounds.FindChecked(Existing);
	TestEqual(TEXT("Consumer sits one gap to the right"), static_cast<float>(Consumer->NodePosX), ExistingBox.Max.X + Settings->HorizontalSpacing, 1.f);
	TestEqual(TEXT("Exec wire is level"), Consumer->NodePosY, Existing->NodePosY);
	TestEqual(TEXT("Existing node did not move"), FIntPoint(Existing->NodePosX, Existing->NodePosY), Origin);

	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("Undo puts the node back where it was dropped"), FIntPoint(Consumer->NodePosX, Consumer->NodePosY), Origin + FIntPoint(3000, 2000));
		TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
	}

	// A second consumer of the same output: the natural spot is taken, so it is nudged clear of everything.
	UK2Node_CallFunction* Second = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, Origin + FIntPoint(-2000, 5000));
	Link(Existing, UEdGraphSchema_K2::PN_Then, Second, UEdGraphSchema_K2::PN_Execute);
	const FGraphAutoLayoutRunResult Nudged = FGraphAutoLayoutRunner::PlaceNewNode(Graph, Second, nullptr);
	TestTrue(TEXT("Second consumer placed"), Nudged.bSucceeded);
	const FBox2f SecondBox = Nudged.NodeBounds.FindChecked(Second);
	int32 Overlaps = 0;
	for (const TPair<const UEdGraphNode*, FBox2f>& Obstacle : Nudged.ObstacleBounds)
	{
		Overlaps += (GapX(SecondBox, Obstacle.Value) < Settings->HorizontalSpacing - 1.f && GapY(SecondBox, Obstacle.Value) < Settings->VerticalSpacing - 1.f) ? 1 : 0;
	}
	TestEqual(TEXT("Nudged node is clear of all other nodes"), Overlaps, 0);
	TestEqual(TEXT("Nudged node stays in the consumer column"), Second->NodePosX, Consumer->NodePosX);

	// A producer goes to the left of what it feeds.
	UK2Node_CallFunction* Producer = AddCall(*Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString), Origin + FIntPoint(4000, -3000));
	FirstOutputPin(Producer)->MakeLinkTo(Existing->FindPinChecked(TEXT("InString"), EGPD_Input));
	const FGraphAutoLayoutRunResult Left = FGraphAutoLayoutRunner::PlaceNewNode(Graph, Producer, nullptr);
	TestTrue(TEXT("Producer placed"), Left.bSucceeded);
	TestEqual(TEXT("Producer sits one gap to the left"), Left.NodeBounds.FindChecked(Producer).Max.X + Settings->HorizontalSpacing, static_cast<float>(Existing->NodePosX), 1.f);

	// Not wired to anything: left alone.
	UK2Node_CallFunction* Loose = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, Origin + FIntPoint(7000, 7000));
	const FGraphAutoLayoutRunResult NotPlaced = FGraphAutoLayoutRunner::PlaceNewNode(Graph, Loose, nullptr);
	TestFalse(TEXT("Unwired node is not placed"), NotPlaced.bSucceeded);
	TestEqual(TEXT("Unwired node stays put"), FIntPoint(Loose->NodePosX, Loose->NodePosY), Origin + FIntPoint(7000, 7000));

	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
