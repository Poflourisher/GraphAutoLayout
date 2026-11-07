// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GraphAutoLayoutSettings.generated.h"

UENUM()
enum class EGraphAutoLayoutCommentMode : uint8
{
	/** Comment boxes are left exactly as they are. */
	Ignore,
	/** Comments that contained moved nodes are resized to wrap their nodes again. Their nodes are laid out like any other. */
	ResizeToFit UMETA(DisplayName = "Resize To Fit"),
	/**
	 * Comments in Group Movement mode keep their nodes together: each one's nodes are laid out as a block, and the comment
	 * is fitted around them, so other nodes never end up inside it. Selecting a comment lays out everything inside it.
	 * Comments in plain Comment mode are resized like Resize To Fit.
	 */
	KeepTogether UMETA(DisplayName = "Keep Together"),
};

/**
 * Editor Preferences > Plugins > Graph Auto Layout. Per-user settings.
 * The hotkey itself lives in Editor Preferences > Keyboard Shortcuts > Graph Auto Layout.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Graph Auto Layout"))
class GRAPHAUTOLAYOUT_API UGraphAutoLayoutSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
#if WITH_EDITOR
	virtual FText GetSectionDescription() const override;
#endif

	// ---- General ----

	/** Show a small toast when the hotkey does nothing (empty selection, unsupported or read-only graph). */
	UPROPERTY(config, EditAnywhere, Category = "General")
	bool bShowNotifications = true;

	// ---- Spacing ----

	/** Gap between columns of nodes. */
	UPROPERTY(config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "0", ClampMax = "1000", UIMax = "400"))
	float HorizontalSpacing = 80.f;

	/** Gap between nodes stacked in the same column. */
	UPROPERTY(config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "0", ClampMax = "1000", UIMax = "400"))
	float VerticalSpacing = 40.f;

	/** Blueprint only: horizontal gap inside the small trees of pure nodes hanging off an exec node. */
	UPROPERTY(config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "0", ClampMax = "1000", UIMax = "400"))
	float PureNodeHorizontalSpacing = 40.f;

	/** Blueprint only: vertical gap inside the small trees of pure nodes hanging off an exec node. */
	UPROPERTY(config, EditAnywhere, Category = "Spacing", meta = (ClampMin = "0", ClampMax = "1000", UIMax = "400"))
	float PureNodeVerticalSpacing = 20.f;

	// ---- Disconnected groups ----

	/**
	 * When the selection contains groups that aren't connected to each other, pack them into tidy rows
	 * under the main group. Off: each group stays near where it was and only overlapping groups move.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Disconnected Groups", meta = (DisplayName = "Compact Disconnected Groups"))
	bool bCompactDisconnectedGroups = true;

	/** Gap between packed groups. Larger than the node spacing so separate groups still read as separate. */
	UPROPERTY(config, EditAnywhere, Category = "Disconnected Groups", meta = (ClampMin = "0", ClampMax = "1000", UIMax = "400", EditCondition = "bCompactDisconnectedGroups"))
	float GroupSpacing = 80.f;

	// ---- Comments ----

	/** What happens to comment boxes when the nodes inside them are laid out. Comments themselves are never laid out as nodes. */
	UPROPERTY(config, EditAnywhere, Category = "Comments")
	EGraphAutoLayoutCommentMode CommentMode = EGraphAutoLayoutCommentMode::KeepTogether;

	/** Space between a resized comment's border and the nodes inside it (the title bar is added on top). */
	UPROPERTY(config, EditAnywhere, Category = "Comments", meta = (ClampMin = "0", ClampMax = "400", UIMax = "120", EditCondition = "CommentMode != EGraphAutoLayoutCommentMode::Ignore"))
	float CommentPadding = 30.f;

	// ---- Auto format ----

	/**
	 * When you add a node that gets wired to an existing node (e.g. by dragging off a pin), snap the new node next to it
	 * with a straight wire, clear of other nodes. Existing nodes never move. Pasting or duplicating several nodes is ignored.
	 * It is its own undo step, so Ctrl+Z once keeps the node where you dropped it.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Auto Format", meta = (DisplayName = "Auto Place New Nodes"))
	bool bAutoFormatOnNodePlaced = false;

	// ---- Graph types ----

	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for Blueprint Graphs"))
	bool bEnableBlueprint = true;

	/** Animation Blueprint AnimGraph, state graphs and transition rules (not the state machine overview). */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for Animation Blueprint Graphs"))
	bool bEnableAnimGraph = true;

	/** Materials and Material Functions. */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for Material Graphs"))
	bool bEnableMaterial = true;

	/** PCG graphs: the Input node stays put and data flows right from it. */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for PCG Graphs"))
	bool bEnablePCG = true;

	/** Niagara Scratch Pad graphs only; standalone Niagara script assets are not supported. */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for Niagara Scratch Pad"))
	bool bEnableNiagaraScratchPad = true;

	/**
	 * Scratch Pad only applies (and keeps) a graph when it needs a recompile, which a pure position change never
	 * triggers. When on, a layout marks the graph changed so Apply/Save keep it; the scratch script recompiles on Apply.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Mark Niagara Scratch Pad Changed After Layout", EditCondition = "bEnableNiagaraScratchPad"))
	bool bMarkNiagaraScratchPadChanged = true;

	/** The Niagara System Overview: the System node and its emitters are lined up in one row, in emitter-list order. */
	UPROPERTY(config, EditAnywhere, Category = "Graph Types", meta = (DisplayName = "Enable for Niagara System Overview"))
	bool bEnableNiagaraSystemOverview = true;

	// ---- Debug ----

	/** Log adapter, node/layer counts, timing and skipped nodes to LogGraphAutoLayout. Same as the GraphAutoLayout.Debug console variable. */
	UPROPERTY(config, EditAnywhere, Category = "Debug")
	bool bDebugLogging = false;
};
