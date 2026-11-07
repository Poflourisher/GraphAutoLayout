// Copyright 2026 Jot. All Rights Reserved.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_TwoWayBlend.h"
#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphLayoutBlueprintTestUtils.h"
#include "GraphLayoutCommentTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "Kismet/KismetMathLibrary.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::GraphAutoLayout::Tests
{
	static UEdGraphPin* FindPosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction, int32 Index = 0)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && UAnimationGraphSchema::IsPosePin(Pin->PinType) && Index-- == 0)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	template <typename T>
	static T* AddAnimNode(UEdGraph& Graph, FIntPoint Pos)
	{
		FGraphNodeCreator<T> Creator(Graph);
		T* Node = Creator.CreateNode(false);
		Node->NodePosX = Pos.X;
		Node->NodePosY = Pos.Y;
		Creator.Finalize();
		return Node;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutAnimGraphTest, "GraphAutoLayout.Integration.AnimGraph", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutAnimGraphTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// Created like UAnimBlueprintFactory does (AnimBlueprintFactory.cpp): no skeleton for the first compile, then a template.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/ABP_LayoutTest"));
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(FKismetEditorUtilities::CreateBlueprint(UAnimInstance::StaticClass(), Package, TEXT("ABP_LayoutTest"),
		BPTYPE_Normal, UAnimBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("Anim Blueprint created"), AnimBP))
	{
		return false;
	}
	AnimBP->bIsTemplate = true;

	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (Graph && Graph->GetSchema() && Graph->GetSchema()->IsA<UAnimationGraphSchema>())
		{
			AnimGraph = Graph;
		}
	}
	if (!TestNotNull(TEXT("AnimGraph found"), AnimGraph))
	{
		return false;
	}
	UEdGraphNode* Root = nullptr;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		Root = Node && Node->IsA<UAnimGraphNode_Root>() ? Node : Root;
	}
	if (!TestNotNull(TEXT("Output Pose node"), Root))
	{
		return false;
	}

	// Two players blended into the output, with a math node driving the blend alpha.
	auto* PlayerA = AddAnimNode<UAnimGraphNode_SequencePlayer>(*AnimGraph, FIntPoint(300, 400));
	auto* PlayerB = AddAnimNode<UAnimGraphNode_SequencePlayer>(*AnimGraph, FIntPoint(-500, -300));
	auto* Blend = AddAnimNode<UAnimGraphNode_TwoWayBlend>(*AnimGraph, FIntPoint(-200, 600));
	UK2Node_CallFunction* AlphaMath = AddCall(*AnimGraph, UKismetMathLibrary::StaticClass(), GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Abs), FIntPoint(500, -500));

	UEdGraphPin* BlendA = FindPosePin(Blend, EGPD_Input, 0);
	UEdGraphPin* BlendB = FindPosePin(Blend, EGPD_Input, 1);
	UEdGraphPin* BlendAlpha = Blend->FindPin(TEXT("Alpha"), EGPD_Input);
	if (!TestNotNull(TEXT("Blend pose A"), BlendA) || !TestNotNull(TEXT("Blend pose B"), BlendB) || !TestNotNull(TEXT("Blend alpha"), BlendAlpha)
		|| !TestNotNull(TEXT("Player pose out"), FindPosePin(PlayerA, EGPD_Output)) || !TestNotNull(TEXT("Root pose in"), FindPosePin(Root, EGPD_Input)))
	{
		return false;
	}
	FindPosePin(PlayerA, EGPD_Output)->MakeLinkTo(BlendA);
	FindPosePin(PlayerB, EGPD_Output)->MakeLinkTo(BlendB);
	FindPosePin(Blend, EGPD_Output)->MakeLinkTo(FindPosePin(Root, EGPD_Input));
	FirstOutputPin(AlphaMath)->MakeLinkTo(BlendAlpha);

	const TArray<UEdGraphNode*> Selection = { Root, PlayerA, PlayerB, Blend, AlphaMath };
	const FIntPoint RootPos(Root->NodePosX, Root->NodePosY);
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(AnimGraph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Adapter"), Result.AdapterName, FName(TEXT("AnimGraph")));
	TestEqual(TEXT("Output Pose (anchor) kept its position"), FIntPoint(Root->NodePosX, Root->NodePosY), RootPos);

	// Every wire flows left to right into its consumer.
	int32 Backward = 0;
	for (UEdGraphNode* Node : Selection)
	{
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				Backward += Result.NodeBounds.FindChecked(Node).Max.X < Result.NodeBounds.FindChecked(Linked->GetOwningNode()).Min.X ? 0 : 1;
			}
		}
	}
	TestEqual(TEXT("All wires flow left to right"), Backward, 0);

	// Pose spine is straight into the output: with estimated sizes both pose pins are the first row on their side.
	TestEqual(TEXT("Blend level with Output Pose"), Blend->NodePosY, Root->NodePosY);
	TestTrue(TEXT("Alpha math hangs below the blend's pose inputs"), AlphaMath->NodePosY > Blend->NodePosY);
	TestTrue(TEXT("Alpha math left of the blend"), Result.NodeBounds.FindChecked(AlphaMath).Max.X < Blend->NodePosX);

	TArray<FBox2f> Boxes;
	Result.NodeBounds.GenerateValueArray(Boxes);
	int32 Overlaps = 0;
	for (int32 A = 0; A < Boxes.Num(); ++A)
	{
		for (int32 B = A + 1; B < Boxes.Num(); ++B)
		{
			Overlaps += (GapX(Boxes[A], Boxes[B]) < 1.f && GapY(Boxes[A], Boxes[B]) < 1.f) ? 1 : 0;
		}
	}
	TestEqual(TEXT("No overlapping nodes"), Overlaps, 0);

	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("Blend restored by undo"), FIntPoint(Blend->NodePosX, Blend->NodePosY), FIntPoint(-200, 600));

		// Keep Together: back to the laid-out graph, then a comment around the two players. They stay together and every
		// other node stays clear of the comment.
		TestTrue(TEXT("Comment group: redo"), GEditor->RedoTransaction());
		UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
		TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
		const FBox2f Players = Result.NodeBounds.FindChecked(PlayerA) + Result.NodeBounds.FindChecked(PlayerB);
		UEdGraphNode_Comment* Comment = AddComment(*AnimGraph, FIntPoint(FMath::FloorToInt(Players.Min.X) - 10, FMath::FloorToInt(Players.Min.Y) - 10),
			FIntPoint(FMath::CeilToInt(Players.GetSize().X) + 20, FMath::CeilToInt(Players.GetSize().Y) + 20));
		const TArray<UEdGraphNode*> Others = { Root, Blend, AlphaMath };
		for (UEdGraphNode* Other : Others)
		{
			TestFalse(FString::Printf(TEXT("Comment group setup: comment starts without %s"), *Other->GetName()), CommentBox(*Comment).IsInside(Result.NodeBounds.FindChecked(Other).GetCenter()));
		}
		const FGraphAutoLayoutRunResult Grouped = FGraphAutoLayoutRunner::LayoutNodes(AnimGraph, Selection, nullptr);
		TestTrue(TEXT("Comment group: layout succeeded"), Grouped.bSucceeded);
		TestEqual(TEXT("Comment group: one group"), Grouped.NumCommentGroups, 1);
		const FBox2f Box = CommentBox(*Comment);
		TestTrue(TEXT("Comment group: wraps player A"), WrapsNode(Box, Grouped.NodeBounds.FindChecked(PlayerA), Settings->CommentPadding, CommentTitleHeight(*Comment)));
		TestTrue(TEXT("Comment group: wraps player B"), WrapsNode(Box, Grouped.NodeBounds.FindChecked(PlayerB), Settings->CommentPadding, CommentTitleHeight(*Comment)));
		// The alpha math may hang off the blend as a pure-node tree, which uses the tighter pure-node spacing.
		const float MinH = FMath::Min(Settings->HorizontalSpacing, Settings->PureNodeHorizontalSpacing);
		const float MinV = FMath::Min(Settings->VerticalSpacing, Settings->PureNodeVerticalSpacing);
		for (UEdGraphNode* Other : Others)
		{
			TestFalse(FString::Printf(TEXT("Comment group: keeps clear of %s"), *Other->GetName()), TooClose(Box, Grouped.NodeBounds.FindChecked(Other), MinH, MinV));
		}
		TestEqual(TEXT("Comment group: no overlapping nodes"), CountNodeOverlaps(Grouped, MinH, MinV), 0);
	}

	// The Anim Blueprint's event graph is an ordinary K2 graph and still uses the Blueprint adapter.
	if (AnimBP->UbergraphPages.Num() > 0 && AnimBP->UbergraphPages[0]->Nodes.Num() > 0)
	{
		const FGraphAutoLayoutRunResult EventGraph = FGraphAutoLayoutRunner::LayoutNodes(AnimBP->UbergraphPages[0], { AnimBP->UbergraphPages[0]->Nodes[0] }, nullptr);
		TestEqual(TEXT("Event graph uses the Blueprint adapter"), EventGraph.AdapterName, FName(TEXT("Blueprint")));
	}

	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
