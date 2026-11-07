// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Abstract graph description consumed by FGraphLayoutEngine.
 * Nothing in here knows about UEdGraph, Slate or a particular graph type: adapters translate
 * their graphs into these structs and translate the resulting positions back.
 *
 * Coordinate convention: graph space, +X right, +Y down, positions are node top-left corners.
 * Wires always leave a node on its right (output) side and enter on the left (input) side.
 */
namespace UE::GraphAutoLayout
{
	enum class EPinSide : uint8
	{
		Input,
		Output,
	};

	struct FLayoutPin
	{
		EPinSide Side = EPinSide::Input;

		/** Distance from the node's top edge to the pin's centre line. Used to make wires straight. */
		float OffsetY = 0.f;
	};

	enum class ELayerConstraint : uint8
	{
		None,
		/** Force into the leftmost layer (only honoured for nodes without upstream edges). */
		First,
		/** Force into the rightmost layer (only honoured for nodes without downstream edges). */
		Last,
	};

	struct FLayoutNode
	{
		FVector2f Size = FVector2f(100.f, 50.f);

		/** Position before layout. Used for anchoring and to keep the user's existing ordering stable. */
		FVector2f OrigPos = FVector2f::ZeroVector;

		TArray<FLayoutPin> Pins;

		/** In each island the node with the highest priority keeps its original position. */
		int32 AnchorPriority = 0;

		/**
		 * Satellites (e.g. Blueprint pure nodes) are not part of the main flow. Each one is attached to the
		 * earliest non-satellite node that consumes it and laid out as a small tree hanging to the left of it.
		 */
		bool bSatellite = false;

		/** Satellites attached to this node are kept at least this far below its top edge (clear of exec wires). */
		float SatelliteMinTop = 0.f;

		ELayerConstraint LayerConstraint = ELayerConstraint::None;

		/** Innermost group (index into FLayoutInput::Groups) this node belongs to, or INDEX_NONE. */
		int32 Group = INDEX_NONE;
	};

	/**
	 * A set of nodes (e.g. the contents of a comment box) that is laid out on its own first and then placed as one
	 * rigid block. Groups can nest. The block's frame is its members' bounds grown by the padding; nothing outside
	 * the group ends up inside the frame.
	 */
	struct FLayoutGroup
	{
		/** Enclosing group, or INDEX_NONE for a top-level group. */
		int32 Parent = INDEX_NONE;

		/** Room between the members and the frame: left/top (top usually includes a title bar) and right/bottom. */
		FVector2f PaddingMin = FVector2f::ZeroVector;
		FVector2f PaddingMax = FVector2f::ZeroVector;
	};

	/** Directed connection from an output pin (upstream) to an input pin (downstream). */
	struct FLayoutEdge
	{
		int32 SrcNode = INDEX_NONE;
		int32 SrcPin = INDEX_NONE;
		int32 DstNode = INDEX_NONE;
		int32 DstPin = INDEX_NONE;

		/** Primary edges (exec, parameter map) are straightened first and dominate alignment. */
		bool bPrimary = false;
	};

	enum class ELayeringMode : uint8
	{
		/** Layer 0 holds the sources; everything flows right from them (Blueprint exec). */
		FromSources,
		/** Layer 0 (rightmost) holds the sinks; inputs are stacked leftwards from them (Material, Niagara). */
		FromSinks,
	};

	struct FLayoutParams
	{
		ELayeringMode Mode = ELayeringMode::FromSources;

		float HorizontalSpacing = 80.f;
		float VerticalSpacing = 40.f;

		/** Tighter spacing used inside satellite trees. */
		float SatelliteHorizontalSpacing = 40.f;
		float SatelliteVerticalSpacing = 20.f;

		/**
		 * Pack disconnected islands into rows under the main island (true), or leave each near its
		 * original position and only push overlapping ones apart (false).
		 */
		bool bPackIslands = true;

		/** Gap between packed islands. */
		float IslandSpacing = 80.f;

		int32 CrossingSweeps = 8;
		int32 AlignmentSweeps = 8;

		/** Relative weight of primary edges versus ordinary data edges when aligning pins. */
		float PrimaryEdgeWeight = 10.f;
	};

	struct FLayoutInput
	{
		TArray<FLayoutNode> Nodes;
		TArray<FLayoutEdge> Edges;

		/** Optional nested groups, referenced by FLayoutNode::Group. */
		TArray<FLayoutGroup> Groups;

		/** Rectangles that must stay clear (unselected nodes). */
		TArray<FBox2f> Obstacles;

		FLayoutParams Params;
	};

	struct FLayoutStats
	{
		int32 NumIslands = 0;
		int32 NumBlocks = 0;
		int32 MaxLayers = 0;
		int32 NumDummies = 0;
		int32 NumReversedEdges = 0;
		int64 CrossingsBefore = 0;
		int64 CrossingsAfter = 0;

		/** Groups that had members, and how deeply they nest (1 = no nesting). */
		int32 NumGroups = 0;
		int32 MaxGroupDepth = 0;

		double Milliseconds = 0.0;
	};

	struct FLayoutResult
	{
		/** New top-left position per input node, same indexing as FLayoutInput::Nodes. */
		TArray<FVector2f> Positions;

		/** Final frame of each group, same indexing as FLayoutInput::Groups. Invalid (bIsValid == 0) for groups without members. */
		TArray<FBox2f> GroupBounds;

		FLayoutStats Stats;
	};
}
