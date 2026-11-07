// Copyright 2026 Jot. All Rights Reserved.

#include "GraphAutoLayoutSettings.h"

#define LOCTEXT_NAMESPACE "GraphAutoLayout"

#if WITH_EDITOR
FText UGraphAutoLayoutSettings::GetSectionDescription() const
{
	return LOCTEXT("SettingsDescription",
		"Select nodes and press L (or right-click > Auto Layout Selected) to lay them out; Ctrl+A first for the whole graph. "
		"Works in Blueprint, Animation Blueprint, Material, PCG and Niagara Scratch Pad graphs. Change the key under Keyboard Shortcuts > Graph Auto Layout.");
}
#endif

#undef LOCTEXT_NAMESPACE
