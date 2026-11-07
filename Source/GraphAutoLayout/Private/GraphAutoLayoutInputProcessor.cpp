// Copyright 2026 Jot. All Rights Reserved.

#include "GraphAutoLayoutInputProcessor.h"

#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphAutoLayoutCommands.h"
#include "GraphAutoLayoutLog.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphEditor.h"
#include "Layout/WidgetPath.h"
#include "SGraphPanel.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayout"

namespace UE::GraphAutoLayout::InputPrivate
{
	// Type names come from SNew/SAssignNew (SWidget::GetType). SGraphEditorImpl is private to the
	// GraphEditor module, so we match it by name and walk up to its public SGraphEditor wrapper.
	static const FName GraphPanelType(TEXT("SGraphPanel"));
	static const FName GraphEditorType(TEXT("SGraphEditor"));
	static const FName GraphEditorImplType(TEXT("SGraphEditorImpl"));

	static TWeakPtr<SGraphPanel> LastRightClickedPanel;

	static void Notify(const FText& Text)
	{
		if (GetDefault<UGraphAutoLayoutSettings>()->bShowNotifications)
		{
			FNotificationInfo Info(Text);
			Info.ExpireDuration = 2.5f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}
}

bool FGraphAutoLayoutInputProcessor::MatchesHotkey(const FKeyEvent& InKeyEvent) const
{
	if (!FGraphAutoLayoutCommands::IsRegistered())
	{
		return false;
	}
	const TSharedPtr<FUICommandInfo>& Command = FGraphAutoLayoutCommands::Get().LayoutSelectedNodes;
	if (!Command.IsValid())
	{
		return false;
	}
	const FInputChord Pressed(InKeyEvent.GetKey(), EModifierKey::FromBools(InKeyEvent.IsControlDown(), InKeyEvent.IsAltDown(), InKeyEvent.IsShiftDown(), InKeyEvent.IsCommandDown()));
	for (EMultipleKeyBindingIndex Index : { EMultipleKeyBindingIndex::Primary, EMultipleKeyBindingIndex::Secondary })
	{
		const TSharedRef<const FInputChord> Chord = Command->GetActiveChord(Index);
		if (Chord->IsValidChord() && Chord->GetRelationship(Pressed) == FInputChord::ERelationshipType::Same)
		{
			return true;
		}
	}
	return false;
}

TSharedPtr<SGraphPanel> FGraphAutoLayoutInputProcessor::FindFocusedGraphPanel(FSlateApplication& SlateApp, uint32 UserIndex)
{
	using namespace UE::GraphAutoLayout::InputPrivate;

	const TSharedPtr<SWidget> Focused = SlateApp.GetUserFocusedWidget(UserIndex);
	if (!Focused.IsValid())
	{
		return nullptr;
	}
	const FName Type = Focused->GetType();
	if (Type == GraphPanelType)
	{
		return StaticCastSharedPtr<SGraphPanel>(Focused);
	}
	if (Type == GraphEditorImplType || Type == GraphEditorType)
	{
		for (TSharedPtr<SWidget> Widget = Focused; Widget.IsValid(); Widget = Widget->GetParentWidget())
		{
			if (Widget->GetType() == GraphEditorType)
			{
				if (SGraphPanel* Panel = StaticCastSharedPtr<SGraphEditor>(Widget)->GetGraphPanel())
				{
					return StaticCastSharedRef<SGraphPanel>(Panel->AsShared());
				}
				return nullptr;
			}
		}
	}
	return nullptr;
}

void FGraphAutoLayoutInputProcessor::Disarm(const TCHAR* Reason)
{
	if (bArmed)
	{
		GRAPHAUTOLAYOUT_DEBUG(TEXT("Hotkey disarmed: %s"), Reason);
	}
	bArmed = false;
	ArmedPanel.Reset();
}

bool FGraphAutoLayoutInputProcessor::HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.IsRepeat())
	{
		return false;
	}
	if (bArmed)
	{
		// Any other key while held means the user is doing something else (a different chord).
		if (InKeyEvent.GetKey() != ArmedKey)
		{
			Disarm(TEXT("another key was pressed"));
		}
		return false;
	}
	if (!MatchesHotkey(InKeyEvent))
	{
		return false;
	}
	// A held mouse button means a wire drag or node drag is in progress: the key spawns a node there.
	if (SlateApp.GetPressedMouseButtons().Num() > 0)
	{
		GRAPHAUTOLAYOUT_DEBUG(TEXT("Hotkey ignored: mouse button held"));
		return false;
	}
	const TSharedPtr<SGraphPanel> Panel = FindFocusedGraphPanel(SlateApp, InKeyEvent.GetUserIndex());
	if (!Panel.IsValid())
	{
		return false;
	}
	bArmed = true;
	ArmedKey = InKeyEvent.GetKey();
	ArmedPanel = Panel;
	GRAPHAUTOLAYOUT_DEBUG(TEXT("Hotkey armed on graph %s"), Panel->GetGraphObj() ? *Panel->GetGraphObj()->GetName() : TEXT("<none>"));
	return false;
}

bool FGraphAutoLayoutInputProcessor::HandleKeyUpEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent)
{
	if (!bArmed || InKeyEvent.GetKey() != ArmedKey)
	{
		return false;
	}
	const TSharedPtr<SGraphPanel> Panel = ArmedPanel.Pin();
	Disarm(TEXT("key released"));

	// Focus may have moved while the key was held (e.g. a popup); only act on the same panel.
	if (!Panel.IsValid() || FindFocusedGraphPanel(SlateApp, InKeyEvent.GetUserIndex()) != Panel)
	{
		GRAPHAUTOLAYOUT_DEBUG(TEXT("Hotkey released but the graph panel lost focus"));
		return false;
	}

	// Defer to the next tick: Slate finishes routing this key up first (SNodePanel resets its
	// remembered chord there), and the layout work stays out of input handling.
	const TWeakPtr<SGraphPanel> WeakPanel = Panel;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakPanel](float)
	{
		if (const TSharedPtr<SGraphPanel> Pinned = WeakPanel.Pin())
		{
			LayoutSelection(Pinned.ToSharedRef());
		}
		return false;
	}));
	return false;
}

bool FGraphAutoLayoutInputProcessor::HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	// Key + click is the node spawn chord (L = Lerp); never lay out after that.
	Disarm(TEXT("mouse button pressed while the key was held"));

	// Remember which graph panel is being right-clicked so the context menu entry knows its selection.
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		using namespace UE::GraphAutoLayout::InputPrivate;
		const FWidgetPath Path = SlateApp.LocateWindowUnderMouse(MouseEvent.GetScreenSpacePosition(), SlateApp.GetInteractiveTopLevelWindows());
		TSharedPtr<SGraphPanel> Found;
		for (int32 Idx = Path.Widgets.Num() - 1; Idx >= 0 && !Found.IsValid(); --Idx)
		{
			const TSharedRef<SWidget>& Widget = Path.Widgets[Idx].Widget;
			if (Widget->GetType() == GraphPanelType)
			{
				Found = StaticCastSharedRef<SGraphPanel>(Widget);
			}
		}
		LastRightClickedPanel = Found;
	}
	return false;
}

TSharedPtr<SGraphPanel> FGraphAutoLayoutInputProcessor::GetLastRightClickedPanel(const UEdGraph* Graph)
{
	const TSharedPtr<SGraphPanel> Panel = UE::GraphAutoLayout::InputPrivate::LastRightClickedPanel.Pin();
	return Panel.IsValid() && Graph && Panel->GetGraphObj() == Graph ? Panel : nullptr;
}

bool FGraphAutoLayoutInputProcessor::HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent)
{
	Disarm(TEXT("double click while the key was held"));
	return false;
}

void FGraphAutoLayoutInputProcessor::LayoutSelection(const TSharedRef<SGraphPanel>& Panel)
{
	using namespace UE::GraphAutoLayout::InputPrivate;

	UEdGraph* Graph = Panel->GetGraphObj();
	if (!Graph)
	{
		return;
	}
	if (!Panel->IsGraphEditable() || !Graph->bEditable)
	{
		Notify(LOCTEXT("ReadOnly", "Auto layout: this graph is read-only."));
		return;
	}

	const TArray<UEdGraphNode*> Selected = Panel->GetSelectedGraphNodes();
	if (Selected.Num() == 0)
	{
		Notify(LOCTEXT("NoSelection", "Auto layout: select nodes first (Ctrl+A for the whole graph)."));
		return;
	}

	const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Selected, &Panel.Get());
	if (!Result.bSucceeded)
	{
		Notify(Result.Message);
	}
	GRAPHAUTOLAYOUT_DEBUG(TEXT("%s"), *Result.Message.ToString());
}

#undef LOCTEXT_NAMESPACE
