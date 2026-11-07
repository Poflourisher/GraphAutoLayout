// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace UE::GraphAutoLayout::Tests
{
	/** Transient actor Blueprint in its own /Temp package; returns its event graph. */
	inline UEdGraph* MakeTestBlueprintGraph(const TCHAR* Name, UPackage*& OutPackage)
	{
		OutPackage = CreatePackage(*FString::Printf(TEXT("/Temp/GraphAutoLayoutTests/%s"), Name));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(), OutPackage, Name,
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		return Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
	}

	inline UK2Node_CallFunction* AddCall(UEdGraph& Graph, UClass* Library, FName Function, FIntPoint Pos)
	{
		FGraphNodeCreator<UK2Node_CallFunction> Creator(Graph);
		UK2Node_CallFunction* Node = Creator.CreateNode(false);
		Node->SetFromFunction(Library->FindFunctionByName(Function));
		Node->NodePosX = Pos.X;
		Node->NodePosY = Pos.Y;
		Creator.Finalize();
		return Node;
	}

	inline void Link(UEdGraphNode* From, FName FromPin, UEdGraphNode* To, FName ToPin)
	{
		UEdGraphPin* Out = From->FindPinChecked(FromPin, EGPD_Output);
		UEdGraphPin* In = To->FindPinChecked(ToPin, EGPD_Input);
		Out->MakeLinkTo(In);
	}

	inline UEdGraphPin* FirstOutputPin(UEdGraphNode* Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}
}
