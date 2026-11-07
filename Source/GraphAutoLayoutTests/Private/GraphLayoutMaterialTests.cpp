// Copyright 2026 Jot. All Rights Reserved.

#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "Editor.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphLayoutCommentTestUtils.h"
#include "GraphLayoutTestUtils.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "GraphAutoLayoutSettings.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialFunction.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::GraphAutoLayout::Tests
{
	/** Builds the editor-side graph the same way FMaterialEditor does (MaterialEditor.cpp, InitEditorForMaterial). */
	static UMaterialGraph* BuildMaterialGraph(UMaterial* Material, UMaterialFunction* Function, int32& DirtyCounter)
	{
		UMaterialGraph* Graph = CastChecked<UMaterialGraph>(FBlueprintEditorUtils::CreateNewGraph(Material, NAME_None, UMaterialGraph::StaticClass(), UMaterialGraphSchema::StaticClass()));
		Material->MaterialGraph = Graph;
		Graph->Material = Material;
		Graph->MaterialFunction = Function;
		// Graph nodes copy this delegate when created, so bind before rebuilding.
		Graph->MaterialDirtyDelegate.BindLambda([&DirtyCounter]() { ++DirtyCounter; });
		Graph->RebuildGraph();
		return Graph;
	}

	template <typename T>
	static T* AddExpression(UMaterial* Material, int32 X, int32 Y)
	{
		return CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	static TArray<UEdGraphNode*> AllNonCommentNodes(UEdGraph* Graph)
	{
		TArray<UEdGraphNode*> Nodes;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && !Node->IsA<UEdGraphNode_Comment>())
			{
				Nodes.Add(Node);
			}
		}
		return Nodes;
	}

	/** Every link between laid-out nodes goes from the producer's right edge to a consumer further right. */
	static int32 CountBackwardLinks(const FGraphAutoLayoutRunResult& Result)
	{
		int32 Backward = 0;
		for (const TPair<const UEdGraphNode*, FBox2f>& Pair : Result.NodeBounds)
		{
			for (const UEdGraphPin* Pin : Pair.Key->Pins)
			{
				if (Pin->Direction != EGPD_Output)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (const FBox2f* Dst = Result.NodeBounds.Find(Linked->GetOwningNode()))
					{
						Backward += Pair.Value.Max.X < Dst->Min.X ? 0 : 1;
					}
				}
			}
		}
		return Backward;
	}

	static int32 CountOverlappingNodes(const FGraphAutoLayoutRunResult& Result)
	{
		TArray<FBox2f> Boxes;
		Result.NodeBounds.GenerateValueArray(Boxes);
		int32 Count = 0;
		for (int32 A = 0; A < Boxes.Num(); ++A)
		{
			for (int32 B = A + 1; B < Boxes.Num(); ++B)
			{
				Count += (GapX(Boxes[A], Boxes[B]) < 1.f && GapY(Boxes[A], Boxes[B]) < 1.f) ? 1 : 0;
			}
		}
		return Count;
	}

	/** Expression positions must match their graph nodes, or the layout is lost on save/reload. */
	static int32 CountUnsyncedExpressions(UEdGraph* Graph)
	{
		int32 Unsynced = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(Node))
			{
				const UMaterialExpression* Expression = MaterialNode->MaterialExpression;
				Unsynced += (Expression && (Expression->MaterialExpressionEditorX != Node->NodePosX || Expression->MaterialExpressionEditorY != Node->NodePosY)) ? 1 : 0;
			}
		}
		return Unsynced;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMaterialTest, "GraphAutoLayout.Integration.Material", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutMaterialTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// Headless: no SGraphPanel, so node sizes and pin offsets come from the runner's estimate.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/M_LayoutTest"));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_LayoutTest"), RF_Public | RF_Standalone | RF_Transactional);
	Material->EditorX = 600;
	Material->EditorY = 0;

	// Messy positions, a shared scalar feeding two multiplies, M1 feeding two consumers, several root inputs.
	auto* TexCoord = AddExpression<UMaterialExpressionTextureCoordinate>(Material, -100, 400);
	auto* ColorA = AddExpression<UMaterialExpressionConstant3Vector>(Material, 50, -200);
	auto* ColorB = AddExpression<UMaterialExpressionConstant3Vector>(Material, -300, 100);
	auto* Scalar = AddExpression<UMaterialExpressionScalarParameter>(Material, 200, 300);
	auto* AlphaConst = AddExpression<UMaterialExpressionConstant>(Material, 700, 500);
	auto* Mul1 = AddExpression<UMaterialExpressionMultiply>(Material, 400, 200);
	auto* Mul2 = AddExpression<UMaterialExpressionMultiply>(Material, -50, -50);
	auto* Lerp = AddExpression<UMaterialExpressionLinearInterpolate>(Material, 300, -300);
	auto* AddNode = AddExpression<UMaterialExpressionAdd>(Material, 100, 100);
	auto* Rough = AddExpression<UMaterialExpressionConstant>(Material, 800, 300);

	Mul1->A.Connect(0, ColorA);
	Mul1->B.Connect(0, Scalar);
	Mul2->A.Connect(0, ColorB);
	Mul2->B.Connect(0, Scalar);
	Lerp->A.Connect(0, Mul1);
	Lerp->B.Connect(0, Mul2);
	Lerp->Alpha.Connect(0, AlphaConst);
	AddNode->A.Connect(0, Lerp);
	AddNode->B.Connect(0, Mul1);
	UMaterialEditingLibrary::ConnectMaterialProperty(AddNode, TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(TexCoord, TEXT(""), MP_Normal);

	int32 DirtyCount = 0;
	UMaterialGraph* Graph = BuildMaterialGraph(Material, nullptr, DirtyCount);
	if (!TestNotNull(TEXT("Root node"), Graph->RootNode.Get()))
	{
		return false;
	}
	UMaterialGraphNode_Root* Root = Graph->RootNode;
	const FIntPoint RootPos(Root->NodePosX, Root->NodePosY);
	TestEqual(TEXT("Root starts at Material EditorX/Y"), RootPos, FIntPoint(600, 0));

	const TArray<UEdGraphNode*> Selection = AllNonCommentNodes(Graph);
	TMap<UEdGraphNode*, FIntPoint> Before;
	for (UEdGraphNode* Node : Selection)
	{
		Before.Add(Node, FIntPoint(Node->NodePosX, Node->NodePosY));
	}

	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Adapter"), Result.AdapterName, FName(TEXT("Material")));
	TestEqual(TEXT("Every node laid out"), Result.NumLaidOut, Selection.Num());
	TestTrue(TEXT("Something moved"), Result.NumMoved > 0);

	TestEqual(TEXT("Root (anchor) kept its position"), FIntPoint(Root->NodePosX, Root->NodePosY), RootPos);
	TestEqual(TEXT("Material EditorX/Y match root"), FIntPoint(Material->EditorX, Material->EditorY), FIntPoint(Root->NodePosX, Root->NodePosY));
	TestEqual(TEXT("Expression positions synced to graph nodes"), CountUnsyncedExpressions(Graph), 0);
	TestEqual(TEXT("Every link flows into its consumer (left to right)"), CountBackwardLinks(Result), 0);
	TestEqual(TEXT("No overlapping nodes"), CountOverlappingNodes(Result), 0);
	for (UEdGraphNode* Node : Selection)
	{
		if (Node != Root)
		{
			TestTrue(FString::Printf(TEXT("%s is left of the root"), *Node->GetName()), Result.NodeBounds.FindChecked(Node).Max.X < Root->NodePosX);
		}
	}
	TestTrue(TEXT("Material editor was told the material is dirty"), DirtyCount > 0);

	// Undo restores graph nodes and expressions; redo re-applies both.
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
		TestEqual(TEXT("Expressions restored by undo"), CountUnsyncedExpressions(Graph), 0);
		TestTrue(TEXT("Redo"), GEditor->RedoTransaction());
		for (UEdGraphNode* Node : Selection)
		{
			TestEqual(FString::Printf(TEXT("%s re-applied by redo"), *Node->GetName()), FIntPoint(Node->NodePosX, Node->NodePosY), After.FindChecked(Node));
		}
		TestEqual(TEXT("Expressions re-applied by redo"), CountUnsyncedExpressions(Graph), 0);
	}

	// Partial selection without the root: the right-most selected sink anchors, unselected nodes don't move.
	const TArray<UEdGraphNode*> Partial = { Mul1->GraphNode.Get(), Mul2->GraphNode.Get(), ColorA->GraphNode.Get(), ColorB->GraphNode.Get(), Scalar->GraphNode.Get() };
	const FIntPoint LerpPos(Lerp->GraphNode->NodePosX, Lerp->GraphNode->NodePosY);
	const FGraphAutoLayoutRunResult PartialResult = FGraphAutoLayoutRunner::LayoutNodes(Graph, Partial, nullptr);
	TestTrue(TEXT("Partial layout succeeded"), PartialResult.bSucceeded);
	TestEqual(TEXT("Unselected Lerp untouched"), FIntPoint(Lerp->GraphNode->NodePosX, Lerp->GraphNode->NodePosY), LerpPos);
	TestEqual(TEXT("Partial: links flow right"), CountBackwardLinks(PartialResult), 0);
	int32 ObstacleHits = 0;
	for (const TPair<const UEdGraphNode*, FBox2f>& Laid : PartialResult.NodeBounds)
	{
		for (const TPair<const UEdGraphNode*, FBox2f>& Obstacle : PartialResult.ObstacleBounds)
		{
			ObstacleHits += (GapX(Laid.Value, Obstacle.Value) < 1.f && GapY(Laid.Value, Obstacle.Value) < 1.f) ? 1 : 0;
		}
	}
	TestEqual(TEXT("Partial: no overlap with unselected nodes"), ObstacleHits, 0);
	TestEqual(TEXT("Partial: expressions synced"), CountUnsyncedExpressions(Graph), 0);

	Material->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMaterialCommentTest, "GraphAutoLayout.Integration.MaterialComment", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutMaterialCommentTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// A material comment is mirrored by a UMaterialExpressionComment, which is what gets saved.
	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	const EGraphAutoLayoutCommentMode OldMode = Settings->CommentMode;
	Settings->CommentMode = EGraphAutoLayoutCommentMode::ResizeToFit;

	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/M_CommentTest"));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_CommentTest"), RF_Public | RF_Standalone | RF_Transactional);
	auto* ColorA = AddExpression<UMaterialExpressionConstant3Vector>(Material, -900, 600);
	auto* ColorB = AddExpression<UMaterialExpressionConstant3Vector>(Material, -200, -500);
	auto* Mul = AddExpression<UMaterialExpressionMultiply>(Material, -100, 900);
	Mul->A.Connect(0, ColorA);
	Mul->B.Connect(0, ColorB);
	UMaterialEditingLibrary::ConnectMaterialProperty(Mul, TEXT(""), MP_BaseColor);

	UMaterialExpressionComment* CommentExpr = NewObject<UMaterialExpressionComment>(Material, NAME_None, RF_Transactional);
	CommentExpr->MaterialExpressionEditorX = -150;
	CommentExpr->MaterialExpressionEditorY = 820;
	CommentExpr->SizeX = 400;
	CommentExpr->SizeY = 300;
	CommentExpr->Text = TEXT("Around the multiply");
	Material->GetExpressionCollection().AddComment(CommentExpr);

	int32 DirtyCount = 0;
	UMaterialGraph* Graph = BuildMaterialGraph(Material, nullptr, DirtyCount);
	const TArray<UEdGraphNode*> Selection = AllNonCommentNodes(Graph);
	const int32 OldSizeX = CommentExpr->SizeX;
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Comment resized"), Result.NumCommentsResized, 1);

	UEdGraphNode* CommentNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		CommentNode = Node && Node->IsA<UEdGraphNode_Comment>() ? Node : CommentNode;
	}
	if (TestNotNull(TEXT("Comment graph node"), CommentNode))
	{
		TestEqual(TEXT("Comment expression X synced"), CommentExpr->MaterialExpressionEditorX, CommentNode->NodePosX);
		TestEqual(TEXT("Comment expression Y synced"), CommentExpr->MaterialExpressionEditorY, CommentNode->NodePosY);
		TestEqual(TEXT("Comment expression width synced"), CommentExpr->SizeX, CommentNode->NodeWidth);
		TestEqual(TEXT("Comment expression height synced"), CommentExpr->SizeY, CommentNode->NodeHeight);
		const FBox2f MulBox = Result.NodeBounds.FindChecked(Mul->GraphNode.Get());
		const FVector2f CommentMin(static_cast<float>(CommentNode->NodePosX), static_cast<float>(CommentNode->NodePosY));
		const FBox2f CommentRect(CommentMin, CommentMin + FVector2f(static_cast<float>(CommentNode->NodeWidth), static_cast<float>(CommentNode->NodeHeight)));
		TestTrue(TEXT("Comment wraps the multiply"), CommentRect.IsInside(MulBox.Min) && CommentRect.IsInside(MulBox.Max));
	}
	TestTrue(TEXT("Material editor told it is dirty"), DirtyCount > 0);

	if (TestNotNull(TEXT("GEditor"), GEditor))
	{
		TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
		TestEqual(TEXT("Comment expression size restored by undo"), CommentExpr->SizeX, OldSizeX);
	}

	Settings->CommentMode = OldMode;
	Material->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMaterialFunctionTest, "GraphAutoLayout.Integration.MaterialFunction", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutMaterialFunctionTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	// The function editor wraps the function's expressions in a material and sets MaterialGraph->MaterialFunction,
	// which suppresses the root node (MaterialGraph.cpp, RebuildGraphInternal). FunctionOutput nodes are the sinks.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/MF_LayoutTest"));
	UMaterialFunction* Function = NewObject<UMaterialFunction>(Package, TEXT("MF_LayoutTest"), RF_Public | RF_Standalone | RF_Transactional);
	UMaterial* Wrapper = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional);

	auto* Input = AddExpression<UMaterialExpressionFunctionInput>(Wrapper, 300, 300);
	auto* Const = AddExpression<UMaterialExpressionConstant>(Wrapper, 500, -100);
	auto* Mul = AddExpression<UMaterialExpressionMultiply>(Wrapper, -200, 0);
	auto* OutTop = AddExpression<UMaterialExpressionFunctionOutput>(Wrapper, 400, -200);
	auto* OutBottom = AddExpression<UMaterialExpressionFunctionOutput>(Wrapper, 0, 400);
	Mul->A.Connect(0, Input);
	Mul->B.Connect(0, Const);
	OutTop->A.Connect(0, Mul);
	OutBottom->A.Connect(0, Const);

	int32 DirtyCount = 0;
	UMaterialGraph* Graph = BuildMaterialGraph(Wrapper, Function, DirtyCount);
	TestNull(TEXT("No root node in function graphs"), Graph->RootNode.Get());

	const TArray<UEdGraphNode*> Selection = AllNonCommentNodes(Graph);
	const FIntPoint OutTopPos(OutTop->GraphNode->NodePosX, OutTop->GraphNode->NodePosY);
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Top function output (anchor) kept its position"), FIntPoint(OutTop->GraphNode->NodePosX, OutTop->GraphNode->NodePosY), OutTopPos);
	TestEqual(TEXT("Every link flows into its consumer"), CountBackwardLinks(Result), 0);
	TestEqual(TEXT("No overlapping nodes"), CountOverlappingNodes(Result), 0);
	TestEqual(TEXT("Expression positions synced"), CountUnsyncedExpressions(Graph), 0);
	TestEqual(TEXT("Both outputs in the right-most column"), OutTop->GraphNode->NodePosX + Result.NodeBounds.FindChecked(OutTop->GraphNode.Get()).GetSize().X,
		OutBottom->GraphNode->NodePosX + Result.NodeBounds.FindChecked(OutBottom->GraphNode.Get()).GetSize().X, 1.f);
	TestTrue(TEXT("Material editor was told the function is dirty"), DirtyCount > 0);

	Function->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphLayoutMaterialCommentGroupTest, "GraphAutoLayout.Integration.CommentGroups.Material", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphLayoutMaterialCommentGroupTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout::Tests;

	UGraphAutoLayoutSettings* Settings = GetMutableDefault<UGraphAutoLayoutSettings>();
	if (!TestNotNull(TEXT("GEditor"), GEditor))
	{
		return false;
	}

	// Lerp inputs top to bottom: A <- ColorA, B <- ColorB, Alpha <- AlphaConst. The comment holds ColorA and the alpha only.
	UPackage* Package = CreatePackage(TEXT("/Temp/GraphAutoLayoutTests/M_CommentGroupTest"));
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_CommentGroupTest"), RF_Public | RF_Standalone | RF_Transactional);
	auto* Lerp = AddExpression<UMaterialExpressionLinearInterpolate>(Material, -300, 0);
	auto* ColorA = AddExpression<UMaterialExpressionConstant3Vector>(Material, -800, -400);
	auto* AlphaConst = AddExpression<UMaterialExpressionConstant>(Material, -800, -150);
	auto* ColorB = AddExpression<UMaterialExpressionConstant3Vector>(Material, -800, 500);
	Lerp->A.Connect(0, ColorA);
	Lerp->B.Connect(0, ColorB);
	Lerp->Alpha.Connect(0, AlphaConst);
	UMaterialEditingLibrary::ConnectMaterialProperty(Lerp, TEXT(""), MP_BaseColor);

	UMaterialExpressionComment* CommentExpr = NewObject<UMaterialExpressionComment>(Material, NAME_None, RF_Transactional);
	CommentExpr->MaterialExpressionEditorX = -850;
	CommentExpr->MaterialExpressionEditorY = -480;
	CommentExpr->SizeX = 500;
	CommentExpr->SizeY = 600;
	CommentExpr->Text = TEXT("Color A and alpha");
	Material->GetExpressionCollection().AddComment(CommentExpr);

	int32 DirtyCount = 0;
	UMaterialGraph* Graph = BuildMaterialGraph(Material, nullptr, DirtyCount);
	UEdGraphNode_Comment* Comment = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Comment = Cast<UEdGraphNode_Comment>(Node) ? Cast<UEdGraphNode_Comment>(Node) : Comment;
	}
	if (!TestNotNull(TEXT("Comment graph node"), Comment))
	{
		return false;
	}
	const TArray<UEdGraphNode*> Selection = AllNonCommentNodes(Graph);
	const UEdGraphNode* NodeA = ColorA->GraphNode.Get();
	const UEdGraphNode* NodeB = ColorB->GraphNode.Get();
	const UEdGraphNode* NodeAlpha = AlphaConst->GraphNode.Get();
	const UEdGraphNode* NodeLerp = Lerp->GraphNode.Get();
	const int32 OldSizeX = CommentExpr->SizeX;
	const int32 OldSizeY = CommentExpr->SizeY;
	const float H = Settings->HorizontalSpacing;
	const float V = Settings->VerticalSpacing;

	// Control: Resize To Fit stacks the three inputs in one column, so the refitted comment covers color B.
	{
		TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::ResizeToFit);
		const FGraphAutoLayoutRunResult Plain = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
		TestTrue(TEXT("Control: Resize To Fit covers color B"), CommentBox(*Comment).IsInside(Plain.NodeBounds.FindChecked(NodeB).GetCenter()));
		TestTrue(TEXT("Control: undo"), GEditor->UndoTransaction());
	}

	TSettingOverride<EGraphAutoLayoutCommentMode> Mode(Settings->CommentMode, EGraphAutoLayoutCommentMode::KeepTogether);
	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selection, nullptr);
	TestTrue(TEXT("Layout succeeded"), Result.bSucceeded);
	TestEqual(TEXT("One comment group"), Result.NumCommentGroups, 1);
	const FBox2f Box = CommentBox(*Comment);
	TestTrue(TEXT("Comment wraps color A"), WrapsNode(Box, Result.NodeBounds.FindChecked(NodeA), Settings->CommentPadding, CommentTitleHeight(*Comment)));
	TestTrue(TEXT("Comment wraps the alpha"), WrapsNode(Box, Result.NodeBounds.FindChecked(NodeAlpha), Settings->CommentPadding, CommentTitleHeight(*Comment)));
	TestFalse(TEXT("Comment keeps clear of color B"), TooClose(Box, Result.NodeBounds.FindChecked(NodeB), H, V));
	TestFalse(TEXT("Comment keeps clear of the lerp"), TooClose(Box, Result.NodeBounds.FindChecked(NodeLerp), H, V));
	TestEqual(TEXT("No overlapping nodes"), CountNodeOverlaps(Result, H, V), 0);
	TestEqual(TEXT("Expression positions synced"), CountUnsyncedExpressions(Graph), 0);
	TestEqual(TEXT("Comment expression X synced"), CommentExpr->MaterialExpressionEditorX, Comment->NodePosX);
	TestEqual(TEXT("Comment expression Y synced"), CommentExpr->MaterialExpressionEditorY, Comment->NodePosY);
	TestEqual(TEXT("Comment expression width synced"), CommentExpr->SizeX, Comment->NodeWidth);
	TestEqual(TEXT("Comment expression height synced"), CommentExpr->SizeY, Comment->NodeHeight);

	TestTrue(TEXT("Undo"), GEditor->UndoTransaction());
	TestEqual(TEXT("Comment expression width restored by undo"), CommentExpr->SizeX, OldSizeX);
	TestEqual(TEXT("Comment expression height restored by undo"), CommentExpr->SizeY, OldSizeY);

	Material->ClearFlags(RF_Standalone);
	Package->MarkAsGarbage();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
