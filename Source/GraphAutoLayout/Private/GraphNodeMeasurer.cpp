// Copyright 2026 Jot. All Rights Reserved.

#include "GraphNodeMeasurer.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "SGraphNode.h"
#include "SGraphPanel.h"
#include "SGraphPin.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace UE::GraphAutoLayout
{
	namespace MeasurePrivate
	{
		struct FFullDetailMeasure
		{
			FVector2f Size = FVector2f::ZeroVector;
			/** Keyed by pin id: pin objects can be recreated when a node reconstructs. */
			TMap<FGuid, float> PinOffsets;
		};

		/** Last full-detail measurement per node, so zoomed-out layouts use zoomed-in sizes. Editor-session only. */
		static TMap<TWeakObjectPtr<const UEdGraphNode>, FFullDetailMeasure>& GetCache()
		{
			static TMap<TWeakObjectPtr<const UEdGraphNode>, FFullDetailMeasure> Cache;
			return Cache;
		}

		static void PruneCache()
		{
			TMap<TWeakObjectPtr<const UEdGraphNode>, FFullDetailMeasure>& Cache = GetCache();
			if (Cache.Num() > 8192)
			{
				for (auto It = Cache.CreateIterator(); It; ++It)
				{
					if (!It.Key().IsValid())
					{
						It.RemoveCurrent();
					}
				}
			}
		}
	}

	FGraphNodeMeasurer::FGraphNodeMeasurer(const SGraphPanel* InPanel)
		: Panel(InPanel)
	{
		// LowDetail and below hide pin names and (LowestDetail) replace titles; MediumDetail and up are full size.
		bFullDetail = Panel && Panel->GetCurrentLOD() > EGraphRenderingLOD::LowDetail;
	}

	bool FGraphNodeMeasurer::IsPinVisible(const UEdGraphNode& Node, const UEdGraphPin& Pin)
	{
		if (Pin.bHidden)
		{
			return false;
		}
		return !(Pin.bAdvancedView && Node.AdvancedPinDisplay == ENodeAdvancedPins::Hidden);
	}

	FMeasuredNode FGraphNodeMeasurer::Measure(UEdGraphNode& Node) const
	{
		using namespace MeasurePrivate;

		FMeasuredNode Out;
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && IsPinVisible(Node, *Pin))
			{
				Out.Pins.Add({ Pin, 0.f });
			}
		}

		FMeasuredNode Estimated = Out;
		Estimate(Node, Estimated);

		const TSharedPtr<SGraphNode> Widget = Panel ? Panel->GetNodeWidgetFromGuid(Node.NodeGuid) : nullptr;
		FVector2f WidgetSize = FVector2f::ZeroVector;
		bool bHasWidgetSize = false;
		if (Widget.IsValid())
		{
			// Desired size is what SNodePanel itself uses for node bounds (SNodePanel::GetBoundsForNode).
			WidgetSize = Widget->GetDesiredSize();
			bHasWidgetSize = WidgetSize.X > 1.f && WidgetSize.Y > 1.f;
		}

		TMap<TWeakObjectPtr<const UEdGraphNode>, FFullDetailMeasure>& Cache = GetCache();
		const FFullDetailMeasure* Cached = Cache.Find(&Node);

		const FResolvedNodeSize Resolved = ResolveNodeSize(bFullDetail, bHasWidgetSize ? &WidgetSize : nullptr, Cached ? &Cached->Size : nullptr, Estimated.Size);
		Out.Size = Resolved.Size;
		Out.SizeSource = Resolved.Source;

		// Pins: fresh full-detail widget geometry, else the remembered full-detail offsets, else estimates.
		if (Resolved.Source == EMeasureSource::Widget && MeasureWidgetPins(*Widget, Out.Size, Out))
		{
			Out.PinSource = EMeasureSource::Widget;
		}
		else
		{
			bool bAllCached = Cached != nullptr;
			for (int32 Idx = 0; bAllCached && Idx < Out.Pins.Num(); ++Idx)
			{
				const float* Offset = Cached->PinOffsets.Find(Out.Pins[Idx].Pin->PinId);
				bAllCached = Offset && *Offset <= Out.Size.Y;
				Out.Pins[Idx].OffsetY = Offset ? *Offset : 0.f;
			}
			if (bAllCached)
			{
				Out.PinSource = EMeasureSource::Cache;
			}
			else
			{
				for (int32 Idx = 0; Idx < Out.Pins.Num(); ++Idx)
				{
					Out.Pins[Idx].OffsetY = FMath::Min(Estimated.Pins[Idx].OffsetY, Out.Size.Y - PinRowHeight * 0.5f);
				}
				Out.PinSource = EMeasureSource::Estimate;
			}
		}

		if (Resolved.Source == EMeasureSource::Widget)
		{
			PruneCache();
			FFullDetailMeasure& Entry = Cache.FindOrAdd(&Node);
			Entry.Size = Out.Size;
			if (Out.PinSource == EMeasureSource::Widget)
			{
				Entry.PinOffsets.Reset();
				for (const FMeasuredPin& MeasuredPin : Out.Pins)
				{
					Entry.PinOffsets.Add(MeasuredPin.Pin->PinId, MeasuredPin.OffsetY);
				}
			}
		}
		return Out;
	}

	bool FGraphNodeMeasurer::MeasureWidgetPins(const SGraphNode& Widget, const FVector2f& Size, FMeasuredNode& Out) const
	{
		// Pin offsets come from the last ticked geometry. If the node hasn't been ticked at its current
		// size (e.g. culled off-screen), the geometry is stale and the caller falls back.
		const FGeometry& NodeGeometry = Widget.GetTickSpaceGeometry();
		const FVector2f LocalSize = NodeGeometry.GetLocalSize();
		const bool bFresh = NodeGeometry.Scale > KINDA_SMALL_NUMBER
			&& FMath::IsNearlyEqual(LocalSize.X, Size.X, 2.f)
			&& FMath::IsNearlyEqual(LocalSize.Y, Size.Y, 2.f);
		if (!bFresh)
		{
			return false;
		}

		const float NodeTop = FVector2f(NodeGeometry.GetAbsolutePosition()).Y;
		TArray<float> Offsets;
		Offsets.Reserve(Out.Pins.Num());
		for (const FMeasuredPin& MeasuredPin : Out.Pins)
		{
			const TSharedPtr<SGraphPin> PinWidget = Widget.FindWidgetForPin(MeasuredPin.Pin);
			if (!PinWidget.IsValid())
			{
				return false;
			}
			const FGeometry& PinGeometry = PinWidget->GetTickSpaceGeometry();
			const float PinCentre = FVector2f(PinGeometry.GetAbsolutePosition()).Y + FVector2f(PinGeometry.GetAbsoluteSize()).Y * 0.5f;
			const float Offset = (PinCentre - NodeTop) / NodeGeometry.Scale;
			if (Offset < 0.f || Offset > Size.Y)
			{
				return false;
			}
			Offsets.Add(Offset);
		}
		for (int32 Idx = 0; Idx < Offsets.Num(); ++Idx)
		{
			Out.Pins[Idx].OffsetY = Offsets[Idx];
		}
		return true;
	}

	void FGraphNodeMeasurer::Estimate(UEdGraphNode& Node, FMeasuredNode& Out)
	{
		// Reroute/knot style nodes are drawn as a small control point.
		int32 InputIdx = INDEX_NONE;
		int32 OutputIdx = INDEX_NONE;
		if (Node.ShouldDrawNodeAsControlPointOnly(InputIdx, OutputIdx))
		{
			Out.Size = FVector2f(42.f, 24.f);
			for (FMeasuredPin& MeasuredPin : Out.Pins)
			{
				MeasuredPin.OffsetY = Out.Size.Y * 0.5f;
			}
			return;
		}

		// Inputs and outputs are stacked in separate columns under the title bar.
		int32 NumInputs = 0;
		int32 NumOutputs = 0;
		int32 LongestInput = 0;
		int32 LongestOutput = 0;
		for (FMeasuredPin& MeasuredPin : Out.Pins)
		{
			const bool bInput = MeasuredPin.Pin->Direction == EGPD_Input;
			int32& Row = bInput ? NumInputs : NumOutputs;
			MeasuredPin.OffsetY = HeaderHeight + Row * PinRowHeight + PinRowHeight * 0.5f;
			++Row;
			const int32 NameLength = Node.GetPinDisplayName(MeasuredPin.Pin).ToString().Len();
			int32& Longest = bInput ? LongestInput : LongestOutput;
			Longest = FMath::Max(Longest, NameLength);
		}

		FString Title = Node.GetNodeTitle(ENodeTitleType::FullTitle).ToString();
		int32 NewLine = INDEX_NONE;
		if (Title.FindChar(TEXT('\n'), NewLine))
		{
			Title.LeftInline(NewLine);
		}
		constexpr float CharWidth = 7.f;
		const float TitleWidth = Title.Len() * CharWidth + 60.f;
		const float PinsWidth = (LongestInput + LongestOutput) * CharWidth + 80.f;
		const float Width = FMath::Max3(120.f, TitleWidth, PinsWidth);
		const float Height = HeaderHeight + FMath::Max(1, FMath::Max(NumInputs, NumOutputs)) * PinRowHeight + 12.f;
		Out.Size = FVector2f(Width, Height);
	}
}
