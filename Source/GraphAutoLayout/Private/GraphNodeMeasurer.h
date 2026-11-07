// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GraphNodeSizeResolver.h"

class SGraphNode;
class SGraphPanel;
class UEdGraphNode;
class UEdGraphPin;

namespace UE::GraphAutoLayout
{
	struct FMeasuredPin
	{
		UEdGraphPin* Pin = nullptr;

		/** Distance from the node's top edge to the pin's centre line. */
		float OffsetY = 0.f;
	};

	struct FMeasuredNode
	{
		FVector2f Size = FVector2f::ZeroVector;

		/** Visible pins only, in UEdGraphNode::Pins order. */
		TArray<FMeasuredPin> Pins;

		EMeasureSource SizeSource = EMeasureSource::Estimate;
		EMeasureSource PinSource = EMeasureSource::Estimate;
	};

	/**
	 * Measures nodes at their full-detail size, whatever the current zoom.
	 *
	 * With a graph panel at full detail, sizes and pin offsets come from the live widgets and are remembered
	 * per node. Zoomed out, widgets are simplified and too small, so the remembered full-detail measurement
	 * (or an estimate) is used instead; see ResolveNodeSize. Without a panel (headless tests) everything is estimated.
	 */
	class FGraphNodeMeasurer
	{
	public:
		explicit FGraphNodeMeasurer(const SGraphPanel* InPanel);

		FMeasuredNode Measure(UEdGraphNode& Node) const;

		/** Whether widget sizes can be trusted at the panel's current zoom. */
		bool IsFullDetail() const { return bFullDetail; }

		static bool IsPinVisible(const UEdGraphNode& Node, const UEdGraphPin& Pin);

		/** Typical node chrome, used by the estimate. */
		static constexpr float HeaderHeight = 32.f;
		static constexpr float PinRowHeight = 24.f;

	private:
		/** Fills Out.Size and every pin offset with estimates. */
		static void Estimate(UEdGraphNode& Node, FMeasuredNode& Out);

		/** Reads pin offsets from fresh widget geometry; false if any pin can't be measured. */
		bool MeasureWidgetPins(const SGraphNode& Widget, const FVector2f& Size, FMeasuredNode& Out) const;

		const SGraphPanel* Panel = nullptr;
		bool bFullDetail = false;
	};
}
