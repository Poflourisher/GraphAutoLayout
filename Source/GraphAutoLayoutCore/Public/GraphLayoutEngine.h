// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutTypes.h"

namespace UE::GraphAutoLayout
{
	/**
	 * Layered (Sugiyama-style) layout of an abstract node graph.
	 *
	 * Pipeline per connected island:
	 *   cycle breaking -> layering -> dummy nodes for long edges -> barycenter crossing reduction
	 *   -> column X placement -> iterative pin alignment for Y -> anchoring -> island/obstacle separation.
	 *
	 * Satellite nodes (FLayoutNode::bSatellite) are first grouped with their owning node into rigid blocks,
	 * each laid out with the same pipeline, and the blocks are then laid out as single nodes.
	 *
	 * Groups (FLayoutInput::Groups, e.g. comment boxes) work the same way one level up: innermost first, each group's
	 * members are laid out on their own and then placed as one node sized to the group's frame, so a group never
	 * interleaves with outside nodes. FLayoutResult::GroupBounds reports each frame.
	 *
	 * Overall cost is roughly O(S * (V + E) log V) for S sweeps, with V/E counting dummy nodes and segments.
	 */
	class GRAPHAUTOLAYOUTCORE_API FGraphLayoutEngine
	{
	public:
		static FLayoutResult Run(const FLayoutInput& Input);

		/**
		 * Smallest vertical shift (up or down, ties go down) that keeps every rect at least the spacing away
		 * from every obstacle. Used to move a laid-out group, or a single placed node, out of the way.
		 */
		static float FindFreeVerticalShift(TConstArrayView<FBox2f> Rects, TConstArrayView<FBox2f> Obstacles, float HorizontalSpacing, float VerticalSpacing);
	};
}
