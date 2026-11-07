// Copyright 2026 Jot. All Rights Reserved.

#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GraphAutoLayoutRunner.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "GraphEditor.h"
#include "SGraphPanel.h"
#include "ShaderCompiler.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/SWindow.h"
#include "Widgets/Docking/SDockTab.h"

DEFINE_LOG_CATEGORY_STATIC(LogGraphAutoLayoutPerf, Log, All);

namespace UE::GraphAutoLayout::Perf
{
	/**
	 * Profiles the hotkey path in a real editor: -GraphAutoLayoutPerf=/Path/To/Asset.Asset
	 * Opens the asset, waits for shaders, then lays out every node of the graph with the live panel,
	 * logging each step and the frames that follow (the editor reacting to the change).
	 */
	class FProfiler : public TSharedFromThis<FProfiler>
	{
	public:
		void Start(const FString& InAssetPath)
		{
			AssetPath = InAssetPath;
			Handle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(AsShared(), &FProfiler::Tick));
		}

	private:
		// The panel is not reachable through GetChildren; SGraphEditor hands it out directly.
		static TSharedPtr<SGraphPanel> FindPanel(const TSharedRef<SWidget>& Widget)
		{
			static const FName EditorType("SGraphEditor");
			if (Widget->GetType() == EditorType)
			{
				if (SGraphPanel* Found = StaticCastSharedRef<SGraphEditor>(Widget)->GetGraphPanel())
				{
					return StaticCastSharedRef<SGraphPanel>(Found->AsShared());
				}
			}
			FChildren* Children = Widget->GetChildren();
			for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
			{
				if (TSharedPtr<SGraphPanel> Found = FindPanel(Children->GetChildAt(Index)))
				{
					return Found;
				}
			}
			return nullptr;
		}

		static void DumpTypes(const TSharedRef<SWidget>& Widget, int32 Depth)
		{
			const FString Type = Widget->GetTypeAsString();
			if (Type.Contains(TEXT("Graph")) || Type.Contains(TEXT("Dock")) || Type.Contains(TEXT("Splitter")) || Depth < 3)
			{
				UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF tree %s%s (%d children)"), *FString::ChrN(Depth, TEXT('.')), *Type, Widget->GetChildren() ? Widget->GetChildren()->Num() : 0);
			}
			FChildren* Children = Widget->GetChildren();
			for (int32 Index = 0; Children && Index < Children->Num() && Depth < 60; ++Index)
			{
				DumpTypes(Children->GetChildAt(Index), Depth + 1);
			}
		}

		void RunLayout(const TCHAR* Label)
		{
			UEdGraph* Graph = Panel->GetGraphObj();
			TArray<UEdGraphNode*> Nodes(Graph->Nodes);
			const double Begin = FPlatformTime::Seconds();
			const FGraphAutoLayoutRunResult Result = FGraphAutoLayoutRunner::LayoutNodes(Graph, Nodes, Panel.Get());
			const double Total = (FPlatformTime::Seconds() - Begin) * 1000.0;
			// For graphs laid out in a row (Niagara System Overview), show the resulting order and alignment.
			if (Graph->GetSchema() && Graph->GetSchema()->GetClass()->GetName().Contains(TEXT("Overview")))
			{
				TArray<UEdGraphNode*> Sorted(Graph->Nodes);
				Sorted.Sort([](const UEdGraphNode& A, const UEdGraphNode& B) { return A.NodePosX < B.NodePosX; });
				for (const UEdGraphNode* Node : Sorted)
				{
					UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF row: x=%d y=%d %s"), Node->NodePosX, Node->NodePosY, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
			UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF %s: %d nodes, %d moved | total %.1f ms = measure %.1f + layout %.1f + apply %.1f + comments %.1f + finish %.1f | %s"),
				Label, Result.NumLaidOut, Result.NumMoved, Total, Result.MeasureMs, Result.LayoutMs, Result.ApplyMs, Result.CommentsMs, Result.FinishMs, *Result.Message.ToString());
			FramesToLog = 8;
			FrameLabel = Label;
		}

		bool Tick(float DeltaTime)
		{
			if (FramesToLog > 0)
			{
				UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF %s frame +%d: %.1f ms"), *FrameLabel, 9 - FramesToLog, DeltaTime * 1000.0);
				--FramesToLog;
				return true;
			}

			const double Now = FPlatformTime::Seconds();
			if (Now < NextTime)
			{
				return true;
			}
			switch (Stage++)
			{
			case 0:
				if (!GEditor || !FSlateApplication::IsInitialized() || !FGlobalTabmanager::Get()->FindExistingLiveTab(FTabId("LevelEditor")).IsValid())
				{
					--Stage;
					return true;
				}
				if (UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath))
				{
					GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
				}
				NextTime = Now + 5.0;
				break;
			case 1:
				if (GShaderCompilingManager)
				{
					GShaderCompilingManager->FinishAllCompilation();
				}
				NextTime = Now + 5.0;
				break;
			case 2:
				if (UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath))
				{
					IAssetEditorInstance* Editor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Asset, true);
					const TSharedPtr<FTabManager> TabManager = Editor ? Editor->GetAssociatedTabManager() : nullptr;
					const TSharedPtr<SDockTab> OwnerTab = TabManager.IsValid() ? TabManager->GetOwnerTab() : nullptr;
					if (OwnerTab.IsValid())
					{
						Panel = FindPanel(OwnerTab->GetContent());
					}
					UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF search %d: editor=%d tab=%d panel=%d"), PanelSearches, Editor != nullptr, OwnerTab.IsValid(), Panel.IsValid());
				}
				if (!Panel.IsValid() && ++PanelSearches < 20)
				{
					--Stage;
					NextTime = Now + 1.0;
					return true;
				}
				if (!Panel.IsValid())
				{
					UE_LOG(LogGraphAutoLayoutPerf, Error, TEXT("PERF no graph panel found"));
					FPlatformMisc::RequestExit(false);
					return false;
				}
				UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF graph %s with %d nodes"), *Panel->GetGraphObj()->GetName(), Panel->GetGraphObj()->Nodes.Num());
				FramesToLog = 8;
				FrameLabel = TEXT("idle");
				NextTime = Now + 1.0;
				break;
			case 3:
				RunLayout(TEXT("first L"));
				NextTime = Now + 3.0;
				break;
			case 4:
				RunLayout(TEXT("second L, nothing to move"));
				NextTime = Now + 3.0;
				break;
			case 5:
				RunLayout(TEXT("third L"));
				NextTime = Now + 3.0;
				break;
			case 6:
				RunLayout(TEXT("fourth L"));
				NextTime = Now + 3.0;
				break;
			default:
				UE_LOG(LogGraphAutoLayoutPerf, Display, TEXT("PERF DONE"));
				Panel.Reset();
				FPlatformMisc::RequestExit(false);
				return false;
			}
			return true;
		}

		FString AssetPath;
		FTSTicker::FDelegateHandle Handle;
		TSharedPtr<SGraphPanel> Panel;
		int32 Stage = 0;
		double NextTime = 0.0;
		int32 FramesToLog = 0;
		int32 PanelSearches = 0;
		FString FrameLabel;
	};

	static TSharedPtr<FProfiler> GProfiler;
}

class FGraphAutoLayoutTestsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FString AssetPath;
		if (FParse::Value(FCommandLine::Get(), TEXT("GraphAutoLayoutPerf="), AssetPath))
		{
			UE::GraphAutoLayout::Perf::GProfiler = MakeShared<UE::GraphAutoLayout::Perf::FProfiler>();
			UE::GraphAutoLayout::Perf::GProfiler->Start(AssetPath);
		}
	}

	virtual void ShutdownModule() override
	{
		UE::GraphAutoLayout::Perf::GProfiler.Reset();
	}
};

IMPLEMENT_MODULE(FGraphAutoLayoutTestsModule, GraphAutoLayoutTests)
