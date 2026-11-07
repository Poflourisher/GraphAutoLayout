// Copyright 2026 Jot. All Rights Reserved.

#include "EdGraph/EdGraphNode.h"
#include "Framework/Application/SlateApplication.h"
#include "GraphAutoLayoutCommands.h"
#include "GraphAutoLayoutInputProcessor.h"
#include "GraphAutoLayoutLog.h"
#include "GraphAutoLayoutRunner.h"
#include "GraphAutoLayoutSettings.h"
#include "GraphAutoPlaceWatcher.h"
#include "HAL/IConsoleManager.h"
#include "Modules/ModuleManager.h"
#include "SGraphPanel.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayout"

DEFINE_LOG_CATEGORY(LogGraphAutoLayout);

static TAutoConsoleVariable<bool> CVarGraphAutoLayoutDebug(
	TEXT("GraphAutoLayout.Debug"),
	false,
	TEXT("Log what the graph auto layout does (adapter, node and layer counts, timing, skipped nodes) to LogGraphAutoLayout."));

bool UE::GraphAutoLayout::IsDebugLoggingEnabled()
{
	return CVarGraphAutoLayoutDebug.GetValueOnGameThread() || GetDefault<UGraphAutoLayoutSettings>()->bDebugLogging;
}

class FGraphAutoLayoutModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FGraphAutoLayoutCommands::Register();

		// No Slate application in commandlets; the hotkey, menu and auto-place are simply unavailable there.
		if (FSlateApplication::IsInitialized())
		{
			InputProcessor = MakeShared<FGraphAutoLayoutInputProcessor>();
			FSlateApplication::Get().RegisterInputPreProcessor(InputProcessor);
			AutoPlaceWatcher.Start();
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FGraphAutoLayoutModule::RegisterMenus));
		}
		UE_LOG(LogGraphAutoLayout, Log, TEXT("GraphAutoLayout started (hotkey %s)"), InputProcessor.IsValid() ? TEXT("active") : TEXT("inactive, no Slate"));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		AutoPlaceWatcher.Stop();
		if (InputProcessor.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(InputProcessor);
		}
		InputProcessor.Reset();
		FGraphAutoLayoutCommands::Unregister();
	}

private:
	/**
	 * Adds "Auto Layout Selected" to every graph node's right-click menu. All node menus chain up to the
	 * UEdGraphNode menu (SGraphEditorImpl::RegisterContextMenu), so one extension covers every graph editor;
	 * the entry is only built for graphs we can lay out.
	 */
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(TEXT("GraphEditor.GraphNodeContextMenu.EdGraphNode"));
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GraphAutoLayout"), LOCTEXT("ContextSection", "Auto Layout"));
		Section.AddDynamicEntry(TEXT("GraphAutoLayoutSelected"), FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
		{
			const UGraphNodeContextMenuContext* Context = InSection.FindContext<UGraphNodeContextMenuContext>();
			const UEdGraph* Graph = Context ? Context->Graph.Get() : nullptr;
			const TSharedPtr<SGraphPanel> Panel = FGraphAutoLayoutInputProcessor::GetLastRightClickedPanel(Graph);
			if (!Panel.IsValid() || !FGraphAutoLayoutRunner::CanLayout(Graph, Panel.Get()))
			{
				return;
			}
			const TWeakPtr<SGraphPanel> WeakPanel = Panel;
			const TSharedPtr<FUICommandInfo>& Command = FGraphAutoLayoutCommands::Get().LayoutSelectedNodes;
			InSection.AddEntry(FToolMenuEntry::InitMenuEntry(
				TEXT("GraphAutoLayoutSelected"),
				LOCTEXT("LayoutSelectedLabel", "Auto Layout Selected"),
				LOCTEXT("LayoutSelectedTooltip", "Lay out the selected nodes (same as releasing the Graph Auto Layout hotkey)."),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WeakPanel]()
				{
					if (const TSharedPtr<SGraphPanel> Pinned = WeakPanel.Pin())
					{
						FGraphAutoLayoutInputProcessor::LayoutSelection(Pinned.ToSharedRef());
					}
				})),
				EUserInterfaceActionType::Button,
				NAME_None,
				Command.IsValid() ? TOptional<FText>(Command->GetInputText()) : TOptional<FText>()));
		}));
	}

	TSharedPtr<FGraphAutoLayoutInputProcessor> InputProcessor;
	FGraphAutoPlaceWatcher AutoPlaceWatcher;
};

IMPLEMENT_MODULE(FGraphAutoLayoutModule, GraphAutoLayout)

#undef LOCTEXT_NAMESPACE
