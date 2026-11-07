// Copyright 2026 Jot. All Rights Reserved.

#include "GraphNodeSizeResolver.h"

namespace UE::GraphAutoLayout
{
	const TCHAR* LexToString(EMeasureSource Source)
	{
		switch (Source)
		{
		case EMeasureSource::Widget: return TEXT("widget");
		case EMeasureSource::Cache: return TEXT("cache");
		default: return TEXT("estimate");
		}
	}

	FResolvedNodeSize ResolveNodeSize(bool bFullDetail, const FVector2f* WidgetSize, const FVector2f* CachedFullSize, const FVector2f& Estimate)
	{
		if (bFullDetail && WidgetSize)
		{
			return { *WidgetSize, EMeasureSource::Widget };
		}
		if (CachedFullSize)
		{
			return { WidgetSize ? FVector2f::Max(*CachedFullSize, *WidgetSize) : *CachedFullSize, EMeasureSource::Cache };
		}
		return { WidgetSize ? FVector2f::Max(Estimate, *WidgetSize) : Estimate, EMeasureSource::Estimate };
	}
}
