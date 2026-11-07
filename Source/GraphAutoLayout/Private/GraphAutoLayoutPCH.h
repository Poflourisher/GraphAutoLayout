// Copyright 2026 Jot. All Rights Reserved.

// Module precompiled header. Kept small on purpose: without it this module would share
// the engine's multi-gigabyte UnrealEd header, which is slow to build and to load.
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Application/SlateApplication.h"
#include "SGraphPanel.h"
