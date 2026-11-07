// Copyright 2026 Jot. All Rights Reserved.

#include "GraphLayoutTestUtils.h"
#include "GraphNodeSizeResolver.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGraphNodeSizeResolverTest, "GraphAutoLayout.Measure.ZoomIndependentSize", UE::GraphAutoLayout::Tests::TestFlags)
bool FGraphNodeSizeResolverTest::RunTest(const FString& Parameters)
{
	using namespace UE::GraphAutoLayout;

	const FVector2f FullSize(300.f, 120.f);
	const FVector2f ZoomedOutSize(150.f, 100.f); // Simplified title + no pin names when zoomed out.
	const FVector2f Estimate(260.f, 110.f);

	// Zoomed in: the live widget is the truth.
	{
		const FResolvedNodeSize R = ResolveNodeSize(true, &FullSize, nullptr, Estimate);
		TestTrue(TEXT("Full detail uses the widget"), R.Source == EMeasureSource::Widget && R.Size == FullSize);
	}
	// Zoomed out after having seen the node zoomed in: the remembered full size wins over the small widget.
	{
		const FResolvedNodeSize R = ResolveNodeSize(false, &ZoomedOutSize, &FullSize, Estimate);
		TestTrue(TEXT("Low detail uses the cached full size"), R.Source == EMeasureSource::Cache && R.Size == FullSize);
	}
	// Zoomed out, never seen up close: never smaller than the estimate.
	{
		const FResolvedNodeSize R = ResolveNodeSize(false, &ZoomedOutSize, nullptr, Estimate);
		TestTrue(TEXT("Low detail without cache uses the estimate"), R.Source == EMeasureSource::Estimate && R.Size == Estimate);
	}
	// Node grew taller since it was cached (pins added while zoomed out): take the larger of each axis.
	{
		const FVector2f Taller(150.f, 200.f);
		const FResolvedNodeSize R = ResolveNodeSize(false, &Taller, &FullSize, Estimate);
		TestTrue(TEXT("Cached size widened by a larger widget"), R.Size == FVector2f(300.f, 200.f));
	}
	// Headless (no widget): estimate, or the cache if available.
	{
		TestTrue(TEXT("No widget, no cache -> estimate"), ResolveNodeSize(true, nullptr, nullptr, Estimate).Size == Estimate);
		TestTrue(TEXT("No widget, cache -> cache"), ResolveNodeSize(true, nullptr, &FullSize, Estimate).Size == FullSize);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
