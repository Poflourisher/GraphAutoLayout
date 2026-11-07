// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "GraphLayoutAdapter.h"

namespace UE::GraphAutoLayout
{
	/**
	 * Niagara Scratch Pad graphs (UNiagaraGraph inside the System/Emitter editor's Scratch Pad).
	 * Standalone Niagara script assets use the same graph class but are deliberately not supported;
	 * the two are told apart by the hosting widget (SNiagaraScratchPadScriptEditor).
	 *
	 * Data flows into the Output node. The parameter-map chain (InputMap -> Map Set -> ... -> Output)
	 * is the straight spine; Map Get nodes are ordinary data nodes placed near what they feed.
	 *
	 * Scratch Pad edits a transient copy and only applies it when the graph needs a recompile, which a
	 * position change alone never triggers, so PostLayout marks the graph changed (optional setting).
	 */
	class FNiagaraScratchPadLayoutAdapter : public IGraphLayoutAdapter
	{
	public:
		virtual FName GetName() const override { return TEXT("NiagaraScratchPad"); }
		virtual bool Supports(const UEdGraph& Graph) const override;
		virtual bool IsEnabled() const override;
		virtual ELayeringMode GetLayeringMode() const override { return ELayeringMode::FromSinks; }
		virtual bool IsPrimaryLink(const UEdGraphPin& OutputPin, const UEdGraphPin& InputPin) const override;
		virtual void ConfigureNode(const UEdGraphNode& Node, const FMeasuredNode& Measured, FLayoutNode& OutNode) const override;
		virtual bool CheckHost(const SGraphPanel& Panel, FText& OutReason) const override;
		virtual void PostLayout(UEdGraph& Graph, const TArray<UEdGraphNode*>& MovedNodes) const override;
	};
}
