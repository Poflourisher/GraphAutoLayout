// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace UE::GraphAutoLayout
{
	enum class EMeasureSource : uint8
	{
		/** Measured from the live widget at full detail. */
		Widget,
		/** Remembered from an earlier full-detail measurement. */
		Cache,
		/** Estimated from title and pin names. */
		Estimate,
	};

	GRAPHAUTOLAYOUT_API const TCHAR* LexToString(EMeasureSource Source);

	struct FResolvedNodeSize
	{
		FVector2f Size = FVector2f::ZeroVector;
		EMeasureSource Source = EMeasureSource::Estimate;
	};

	/**
	 * Picks the size a node is laid out with.
	 *
	 * Zoomed out (LowDetail and below) the graph panel draws simplified nodes: titles become a fixed
	 * placeholder (SGraphNode.cpp, TitleLODBranchNode) and pin names disappear (SGraphPin::UseLowDetailPinNames).
	 * Those widgets under-report the size the node has once you zoom back in, so laying out with them
	 * leaves gaps that are too small. Only full-detail widget sizes are trusted; otherwise we use the last
	 * full-detail size we saw, or else an estimate. Either is widened by the current widget size in case
	 * the node grew (e.g. pins added) since.
	 *
	 * @param bFullDetail     The panel is drawing nodes at full detail (MediumDetail or closer).
	 * @param WidgetSize      Current widget desired size, if there is a widget.
	 * @param CachedFullSize  Last full-detail size measured for this node, if any.
	 * @param Estimate        Estimated full-detail size.
	 */
	GRAPHAUTOLAYOUT_API FResolvedNodeSize ResolveNodeSize(bool bFullDetail, const FVector2f* WidgetSize, const FVector2f* CachedFullSize, const FVector2f& Estimate);
}
