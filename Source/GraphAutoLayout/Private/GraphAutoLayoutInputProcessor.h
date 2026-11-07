// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Application/IInputProcessor.h"
#include "InputCoreTypes.h"

class SGraphPanel;
class UEdGraph;

/**
 * Watches for the layout hotkey without ever consuming input.
 *
 * The graph panel implements "hold a key and click to spawn a node" (L = Lerp in Material and Niagara)
 * by remembering the key in SNodePanel::OnKeyDown and spawning on mouse up; SGraphPanel also spawns
 * on key down while a wire is being dragged. Swallowing either key event would break that, so we only
 * observe: arm on key down when a graph panel has focus and no mouse button is held, disarm on any
 * click or other key, and run the layout on key up if still armed.
 */
class FGraphAutoLayoutInputProcessor : public IInputProcessor
{
public:
	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}
	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override;
	virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override;
	virtual const TCHAR* GetDebugName() const override { return TEXT("GraphAutoLayout"); }

	/**
	 * The graph panel that owns keyboard focus, or null. The focused widget itself must be the panel or its
	 * graph editor wrapper, so typing in node titles, comments, search boxes or details panels never qualifies.
	 */
	static TSharedPtr<SGraphPanel> FindFocusedGraphPanel(FSlateApplication& SlateApp, uint32 UserIndex);

	/** Runs the layout on the panel's current selection (also used by the deferred trigger and the context menu). */
	static void LayoutSelection(const TSharedRef<SGraphPanel>& Panel);

	/** The graph panel that was last right-clicked, if it shows Graph. Context menus don't know their panel. */
	static TSharedPtr<SGraphPanel> GetLastRightClickedPanel(const UEdGraph* Graph);

private:
	bool MatchesHotkey(const FKeyEvent& InKeyEvent) const;
	void Disarm(const TCHAR* Reason);

	bool bArmed = false;
	FKey ArmedKey;
	TWeakPtr<SGraphPanel> ArmedPanel;
};
