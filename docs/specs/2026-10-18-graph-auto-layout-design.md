# GraphAutoLayout — Design (draft for approval)

Engine: UE 5.8.3 (CL 58210709, launcher build). Project: `E:\Projects\Unreal\AutoAlignNodes`.
All engine paths below are relative to `C:\Program Files\Epic Games\UE_5.8\Engine\`.

Status: **APPROVED 2026-10-18** with all recommended answers (see §9). Addendum §10 (comment groups, 2026-10-19): **approved and implemented**. Automation tests pass (34/34); the manual checklist in §10.8 is pending.

---

## 1. Engine findings (verified in source)

### 1.1 The "hold key + click spawns node" chord is generic, not Material-specific
- `Source/Editor/GraphEditor/Private/SNodePanel.cpp:1224-1247`. `SNodePanel::OnKeyDown` stores the key in `LastKeyChordDetected` and returns **Unhandled**. `OnKeyUp` clears it if it's the same key. `OnFocusLost` clears it.
- `SNodePanel.cpp:1138-1148`: on left mouse **up** over empty panel background (with mouse capture), it calls `OnSpawnNodeByShortcutAtLocation(LastKeyChordDetected, pos)`.
- `SGraphPanel.cpp:906-957`: second path. While dragging a wire out of a **single** pin, pressing a key spawns and auto-wires the node immediately, on key **down**.
- Each editor maps chord → node type:
  - Material: `FMaterialEditor::OnSpawnGraphNodeByShortcut` (`MaterialEditor/Private/MaterialEditor.cpp:8700`), via `FMaterialEditorSpawnNodeCommands`. The config is `Config/BaseEditorPerProjectUserSettings.ini:848`: `MaterialExpressionLinearInterpolate Key=L`.
  - Blueprint: `FBlueprintEditor::OnSpawnGraphNodeByShortcut` (`Kismet/Private/BlueprintEditor.cpp:10684`), with `[BlueprintSpawnNodes]` at ini:799-810. **L is not bound by default** (P, N, D, B, S, G, M, F, O, R are).
  - **Niagara also binds L → Lerp.** `Plugins/FX/Niagara/Config/DefaultNiagara.ini:103` (`GraphCreationShortcuts`), consumed in `SNiagaraScriptGraph.cpp:471`.
- No `UI_COMMAND` binds bare `L` in GraphEditor, Kismet, MaterialEditor or NiagaraEditor (grep for `EKeys::L`). The only bare-L UI commands are Sequencer ShuttleForward and the UMG Designer. They live in other binding contexts, so they don't conflict inside a graph panel.

**Consequence:** if our hotkey ever *consumes* the L KeyDown or KeyUp, it breaks L+click Lerp in both the Material and Niagara editors. If we consumed KeyUp, `LastKeyChordDetected` would stay stuck and a later plain click would spawn a Lerp. **The trigger must observe input and never consume it.**

### 1.2 Focus and selection
- When a graph is focused, keyboard focus is on either `SGraphPanel` (`SGraphPanel.cpp:801`, 1512) or `SGraphEditorImpl` (`SGraphEditorImpl.cpp:133-155`, which takes focus on mouse down). `SGraphEditor` (public, `UnrealEd/Public/GraphEditor.h`) wraps the impl and exposes `GetGraphPanel()` (727), `GetCurrentGraph()` (241) and `GetSelectedNodes()` (394).
- `SGraphPanel` (public, `GraphEditor/Public/SGraphPanel.h`) gives `GetGraphObj()` (289), `GetSelectedNodes()` (`SNodePanel.h:244`), `GetNodeWidgetFromGuid()` (349) and `GetBoundsForNode()` (299). `SGraphPanel` is declared with `SLATE_DECLARE_WIDGET_API`, so the widget type can be checked safely.
- Real sizes: `SNodePanel::GetBoundsForNode` (`SNodePanel.cpp:1721`) uses `SNode::GetDesiredSize()`, the same measure the engine's own align/straighten tools use.
- Pin offsets: `SGraphNode::FindWidgetForPin()` (`SGraphNode.h:240`) and `SGraphPin::GetNodeOffset()` (`SGraphPin.h:245`). **Caveat:** `CachedNodeOffset` is only refreshed in `SGraphPin::Tick` (`SGraphPin.cpp:929`), so it can be stale for nodes that are culled off-screen. See Risks.

### 1.3 Material position persistence
- An interactive drag goes through `SGraphNodeMaterialBase::MoveTo` (`MaterialEditor/Private/MaterialNodes/SGraphNodeMaterialBase.cpp:518-526`). It copies `NodePosX/Y` into `MaterialExpression->MaterialExpressionEditorX/Y`, marks the package dirty and fires `MaterialDirtyDelegate`.
- `UMaterialGraph::LinkMaterialExpressionsFromGraph` (`UnrealEd/Private/MaterialGraph.cpp:496-624`) also syncs on apply/save. That covers expressions (544-555), comments (602-623) and the root node → `UMaterial::EditorX/Y` (504-506).
- **Plan:** we write the data ourselves rather than calling widget `MoveTo`. Two reasons: it must work headless, and `SGraphNodeComment::MoveTo` drags contained nodes along.
  - Expressions: `GraphNode->Modify(); Expression->Modify();`, then set `NodePos*` and `MaterialExpressionEditorX/Y`, then `MaterialDirtyDelegate.ExecuteIfBound()`.
  - Root node: `Material->Modify()` and set `EditorX/Y`.
  - Both the expression and the graph node go into the transaction, so undo restores the source of truth even if the editor rebuilds the graph from expressions.

### 1.4 Niagara: which graphs are UEdGraph-based
| Surface | Graph type | Hosted in | In scope |
|---|---|---|---|
| Module Script, Dynamic Input, Function (asset editor) | `UNiagaraGraph : UEdGraph` (`NiagaraEditor/Public/NiagaraGraph.h:235`) | `SNiagaraScriptGraph` → `SGraphEditor` (`SNiagaraScriptGraph.cpp:370`), opened from `NiagaraScriptToolkit.cpp` | ✅ |
| Scratch Pad modules / dynamic inputs | `UNiagaraGraph` | `NiagaraScratchScriptEditor.cpp`, `SNiagaraScratchPadScriptManager.cpp` → `SNiagaraScriptGraph` | ✅ |
| Emitter/System script graph view (System toolkit) | `UNiagaraGraph` | `NiagaraSystemToolkitModeBase.cpp:416,436`, `SNiagaraSystemScript.cpp:18` | ✅ (graph is usually edited through the stack, so treat as low priority) |
| System Overview | plain `UEdGraph` + `UEdGraphSchema_NiagaraSystemOverview`, nodes `UNiagaraOverviewNode` with **no pins** (`NiagaraSystemEditorData.cpp:88`) | `NiagaraEditorWidgets/Private/SNiagaraOverviewGraph.cpp` | ❓ see Q9 |
| Emitter/System **stack** | not a node graph | — | ❌ |

Header visibility (`Plugins/FX/Niagara/Source/NiagaraEditor/`):
- **Public:** `NiagaraGraph.h`, `NiagaraNode.h`, `NiagaraNodeOutput.h`, `NiagaraNodeInput.h`, `NiagaraNodeFunctionCall.h`, `NiagaraNodeOp.h`, `NiagaraNodeCustomHlsl.h`, `EdGraphSchema_Niagara.h` (`PinToTypeDefinition`, line 163), `NiagaraOverviewNode.h`.
- **Private (cannot include):** `NiagaraNodeParameterMapGet/Set.h`, `NiagaraNodeReroute.h`, `NiagaraNodeIf/Select/StaticSwitch/Convert.h`.
- **Alternative for the private ones:**
  - Parameter-map edges: use `UEdGraphSchema_Niagara::PinToTypeDefinition(Pin) == FNiagaraTypeDefinition::GetParameterMapDef()` (`Niagara/Public/NiagaraTypes.h:1038`). Both are public.
  - Reroutes: check the class by name via `FindObject<UClass>(…"/Script/NiagaraEditor.NiagaraNodeReroute")`. This needs no header, and a missing class is handled gracefully.
- Niagara structure: InputMap node → Map Get / ops / function calls → chain of Map Set → `UNiagaraNodeOutput`. Data flows into the output node, with the parameter-map wire as the "spine". This is a sink-anchored, Material-like graph with one privileged edge kind. Phase 3 re-verifies this on real assets before implementation.

### 1.5 Automation command line (verified)
- `Automation RunTests <filter>; Quit`: both sub-commands are parsed in `Developer/AutomationController/Private/AutomationCommandline.cpp:610` and `:726`.
- `-ReportExportPath=<dir>` (`AutomationControllerManager.cpp:233`) writes `index.json` with per-test results.
- `-testexit=` exists (`LaunchEngineLoop.cpp:1923`) but isn't needed when `Quit` is queued.
- Command we'll use:
  ```
  "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "E:\Projects\Unreal\AutoAlignNodes\AutoAlignNodes.uproject" -ExecCmds="Automation RunTests GraphAutoLayout; Quit" -unattended -nullrhi -nosplash -nosound -log -ReportExportPath="E:\Projects\Unreal\AutoAlignNodes\Saved\Automation\GraphAutoLayout"
  ```
- Build:
  ```
  "C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" AutoAlignNodesEditor Win64 Development -Project="E:\Projects\Unreal\AutoAlignNodes\AutoAlignNodes.uproject" -WaitMutex
  ```

---

## 2. Module layout

```
Plugins/GraphAutoLayout/
  GraphAutoLayout.uplugin          (Plugins: Niagara enabled)
  Source/
    GraphAutoLayoutCore/           Type=Editor. Depends on Core ONLY. Pure algorithm.
    GraphAutoLayout/               Type=Editor. Adapters, trigger, commands, settings.
    GraphAutoLayoutTests/          Type=Editor. Automation tests (core + in-memory graphs).
```

- **GraphAutoLayoutCore:** `Core`. Having no Slate or UObject dependency enforces the "independent of Slate and graph type" rule at compile time.
- **GraphAutoLayout:**
  - Public: `Core`, `CoreUObject`, `Engine`.
  - Private: `GraphAutoLayoutCore`, `InputCore`, `Slate`, `SlateCore`, `UnrealEd` (EdGraph comments, MaterialGraph, transactions), `GraphEditor` (SGraphPanel/SGraphNode/SGraphPin), `BlueprintGraph` (K2 schema, `UK2Node::IsNodePure` at `BlueprintGraph/Classes/K2Node.h:244`, `UK2Node_Knot`), `DeveloperSettings`, `Niagara`, `NiagaraCore`, `NiagaraEditor`.
- **GraphAutoLayoutTests:** `GraphAutoLayoutCore`, `GraphAutoLayout`, `UnrealEd`, `BlueprintGraph`, `MaterialEditor` (`UMaterialEditingLibrary::CreateMaterialExpression`, `MaterialEditingLibrary.h:168`), `Engine`.
- The project's `.uproject` stays as is; plugins under `Plugins/` are picked up automatically. Everything is Editor-type, so there's no runtime cost.

## 3. Integration: trigger, focus, undo

### 3.1 Recommended: Slate input preprocessor (option a)
`FGraphAutoLayoutInputProcessor : IInputProcessor`, registered with `FSlateApplication::RegisterInputPreProcessor` (`Slate/Public/Framework/Application/SlateApplication.h:1544`). **Every handler returns `false`**, so it never consumes input.

State machine:
```
KeyDown(chord matches, !IsRepeat):
    if any mouse button currently pressed       -> Armed=false   (wire-drag + L spawns Lerp on keydown)
    else if FocusedGraphPanel() valid           -> Armed=true, Panel=weak ptr
KeyDown(other key) while Armed                  -> Armed=false   (user is doing some other chord)
MouseButtonDown while Armed                     -> Armed=false   (L+click = spawn Lerp; or select/drag)
KeyUp(chord key) while Armed:
    Armed=false
    if FocusedGraphPanel() == Panel             -> defer LayoutSelection(Panel) to next tick
FocusChanging / app deactivated                 -> Armed=false
```

`FocusedGraphPanel()` resolves from `FSlateApplication::GetUserFocusedWidget(0)`, and **the focused widget itself** must be one of:
- an `SGraphPanel`, or
- an `SGraphEditorImpl`/`SGraphEditor` (walk up to the `SGraphEditor`, then call `GetGraphPanel()`).

Text boxes, node-rename fields, search boxes and the details panel never qualify. Even when they sit *inside* a graph panel, the focused widget is then the `SEditableText`, not the panel. That's why we require an exact match instead of "has a graph-panel ancestor".

Running the layout on the next tick (instead of inside the KeyUp handler) keeps input routing light and makes sure Slate has finished its KeyUp bookkeeping, `LastKeyChordDetected` reset included.

### 3.2 Option b (per-editor command lists): rejected
- Commands fire on key **down**, so a release-only trigger isn't possible.
- When a graph is focused, L KeyDown would fire the command before the user has a chance to click, which breaks Lerp.
- There's no public, generic hook into the private `GraphEditorCommands` lists of FBlueprintEditor, FMaterialEditor and the Niagara toolkits. We would need a different hack per editor.

Option a covers every `SGraphEditor`-based editor with one mechanism.

### 3.3 Rebinding
- A `TCommands<FGraphAutoLayoutCommands>` context "Graph Auto Layout" with one `FUICommandInfo` `LayoutSelected`, default chord `L`. It shows up under Editor Preferences › General › Keyboard Shortcuts.
- We never map it to a command list. The preprocessor reads `GetActiveChord(Primary/Secondary)` (`UICommandInfo.h:207`) and matches against it.
- See Q1 for the alternative.

### 3.4 Executing a layout
1. Pick the adapter by `Graph->GetSchema()` class: K2 → Blueprint, `UMaterialGraphSchema` → Material, `UEdGraphSchema_Niagara` → Niagara. If the adapter is disabled in settings or there's no match, log it and do nothing.
2. Selection = `Panel->GetSelectedNodes()` cast to `UEdGraphNode`. If empty, do nothing; optionally show an `FSlateNotificationManager` toast.
3. The adapter builds an `FLayoutGraph`. Sizes and pin offsets come from widgets; when a widget isn't available, it falls back to an estimate.
4. Run the core, then `FScopedTransaction(LOCTEXT("AutoLayout","Auto Layout Nodes"))`. For each moved node, `Modify()`, set `NodePosX/Y`, then the adapter's `PostMove` (Material expression sync, etc.).
5. Comments: see §5.4.

### 3.5 Logging and debugging
- `DEFINE_LOG_CATEGORY(LogGraphAutoLayout)`.
- CVar `GraphAutoLayout.Debug` (0/1), also exposed as the `bDebugLogging` setting.
- Verbose output: adapter, node/edge count, islands, layers, dummy count, crossings before/after, time per phase, and skipped nodes with the reason (no widget → estimated size, comment, unsupported, etc.).

## 4. Core algorithm (`FGraphLayoutEngine`, GraphAutoLayoutCore)

Input:
- `FLayoutGraph`: nodes `{Id, Size, OrigPos, Pins[{Id, Side(In/Out), OffsetY, bPrimary}]}` and edges `{SrcNode, SrcPin, DstNode, DstPin, bPrimary}`.
- `FLayoutParams`: spacing, `LayeringMode` (FromSources/FromSinks), anchor policy.
- Obstacles: rectangles of unselected nodes.

Output: `TMap<Id, FVector2f>`.

Data always flows visually **left → right** in all three editors (outputs on the right, inputs on the left). "Material flows right→left" therefore means the *layering* starts at the sink, not that edges point leftward.

Steps, per **island**. Islands are connected components, found with union-find in O(V+E α).
1. **Cycle breaking.** DFS in a deterministic order (by original X, then Y); reverse back-edges. O(V+E). Ordering by original position means the result honours how the user already laid things out.
2. **Layering.**
   - `FromSinks` (Material, Niagara): longest path to any sink, so `layer = 1 + max(layer(consumers))`. Every edge then points right, and a node with several consumers sits just left of its furthest-left consumer. That handles heavy fan-in and fan-out without overlap.
   - `FromSources` (Blueprint exec): longest path from sources.
   - O(V+E).
3. **Long edges.** Insert dummy nodes (width 0, height = one pin row) on edges that span more than one layer. This lets crossing reduction and coordinate assignment route long wires *between* nodes instead of through them. Cost: O(total span).
4. **Crossing reduction.**
   - Initial order within each layer: by original Y.
   - Then 4–8 alternating sweeps of the **barycenter** heuristic using *pin-level* positions (pin offset, not just node index), so edges into upper pins sort higher. Ties are broken with the median.
   - Keep the best ordering seen.
   - O(sweeps · E log V).
5. **X coordinates.** Column width = widest node in the layer, plus `HorizontalSpacing`. FromSinks layers are right-aligned within their column (outputs line up toward the consumer). FromSources layers are left-aligned.
6. **Y coordinates (straight wires).** Iterative pin-to-pin alignment, 4–6 sweeps alternating direction:
   - Desired Y for each node = weighted median of `(neighbourPinY − ownPinOffset)` over its edges. Primary edges (exec / parameter-map / first input) get a much larger weight, so the spine is straight first.
   - Overlaps within a layer are resolved **without changing order**, using the minimal total squared displacement. This is the pool-adjacent-violators method, O(n) per layer, with `VerticalSpacing` between nodes.
   - O(sweeps · (V+E)). Simpler than Brandes–Köpf, and it handles variable node heights and real pin offsets directly.
7. **Anchor.** Translate the island so its anchor node keeps its original position (Q4).
8. **Islands.** Each island is anchored at its own original position. Overlaps between islands are then resolved by pushing later islands (sorted by original Y) down (Q5).
9. **Obstacles (unselected nodes).** Each island moves as a rigid body. We compute the smallest vertical offset (trying up and down) at which no island node intersects an obstacle inflated by spacing. Candidate offsets come from obstacle edges. Worst case O(I · N · M), with a simple grid bucketing if M is large. Unselected comment boxes are **not** obstacles.

Target performance: well under 50 ms for 500 nodes. The tests log timing.

## 5. Per-graph-type behaviour (adapters, `IGraphLayoutAdapter`)

```cpp
class IGraphLayoutAdapter {
  virtual bool    Supports(const UEdGraph&) const = 0;
  virtual FLayoutGraph Build(const TArray<UEdGraphNode*>& Selected, const FNodeMeasure& Measure) = 0;
  virtual void    Apply(const FLayoutResult&) = 0;   // Modify + write positions + sync
  virtual FName   GetName() const = 0;
};
```
`FNodeMeasure` wraps the optional `SGraphPanel` (real sizes) and falls back to estimates (headless/tests).

### 5.1 Blueprint (K2)
- Edge kinds: exec (`UEdGraphSchema_K2::PC_Exec`) is primary; data is secondary. Pure = `UK2Node::IsNodePure()`. A knot inherits its kind from its wires.
- **Two-level layout** (recommended, Q3):
  1. Assign each pure node to its **earliest** exec consumer (lowest exec layer). Lay out that exec node's pure-input tree with the core in FromSinks mode, with the exec node as the sink. The result is a *block*: the exec node plus its tree to the left, with the tree's top aligned to the first data input pin.
  2. Lay out exec nodes FromSources left→right, each exec node taking its block's footprint. The gap before a column grows to fit the trees hanging to its left. Branches and sequences stack vertically.
- A shared pure node feeding several exec nodes is owned by the first one; its other wires stay long edges. A selection with no exec nodes falls back to plain FromSinks.

### 5.2 Material
- FromSinks. Sinks are the root node (`UMaterialGraphNode_Root`) when selected. Otherwise they're the selected nodes with no selected consumers; in a material function, these are the `FunctionOutput` nodes.
- Material exec pins (`UMaterialGraphSchema::PC_Exec`, seen at `MaterialGraph.cpp:560`) are treated as primary if present.
- Persistence: as in §1.3. Composite (subgraph) nodes are treated as ordinary nodes. We lay out whatever graph is currently shown.

### 5.3 Niagara (Phase 3; verify again first)
- FromSinks, from `UNiagaraNodeOutput` (public). Parameter-map wires are primary, so the map chain InputMap→…→Set→Output forms the straight spine. Data is secondary.
- The InputMap node (`UNiagaraNodeInput` carrying a map) is forced to the leftmost layer of its island.
- Several output nodes (spawn/update/event usages) become separate islands.
- Overview graph: see Q9.

### 5.4 Comments, reroutes, auto-format (Phase 4)
- Comment nodes (`UEdGraphNode_Comment`, `UnrealEd/Public/EdGraphNode_Comment.h`; Material uses a subclass) are never fed to the core.
- Comment mode:
  - `Ignore`
  - `ResizeToFit` (default): before layout, record which selected nodes each comment contains (`GetNodesUnderComment()`, line 115, plus a bounds test). After layout, resize and move the comment to wrap those nodes plus padding. For Material, sync `SizeX/SizeY/MaterialExpressionEditorX/Y`.
- Reroutes: treated as small regular nodes by default (Q7).
- Auto-format on node placed: `UEdGraph::AddOnGraphChangedHandler` (`Engine/Classes/EdGraph/EdGraph.h:118`). On `GRAPHACTION_AddNode` in a graph whose editor is open, it formats the new node with its connected neighbours. Off by default. Detailed design comes in Phase 4.

## 6. Settings (`UGraphAutoLayoutSettings : UDeveloperSettings`)
Config `EditorPerProjectUserSettings`, under Editor Preferences › Plugins › Graph Auto Layout (Q10).

| Setting | Default |
|---|---|
| `HorizontalSpacing` | 80 |
| `VerticalSpacing` | 40 |
| `PureNodeSpacing` (BP) | 24 |
| `bEnableBlueprint`, `bEnableMaterial`, `bEnableNiagara` | true |
| `CommentMode` | ~~ResizeToFit~~ KeepTogether (§10) |
| `CommentPadding` | 30 |
| `RerouteMode` | Regular |
| `bAutoFormatOnNodePlaced` | false |
| `bShowNotifications` | true |
| `bDebugLogging` | false |

## 7. Test plan

**Core** (`GraphAutoLayout.Core.*`, synthetic graphs, real sizes given):
- No overlap: every pair of node AABBs is separated by at least spacing − ε.
- Direction:
  - Every non-reversed edge has `src.right < dst.left`.
  - BP exec chains increase in X.
  - Material: every node is left of its consumers, and the sink is rightmost.
- Cycles: a ring, nested loops and self-loops terminate, and the result has no overlaps.
- Islands: 3 disconnected components; island bounding boxes don't overlap.
- Anchor: the anchor node's position is unchanged to within 0.5 units.
- Obstacles: no laid-out node intersects a given obstacle rectangle.
- Edge cases: empty graph, single node, two nodes with no edges.
- Performance: a random layered DAG with 500 and 2000 nodes, timing logged. Assert < 250 ms for 500 nodes so CI isn't flaky; the expected time is far lower.
- Determinism: the same input twice gives identical output.

**Integration** (`GraphAutoLayout.Integration.*`, in-memory, headless, so the **estimated-size fallback** is used):
- Blueprint: `FKismetEditorUtilities::CreateBlueprint` (`UnrealEd/Public/Kismet2/KismetEditorUtilities.h:110`) in the transient package. Add an event → call → branch → calls chain, plus pure nodes. Run the adapter, then assert positions, exec X ordering, no overlap, and that undo (`GEditor->UndoTransaction()`) restores the original positions.
- Material: transient `UMaterial` + `UMaterialEditingLibrary::CreateMaterialExpression`; build the graph via `FBlueprintEditorUtils::CreateNewGraph(…, UMaterialGraph, UMaterialGraphSchema)` + `RebuildGraph()` (`MaterialGraph.h:200`). Assert `MaterialExpressionEditorX/Y == NodePosX/Y`, `Material->EditorX/Y` for the root, and that every expression is left of its consumer.
- Niagara (Phase 3): a transient `UNiagaraScript` with a module graph, if creation proves feasible headless; otherwise documented and covered by manual tests.

The editor UI path (widget sizes, the hotkey, the Lerp chord) is covered by a manual checklist each phase.

## 8. Risks and unknowns
1. **Stale pin offsets / sizes for off-screen nodes.** `SGraphPin::CachedNodeOffset` only updates on Tick. Mitigation: compute offsets from the pin's and node's cached paint geometry when they are fresh; otherwise estimate (header + row index × row height). Desired size comes from prepass, which is normally valid for all nodes. Zoomed-out LOD may change desired size; we'll test at several zoom levels and fall back to the max of measured and estimated if needed.
2. **L arming vs. other editors' L uses.** We only arm when a graph panel/editor is the exact focused widget, so Sequencer and UMG L bindings are unaffected.
3. **Material editor overwrite.** Mitigated by writing expressions ourselves and by the editor's own `LinkMaterialExpressionsFromGraph` sync. Phase 2 verifies save/reload.
4. **Niagara private node classes.** Handled via public pin-type checks and class-name lookup. Niagara graph layout quality is the least certain part, which is why Phase 3 starts with a structure investigation on real assets.
5. **Comments with group movement.** Laying out nodes doesn't keep a comment's members together; they may spread out and the comment is resized to fit. Comment-aware grouping is out of scope. *Superseded by §10 (Keep Together mode).*
6. **Hard Niagara dependency.** `.uplugin` → Niagara plugin `Enabled`. Niagara is on by default, but this makes the plugin require it.
7. **Not a git repo.** No commits are made, so there's no rollback safety net (see Q11).

## 8b. Scope change (2026-10-18, after Phase 2)
- **Niagara:** only **Scratch Pad** graphs (inside the System/Emitter editor). Standalone Niagara script assets (Module, Dynamic Input and Function editors) are **not** supported; L does nothing there.
- **Scratch Pad saving:** Scratch Pad edits a transient copy (`NiagaraScratchPadScriptViewModel.cpp:65`) and only raises "pending changes" or applies when the graph needs a recompile. After a layout we call the exported `UNiagaraNode::MarkNodeRequiresSynchronization(Reason, true)` (`NiagaraNode.h:181`) on one moved node, inside the transaction. This is behind the setting "Mark Niagara Scratch Pad changed after layout" (on by default).
- **Spine:** InputMap → Map Set → … → Output parameter-map wires are primary. Map Get wires are ordinary data.
- **Order:** Phase 3 = Scratch Pad; Phase 4 = comments, settings polish and auto-format-on-place; Phase 5 = Animation Blueprint; Phase 6 = PCG graphs. Each extra editor gets its own save-path investigation first, because several editors store node positions outside `UEdGraphNode`.

## 9. Decisions (approved)
1. Hotkey rebinding: a TCommands entry under Editor Preferences › Keyboard Shortcuts.
2. The release-to-trigger rule applies in every graph editor. No hold timeout.
3. Blueprint: exec spine left→right, with pure-input trees as blocks to the left of their owner.
4. Anchor: in BP, the leftmost exec source (event/entry); in Material/Niagara, the output/root node, else the rightmost sink.
5. Islands: *revised 2026-10-18*. With "Compact disconnected groups" on (the default), the main island (highest anchor priority, else the largest) stays anchored. The other islands are packed below it in reading order (top→bottom, then left→right) into rows that wrap to keep the block roughly square, with an `IslandSpacing` gap (default 80, i.e. 2× vertical spacing). The whole packed block then moves rigidly to clear unselected nodes. Pressing L again gives the same result. With the setting off, the original behaviour applies: each island stays near its original position and overlapping islands are pushed down.
6. Unselected nodes: the island moves rigidly up or down, whichever is the smaller move.
7. Reroutes: treated as small regular nodes. No collapse option in Phases 1–3.
8. Comments: ResizeToFit by default (Phase 4). *Revised 2026-10-19: Keep Together is the default (§10).*
9. Niagara System Overview: skipped.
10. Settings are per-user (EditorPerProjectUserSettings).
11. git initialised; one commit per phase.

---

## 10. Addendum: keep comment groups together (2026-10-19, draft for approval)

**Problem.** Today every selected node is laid out on its own and comments are resized afterwards. A comment's nodes can end up interleaved with outside nodes, so the resized comment covers nodes that don't belong to it.

**Idea.** Lay out each comment's nodes first, as a block that includes the comment's border and title. The rest of the layout then treats that block as one big node. Afterwards the comment is set to exactly the block's frame. Nothing from outside can land under it, because the layout already keeps normal spacing around every node.

### 10.1 Decisions (user, 2026-10-19: all recommended answers)
1. **Which comments are groups:** comments whose Move Mode is *Group Movement* (Unreal's default, `Editor/UnrealEd/Public/EdGraphNode_Comment.h:36`) that hold at least one laid-out node, whether or not the comment itself is selected. For Material comments the mode comes from `bGroupMode` (`UnrealEd/Private/MaterialGraphNode_Comment.cpp:148`). Comments in plain *Comment* mode keep today's resize-only behaviour.
2. **Nested and overlapping comments:** nested comments become groups inside groups. Each node belongs to the **smallest** Group Movement comment that contains its centre. A comment's parent is the smallest larger comment that contains the comment's centre. After layout, comments are either cleanly nested or apart.
3. **Partly selected comments:** only the selected nodes form the group. Unselected nodes in the comment stay put as obstacles, and the comment is resized to wrap both. **If the comment itself is selected** (title clicked, or a marquee touched its title bar, `GraphEditor/Private/SGraphNodeComment.cpp:518`), all nodes inside it are laid out too, like dragging the comment. So "select a comment, press L" tidies that comment.
4. **Setting:** a third *Comment Mode* value, **Keep Together**, is the new default. The choices are Ignore, Resize To Fit (unchanged Phase 4 behaviour) and Keep Together.
5. **Wires across the group boundary:** from outside, a group behaves like one node whose pins are its members' pins at their real heights, so today's straightening rules apply (exec and parameter-map wires first). Inside the group, a node wired from outside goes to the group's left edge, and a node wired to outside goes to its right edge. That only happens when the node has no in-group wire on that side. A Blueprint pure node outside a comment that feeds a node inside hangs to the left of the whole comment.
6. **Inside a group:** the usual rules apply (pure-node trees, main flow line, anchors). Disconnected parts are packed inside the comment when "Compact disconnected groups" is on.

Auto Place New Nodes is unchanged.

### 10.2 Core (`GraphAutoLayoutCore`)
New input:
```cpp
struct FLayoutGroup
{
    int32 Parent = INDEX_NONE;                       // enclosing group, INDEX_NONE = top level
    FVector2f PaddingMin = FVector2f::ZeroVector;    // left, top (top includes the title bar)
    FVector2f PaddingMax = FVector2f::ZeroVector;    // right, bottom
};
FLayoutNode::Group = INDEX_NONE;                     // innermost group the node belongs to
FLayoutInput::Groups;                                // TArray<FLayoutGroup>
FLayoutResult::GroupBounds;                          // TArray<FBox2f>: final frame per group (invalid box if empty)
```
With no groups, `Run` behaves exactly as today. With groups, the steps are:
1. **Validate.** A bad group index, or a parent chain with a cycle, puts the node or group at the top level.
2. **Collapse bottom-up**, deepest groups first, with ties going to the lower index. For group G:
   - **Units:** the nodes directly in G, plus one *super node* per child group that has content.
   - **Run** today's pipeline on those units and the edges between them, with no obstacles. Edges inside one child group are already handled inside it.
   - **Frame:** the bounds of the units, grown by G's padding.
   - **Super node:**
     - Size = frame. OrigPos = frame min. The inner run anchors at original positions, so the inner anchor node keeps its spot if this group becomes the outer anchor.
     - Pins = every member pin with its offset from the frame, flattened through nested groups.
     - AnchorPriority = max of the members.
     - Satellite only if all members are satellites.
     - SatelliteMinTop = for members entered by a primary wire from outside G, the largest (member offset + its SatelliteMinTop).
     - LayerConstraint = First or Last if the members require only that one, else None.
   - **Boundary rule (decision 5):** before running G, a unit with incoming wires from outside G and no in-group predecessors gets `First`. Otherwise, a unit with outgoing wires to outside G and no in-group successors gets `Last`. The adapter's own constraint wins. The engine already honours First and Last only in exactly these cases (`AssignLayers`).
3. **Top level:** the ungrouped nodes plus the top-level super nodes are laid out with the real obstacles. Island packing and obstacle avoidance then move whole frames.
4. **Expand:** node position = unit position + offset. `GroupBounds[g]` = the frame at its final position.

Cost: one extra pipeline run per group over that group's units only, so total work stays about the same as one flat run.

### 10.3 Runner (`GraphAutoLayoutRunner.cpp`)
Keep Together mode only. Resize To Fit and Ignore keep their current code paths.
1. **Expand the selection (decision 3).** Each selected Group Movement comment adds every non-comment node whose centre is inside it.
2. **Comment tree.** Over all Group Movement comments: parent = the smallest comment with a larger area (ties: lower graph index) that contains this comment's centre. Each non-comment node, laid out or not, is assigned to the smallest comment containing its centre. A comment is a **group** if its subtree holds at least one laid-out node; the group tree follows the comment tree.
   - **Membership** is decided by position only. `GetNodesUnderComment()` is not used here, because it is only refreshed when the comment is selected or resized (`SGraphNodeComment.cpp:314`) and can be stale.
   - **Padding** = `CommentPadding` on all sides, plus the existing title estimate `FontSize * 1.5 + 12` on top.
3. **Resize each group comment, deepest first.** Final rect = group frame, unioned with each unselected node directly in it (grown by padding and title) and each child comment's final or unchanged rect (grown the same way).
   - The resize goes through `PreModifyComment` and `ResizeNode`, as today, so Material comment expressions stay in sync and undo covers everything in one transaction.
   - Comments that aren't groups are not touched.
4. **Plain-Comment-mode boxes** that hold laid-out nodes are resized with today's `CollectCommentsToFit` rule.
5. The debug log adds the group count and nesting depth.

Nothing is adapter-specific. Every supported graph type (Blueprint, Anim BP, Material/Material Function, Niagara Scratch Pad, PCG) goes through the same runner path. PCG comments are `UEdGraphNode_Comment` subclasses (`Plugins/PCG/Source/PCGEditor/Private/Nodes/PCGEditorGraphNodeComment.h:13`).

### 10.4 Settings
`EGraphAutoLayoutCommentMode`: `Ignore`, `ResizeToFit`, `KeepTogether` (display name "Keep Together"). The default becomes `KeepTogether`. `CommentPadding` is editable unless the mode is Ignore. Anyone who explicitly saved Resize To Fit keeps it.

### 10.5 Tests
**Core** (`GraphAutoLayout.Core.Groups.*`), each checking: no node overlaps; every member is inside its frame by at least the padding; no non-member is within spacing of a frame; nested frames sit inside their parent frame.
- *NoInterleave:* two exec chains whose ungrouped layout interleaves, plus a group made of a sub-chain.
- *SplitGroup:* a group {B, D} on the chain A→B→C→D→E, which makes a cycle through the block. C must stay outside the frame.
- *Nested:* a group inside a group.
- *Boundary:* the entry node sits at the frame's left edge and the exit node at its right edge.
- *Satellites:* a pure node outside feeding an exec node inside sits left of the frame.
- *RandomGroups:* random DAGs with random (nested) groups, several seeds, plus determinism.

**Integration** (`GraphAutoLayout.Integration.CommentGroups.*`, headless, nodes placed far from the origin because of the ghost BP event nodes):
- *Blueprint:* chains set up so the old behaviour would interleave. Checks: the comment rect contains no non-member node, wraps its members with padding, no overlaps; undo restores every node and the comment rect, and redo works.
- *Blueprint partial and selected comment:* a partly selected comment wraps both its moved group and the unselected node. Selecting only a comment lays out its contents.
- *Blueprint nested:* the inner comment is inside the outer one; both wrap their members.
- *Material:* a comment group, with the comment expression's position and size synced and undone correctly.
- *Niagara Scratch Pad and Anim graph:* one comment group each, using the existing headless setups.
- PCG has no headless test (see Phase 6), so it's covered by the manual checklist.
- The existing `CommentResize` and `MaterialComment` tests keep pinning Resize To Fit mode.

### 10.6 Limitations (accepted)
- A partly selected comment can still stretch when its group ends up away from its unselected nodes (decision 3a).
- Plain-Comment-mode boxes nested inside a group comment may poke out of the parent comment by about one padding.
- The title height is an estimate. A very long title that wraps to several lines can overlap the top nodes; that's unchanged from Phase 4.
- Comments that hold no laid-out nodes are still not obstacles, so laid-out nodes can land inside an unrelated comment's empty area, as today. Possible follow-up.
- A wire from outside to a member deep inside a group crosses the group's left part when the boundary rule can't apply (the member has in-group wires on that side).

### 10.7 Builder's calls (not asked; flagged for review)
- Membership is by node centre, and the engine's cached "nodes under comment" list isn't used for grouping.
- Super-node OrigPos = frame min of the anchored inner layout, so anchors still hold through groups.
- SatelliteMinTop of a group follows its entry exec pins only.
- Ties in the comment tree are broken by area, then graph index.
- The Niagara and Anim comment-group checks are extra sections at the end of the existing `NiagaraScratchPad` and `AnimGraph` tests, so they reuse those tests' graph setup. The Material check is its own test, `CommentGroups.Material`.

### 10.8 Implementation status and manual test checklist (2026-10-19)
**Implemented.** The core is in `GraphLayoutEngine.cpp` (`RunGrouped`, `RunFlat`); the runner is in `GraphAutoLayoutRunner.cpp` (`BuildCommentGroups`, `FitGroupComments`).

**Automated.** The GraphAutoLayout suite passes 34/34:
- `Core.Groups.*`: NoInterleave, SplitGroup, Nested, Boundary, Satellites, Random, Validation.
- `Integration.CommentGroups.*`: Blueprint (with a Resize To Fit control, undo/redo, and a check that pressing L again changes nothing), PartialAndSelected, Nested, Material.
- Comment-group checks in `Integration.NiagaraScratchPad` and `Integration.AnimGraph`.
- Performance: 500 nodes in 40 nested groups (depth 5) lay out in about 8 ms.

**Manual checklist (in the editor; please report what fails, with a screenshot if possible):**
1. **Setting.** In Editor Preferences › Plugins › Graph Auto Layout › Comments, Comment Mode should read **Keep Together**. If it reads Resize To Fit, an older saved preference is overriding the new default; switch it to Keep Together for these tests.
2. **No more interleaving (Blueprint).**
   - Setup: a Sequence with three outputs, each going to a Print String. Draw a comment around the 1st and 3rd Print String only.
   - Select everything and press L.
   - Expected: the comment holds only those two nodes, with room for its title. The middle Print String sits outside the comment, and no node is under the comment.
3. **Undo/redo.** Ctrl+Z restores every node and the comment in one step. Ctrl+Y redoes it.
4. **Stable.** Pressing L again on the result moves nothing.
5. **Select a comment.** Click only a comment's title bar and press L. Everything inside it is tidied and the comment wraps the result.
6. **Partly selected comment.** Select some of a comment's nodes but not all, and press L. The selected nodes move as one block, the unselected ones stay where they are, and the comment still wraps both.
7. **Nested comments.** Put a comment inside another and press L. The inner comment wraps its nodes, and the outer comment wraps the inner comment plus its own nodes. Neither overlaps an outside node.
8. **Plain comment mode.** On a comment, set Details › Move Mode = "Comment". Its nodes are no longer grouped and the comment is only refitted (Phase 4 behaviour). Selecting only that comment and pressing L gives "Nothing to lay out".
9. **Pure nodes (Blueprint).** A pure node outside a comment that feeds an exec node inside it hangs to the left of the comment, below the exec wire.
10. **Material and Material Function.**
    - Setup: a comment around some of a node's inputs.
    - Press L. The comment wraps only its own nodes, and the other inputs sit outside it.
    - Save, close and reopen. Nodes and comment are where L put them.
11. **Niagara Scratch Pad.**
    - Setup: a comment around two Map Set nodes.
    - Press L. They stay together inside the comment.
    - Apply, close and reopen. The layout is kept.
12. **Animation Blueprint.** A comment around two sequence players in an AnimGraph keeps them together.
13. **PCG** (no headless test exists).
    - Setup: a comment around a few nodes.
    - Press L. They stay together inside the comment.
    - Save and reopen. The comment's size and position are kept.
14. **Zoom.** Repeat item 2 zoomed far out, then zoom in. Nothing overlaps.
15. **Other modes.** Resize To Fit gives the old behaviour (the comment may cover other nodes again). Ignore leaves comments untouched.
16. **Debug log** (optional). With Debug Logging on, the LogGraphAutoLayout line includes "N comment groups (depth D)".

**Known limitations** (in addition to §10.6):
- In Niagara Scratch Pad, a layout that only refits comments (no node moves) doesn't mark the graph changed, so Apply may not keep that comment-only change. This existed in Phase 4, but Keep Together makes it more common.
