// Copyright 2026 Jot. All Rights Reserved.

#include "EdGraphSchema_Niagara.h"
#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphLayoutCommentTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraTypes.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::GraphAutoLayout::Tests
{
	static bool IsMapPin(const UEdGraphPin* Pin)
	{
		return Pin->PinType.PinCategory == UEdGraphSchema_Niagara::PinCategoryType
			&& Pin->PinType.PinSubCategoryObject == FNiagaraTypeDefinition::GetParameterMapStruct();
	}

	static UEdGraphPin* FindMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && IsMapPin(Pin))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Map Get/Set node classes are private to NiagaraEditor, so they are created by class path. */
	static UEdGraphNode* AddNodeByClassPath(UEdGraph& Graph, const TCHAR* ClassPath, FIntPoint Pos)
	{
		UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		if (!Class)
		{
			return nullptr;
		}
		// UEdGraph::CreateNode is protected; FGraphNodeCreator is the public route and also allocates pins.
		FGraphNodeCreator<UNiagaraNode> Creator(Graph);
		UNiagaraNode* Node = Creator.CreateNode(false, Class);
		Node->NodePosX = Pos.X;
		Node->NodePosY = Pos.Y;
		Creator.Finalize();
		return Node;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutNiagaraTest, "GraphAutoLayout.Integration.NiagaraScratchPad", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutNiagaraTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// Headless: no SGraphPanel, so the Scratch Pad host check is skipped and sizes are estimated.
	// The graph is a minimal module graph: InputMap -> Map Set -> Map Set -> Output, plus a Map Get reading the map.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/NiagaraScratchTest"));
	UNiagaraGraph* Graph = NewObject<UNiagaraGraph>(Package, TEXT("ScratchGraph"), RF_Transactional);

	FGraphNodeCreator<UNiagaraNodeInput> InputCreator(*Graph);
	UNiagaraNodeInput* InputMap = InputCreator.CreateNode(false);
	InputMap->Input = FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("InputMap"));
	InputMap->Usage = ENiagaraInputNodeUsage::Parameter;
	InputMap->NodePosX = 600;
	InputMap->NodePosY = 400;
	InputCreator.Finalize();

	FGraphNodeCreator<UNiagaraNodeOutput> OutputCreator(*Graph);
	UNiagaraNodeOutput* Output = OutputCreator.CreateNode(false);
	Output->Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetParameterMapDef(), TEXT("Output")));
	Output->ScriptType = ENiagaraScriptUsage::Module;
	Output->NodePosX = 1000;
	Output->NodePosY = 0;
	OutputCreator.Finalize();

	UEdGraphNode* Set1 = AddNodeByClassPath(*Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"), FIntPoint(200, -300));
	UEdGraphNode* Set2 = AddNodeByClassPath(*Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapSet"), FIntPoint(-100, 250));
	UEdGraphNode* Get = AddNodeByClassPath(*Graph, TEXT("/Script/NiagaraEditor.NiagaraNodeParameterMapGet"), FIntPoint(900, 500));
	if (!TestNotNull(TEXT("Map Set class found"), Set1) || !TestNotNull(TEXT("Map Get class found"), Get))
	{
		return false;
	}

	UEdGraphPin* InputOut = FindMapPin(InputMap, EGPD_Output);
	UEdGraphPin* OutputIn = FindMapPin(Output, EGPD_Input);
	if (!TestNotNull(TEXT("InputMap map pin"), InputOut) || !TestNotNull(TEXT("Output map pin"), OutputIn)
		|| !TestNotNull(TEXT("Map Set pins"), FindMapPin(Set1, EGPD_Input)) || !TestNotNull(TEXT("Map Get pin"), FindMapPin(Get, EGPD_Input)))
	{
		return false;
	}
	InputOut->MakeLinkTo(FindMapPin(Set1, EGPD_Input));
	FindMapPin(Set1, EGPD_Output)->MakeLinkTo(FindMapPin(Set2, EGPD_Input));
	FindMapPin(Set2, EGPD_Output)->MakeLinkTo(OutputIn);
	InputOut->MakeLinkTo(FindMapPin(Get, EGPD_Input));

	const TArray<UEdGraphNode*> Selection = { InputMap, Output, Set1, Set2, Get };
	TMap<UEdGraphNode*, FIntPoint> Before;
	for (UEdGraphNode* Node : Selection)
	{
		Before.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
	}
	const FIntPoint OutputPos(Output->NodePosX, Output->NodePosY);
	const FGuid ChangeIdBefore = Graph->GetChangeID();

	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Adapter"), Result.AdapterName, FName(TEXT("NiagaraScratchPad")));
	TestEqual(TEXT("Output (anchor) kept its position"), FIntPoint(Output->NodePosX, Output->NodePosY), OutputPos);
	for (UEdGraphNode* Node : Selection)
	{
		if (Node != InputMap)
		{
			TestTrue(FString::Printf(TEXT("InputMap is left of %s"), *Node->GetName()), Result.NodeBounds.FindChecked(InputMap).Max.X < Node->NodePosX);
		}
	}
	// The parameter-map spine is straight. With estimated sizes every map pin is the first row on its side,
	// so straight spine wires mean the spine nodes share the same top.
	TestEqual(TEXT("Spine straight: InputMap -> Set1"), InputMap->NodePosY, Set1->NodePosY);
	TestEqual(TEXT("Spine straight: Set1 -> Set2"), Set1->NodePosY, Set2->NodePosY);
	TestEqual(TEXT("Spine straight: Set2 -> Output"), Set2->NodePosY, Output->NodePosY);
	TestTrue(TEXT("Graph marked changed so Scratch Pad keeps the layout"), Graph->GetChangeID() != ChangeIdBefore);

	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		for (UEdGraphNode* Node : Selection)
		{
			TestEqual(FString::Printf(TEXT("%s restored by undo"), *Node->GetName()), FIntPoint(Node->NodePosX, Node->NodePosY), Before.FindChecked(Node));
		}
		AddInfo(FString::Printf(TEXT("Change id restored by undo: %s"), Graph->GetChangeID() == ChangeIdBefore ? TEXT("yes") : TEXT("no")));
	}

	// With the setting off, a layout leaves the change id alone.
	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	const bool bOldMark = Settings->bMarkNiagaraScratchPadChanged;
	Settings->bMarkNiagaraScratchPadChanged = false;
	Set1->NodePosX += 777; // Make sure something moves.
	const FGuid ChangeIdUnmarked = Graph->GetChangeID();
	const FGraphAutoLayoutRunResult Unmarked = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Unmarked layout moved nodes"), Unmarked.NumMoved > 0);
	TestTrue(TEXT("Setting off: change id untouched"), Graph->GetChangeID() == ChangeIdUnmarked);
	Settings->bMarkNiagaraScratchPadChanged = bOldMark;

	// Keep Together: a comment around the two Map Set nodes keeps them together, and the layout makes room for the
	// comment's border and title so no other node ends up under it.
	{
		TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
		const FBox2f Sets = Unmarked.NodeBounds.FindChecked(Set1) + Unmarked.NodeBounds.FindChecked(Set2);
		UEdGraphNode_Comment* Comment = AddComment(*Graph, FIntPoint(FMath::FloorToInt(Sets.Min.X) - 10, FMath::FloorToInt(Sets.Min.Y) - 10),
			FIntPoint(FMath::CeilToInt(Sets.GetSize().X) + 20, FMath::CeilToInt(Sets.GetSize().Y) + 20));
		const FGraphAutoLayoutRunResult Grouped = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
		TestTrue(TEXT("Comment group: layout succeeded"), Grouped.bSucceeded);
		TestEqual(TEXT("Comment group: one group"), Grouped.NumCommentGroups, 1);
		const FBox2f Box = CommentBox(*Comment);
		for (UEdGraphNode* Member : { Set1, Set2 })
		{
			TestTrue(FString::Printf(TEXT("Comment group: wraps %s"), *Member->GetName()), WrapsNode(Box, Grouped.NodeBounds.FindChecked(Member), Settings->CommentPadding, CommentTitleHeight(*Comment)));
		}
		for (UEdGraphNode* Other : TArray<UEdGraphNode*>{ InputMap, Output, Get })
		{
			TestFalse(FString::Printf(TEXT("Comment group: keeps clear of %s"), *Other->GetName()), TooClose(Box, Grouped.NodeBounds.FindChecked(Other), Settings->HorizontalSpacing, Settings->VerticalSpacing));
		}
	}

	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
