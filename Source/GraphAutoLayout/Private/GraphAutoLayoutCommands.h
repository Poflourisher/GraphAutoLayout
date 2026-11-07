// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"

/**
 * Registers the rebindable hotkey (Editor Preferences > Keyboard Shortcuts > Graph Auto Layout).
 * The command is never mapped to a command list: command lists fire on key down, which would
 * break hold-L-and-click node spawning. FGraphAutoLayoutInputProcessor reads the active chord instead.
 */
class FGraphAutoLayoutCommands : public TCommands<FGraphAutoLayoutCommands>
{
public:
	FGraphAutoLayoutCommands();

	virtual void RegisterCommands() override;

	TSharedPtr<FUICommandInfo> LayoutSelectedNodes;
};
