// Copyright 2026 Jot. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

GRAPHAUTOLAYOUT_API DECLARE_LOG_CATEGORY_EXTERN(LogGraphAutoLayout, Log, All);

namespace UE::GraphAutoLayout
{
	/** True when the GraphAutoLayout.Debug cvar or the bDebugLogging setting is on. */
	GRAPHAUTOLAYOUT_API bool IsDebugLoggingEnabled();
}

/** Debug output: always available at Verbose, promoted to Log when debug logging is switched on. */
#define GRAPHAUTOLAYOUT_DEBUG(Format, ...) \
	do \
	{ \
		if (UE::GraphAutoLayout::IsDebugLoggingEnabled()) \
		{ \
			UE_LOG(LogGraphAutoLayout, Log, Format, ##__VA_ARGS__); \
		} \
		else \
		{ \
			UE_LOG(LogGraphAutoLayout, Verbose, Format, ##__VA_ARGS__); \
		} \
	} while (0)
