// Copyright 2026 Jot. All Rights Reserved.

#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphLayoutBlueprintTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetStringLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutBlueprintTest, "GraphAutoLayout.Integration.Blueprint", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutBlueprintTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// Headless: no SGraphPanel, so node sizes and pin offsets come from the runner's estimate.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/BP_LayoutTest"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), Package, TEXT("BP_LayoutTest"),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("Blueprint created"), Blueprint))
	{
		return false;
	}
	UEdGraph* Graph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
	if (!TestNotNull(TEXT("Event graph found"), Graph))
	{
		return false;
	}

	FGraphNodeCreator<UK2Node_CustomEvent> EventCreator(*Graph);
	UK2Node_CustomEvent* Event = EventCreator.CreateNode(false);
	Event->CustomFunctionName = TEXT("LayoutTestEvent");
	Event->NodePosX = 2000;
	Event->NodePosY = 1000;
	EventCreator.Finalize();

	const FName PrintString = GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString);
	UK2Node_CallFunction* Print1 = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(1900, 1300));
	UK2Node_CallFunction* Print2 = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(2050, 900));
	UK2Node_CallFunction* Print3 = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(1800, 1100));

	FGraphNodeCreator<UK2Node_IfThenElse> BranchCreator(*Graph);
	UK2Node_IfThenElse* Branch = BranchCreator.CreateNode(false);
	Branch->NodePosX = 1950;
	Branch->NodePosY = 1050;
	BranchCreator.Finalize();

	// Pure nodes: Conv_IntToString(Add_IntInt) -> Print1.InString, Conv_IntToString -> Print2.InString.
	UK2Node_CallFunction* Add = AddCall(*Graph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_IntInt), FIntPoint(2100, 1200));
	UK2Node_CallFunction* ToString1 = AddCall(*Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString), FIntPoint(1850, 950));
	UK2Node_CallFunction* ToString2 = AddCall(*Graph, UKismetStringLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetStringLibrary, Conv_IntToString), FIntPoint(2000, 1250));

	Link(Event, UEdGraphSchema_K2::PN_Then, Print1, UEdGraphSchema_K2::PN_Execute);
	Link(Print1, UEdGraphSchema_K2::PN_Then, Branch, UEdGraphSchema_K2::PN_Execute);
	Link(Branch, UEdGraphSchema_K2::PN_Then, Print2, UEdGraphSchema_K2::PN_Execute);
	Link(Branch, UEdGraphSchema_K2::PN_Else, Print3, UEdGraphSchema_K2::PN_Execute);
	FirstOutputPin(Add)->MakeLinkTo(ToString1->FindPinChecked(TEXT("InInt"), EGPD_Input));
	FirstOutputPin(ToString1)->MakeLinkTo(Print1->FindPinChecked(TEXT("InString"), EGPD_Input));
	FirstOutputPin(ToString2)->MakeLinkTo(Print2->FindPinChecked(TEXT("InString"), EGPD_Input));

	// An unselected node. First out of the way (to the left of the anchor), later moved into the layout's path.
	UK2Node_CallFunction* Bystander = AddCall(*Graph, UKismetSystemLibrary::StaticClass(), PrintString, FIntPoint(800, 1000));

	const TArray<UEdGraphNode*> Selection = { Event, Print1, Print2, Print3, Branch, Add, ToString1, ToString2 };
	const FIntPoint EventPos(Event->NodePosX, Event->NodePosY);
	const FIntPoint BystanderPos(Bystander->NodePosX, Bystander->NodePosY);
	TMap<UEdGraphNode*, FIntPoint> Before;
	for (UEdGraphNode* Node : Selection)
	{
		Before.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
	}

	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Adapter"), Result.AdapterName, FName(TEXT("Blueprint")));
	TestEqual(TEXT("All selected nodes laid out"), Result.NumLaidOut, Selection.Num());
	TestTrue(TEXT("Something moved"), Result.NumMoved > 0);

	auto Right = [&](UEdGraphNode* Node) { return Result.NodeBounds.FindChecked(Node).Max.X; };
	TestEqual(TEXT("Event (anchor) kept its position"), FIntPoint(Event->NodePosX, Event->NodePosY), EventPos);
	TestEqual(TEXT("Unselected node untouched"), FIntPoint(Bystander->NodePosX, Bystander->NodePosY), BystanderPos);
	TestTrue(TEXT("Event -> Print1 flows right"), Right(Event) < Print1->NodePosX);
	TestTrue(TEXT("Print1 -> Branch flows right"), Right(Print1) < Branch->NodePosX);
	TestTrue(TEXT("Branch -> Print2 flows right"), Right(Branch) < Print2->NodePosX);
	TestTrue(TEXT("Branch -> Print3 flows right"), Right(Branch) < Print3->NodePosX);
	TestEqual(TEXT("Main exec line is straight (Event/Print1/Branch)"), Print1->NodePosY, Branch->NodePosY);
	TestTrue(TEXT("Pure Add left of its consumer"), Right(Add) < ToString1->NodePosX);
	TestTrue(TEXT("Pure ToString1 left of Print1"), Right(ToString1) < Print1->NodePosX);
	TestTrue(TEXT("Pure ToString2 left of Print2"), Right(ToString2) < Print2->NodePosX);
	TestTrue(TEXT("Pure nodes below the exec line"), ToString1->NodePosY > Print1->NodePosY && ToString2->NodePosY > Print2->NodePosY);

	// No overlaps between laid-out nodes (pure trees use the tighter pure spacing) or with unselected nodes.
	auto CountOverlapsInResult = [](const FGraphAutoLayoutRunResult& InResult)
	{
		TArray<FBox2f> Boxes;
		InResult.NodeBounds.GenerateValueArray(Boxes);
		int32 Count = 0;
		for (int32 A = 0; A < Boxes.Num(); ++A)
		{
			for (int32 B = A + 1; B < Boxes.Num(); ++B)
			{
				Count += (GapX(Boxes[A], Boxes[B]) < 1.f && GapY(Boxes[A], Boxes[B]) < 1.f) ? 1 : 0;
			}
			for (const TPair<const UEdGraphNode*, FBox2f>& Obstacle : InResult.ObstacleBounds)
			{
				Count += (GapX(Boxes[A], Obstacle.Value) < 1.f && GapY(Boxes[A], Obstacle.Value) < 1.f) ? 1 : 0;
			}
		}
		return Count;
	};
	TestEqual(TEXT("No overlapping nodes"), CountOverlapsInResult(Result), 0);

	// Undo restores every position, redo re-applies them.
	TMap<UEdGraphNode*, FIntPoint> After;
	for (UEdGraphNode* Node : Selection)
	{
		After.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
	}
	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		for (UEdGraphNode* Node : Selection)
		{
			TestEqual(FString::Printf(TEXT("%s restored by undo"), *Node->GetName()), FIntPoint(Node->NodePosX, Node->NodePosY), Before.FindChecked(Node));
		}
		TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
		for (UEdGraphNode* Node : Selection)
		{
			TestEqual(FString::Printf(TEXT("%s re-applied by redo"), *Node->GetName()), FIntPoint(Node->NodePosX, Node->NodePosY), After.FindChecked(Node));
		}
	}

	// Put the unselected node exactly where Print2 ended up; a re-layout must route around it.
	Bystander->NodePosX = Print2->NodePosX;
	Bystander->NodePosY = Print2->NodePosY;
	const FIntPoint BlockingPos(Bystander->NodePosX, Bystander->NodePosY);
	const FGraphAutoLayoutRunResult Blocked = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Second layout succeeded"), Blocked.bSucceeded);
	TestEqual(TEXT("Unselected node still untouched"), FIntPoint(Bystander->NodePosX, Bystander->NodePosY), BlockingPos);
	TestEqual(TEXT("No overlaps with the unselected node"), CountOverlapsInResult(Blocked), 0);

	// Nothing selected but comments/foreign nodes -> no-op.
	const FGraphAutoLayoutRunResult Empty = FGraphAutoLayoutRunner::LayoutNodes(Graph, {}, nullptr);
	TestFalse(TEXT("Empty selection does nothing"), Empty.bSucceeded);

	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
