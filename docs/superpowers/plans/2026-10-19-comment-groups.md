# Keep Comment Groups Together — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In the new default Comment Mode "Keep Together", pressing L lays out each Group Movement comment's nodes as one block that includes the comment frame, so comments never interleave with or cover unrelated nodes.

**Architecture:** The core (`GraphAutoLayoutCore`) gains optional nested groups. Each group is collapsed bottom-up: its direct members are laid out with today's flat pipeline, then the result becomes one rigid "super node" with all member pins. The runner builds the group tree from comments, pulls in the contents of selected comments, and resizes each group comment to its frame.

**Tech Stack:** UE 5.8.3 editor plugin (C++), automation tests (`IMPLEMENT_SIMPLE_AUTOMATION_TEST`).

**Spec:** `docs/specs/2026-10-18-graph-auto-layout-design.md` §10 (approved 2026-10-19).

## Global Constraints
- No new module dependencies. The core still depends on `Core` only.
- With no groups, `FGraphLayoutEngine::Run` must give byte-identical results to today (existing tests pin this).
- Resize To Fit and Ignore behave exactly as before. The existing `CommentResize` and `MaterialComment` tests pin Resize To Fit explicitly.
- Every engine API used must be verified in `C:\Program Files\Epic Games\UE_5.8\Engine\Source` or `\Plugins`, with the file cited in code comments where it isn't obvious.
- Before each build, check that `UnrealEditor*` isn't running.
- Integration tests place their nodes far from the origin, because of the ghost Blueprint event nodes near it.
- Build: `Build.bat AutoAlignNodesEditor Win64 Development -Project=... -WaitMutex -NoHotReloadFromIDE`. Tests: `UnrealEditor-Cmd.exe ... -ExecCmds="Automation RunTests GraphAutoLayout; Quit" ... -ReportExportPath=Saved/Automation/GraphAutoLayout`; results are in `index.json`.
- A build/test cycle takes about 5–10 minutes, so each task is built once with its tests and code together. The usual "watch it fail first" run is skipped; each test asserts a control case where that's practical.

## Review Focus
1. **A comment that holds only unselected nodes and a nested comment with selected nodes:** the outer comment must still wrap its unselected nodes and the inner comment. Pinned in Task 3 (`CommentGroups.PartialAndSelected`).
2. **A selected plain Comment-mode box:** it must not pull in its contents. Selecting only that box still gives "Nothing to lay out". Pinned in Task 3.
3. **Identical comment boxes stacked on top of each other:** the tree must be deterministic and acyclic, both must wrap the node, and the later one sits inside the earlier one. Pinned in Task 3 (`CommentGroups.Nested`).
4. **A bad group index, a parent cycle, or an empty group in the core input:** no crash, an invalid bounds result for the empty group, and the other groups still respected. Pinned in Task 1 (`Core.Groups.Validation`).
5. **Undo after a selected comment pulled in unselected nodes:** every pulled-in node and the comment are restored. Pinned in Task 3.

---

### Task 1: Core nested groups

**Files:**
- Modify: `GraphAutoLayoutCore/Public/GraphLayoutTypes.h` (FLayoutGroup, FLayoutNode::Group, FLayoutInput::Groups, FLayoutResult::GroupBounds, FLayoutStats::NumGroups and MaxGroupDepth)
- Modify: `GraphAutoLayoutCore/Private/GraphLayoutEngine.cpp`: move today's `Run` body into `LayoutPrivate::RunFlat`, add `LayoutPrivate::RunGrouped`; `Run` dispatches and times.
- Modify: `GraphAutoLayoutCore/Public/GraphLayoutEngine.h` (doc comment only)
- Test: `GraphAutoLayoutTests/Private/GraphLayoutGroupTests.cpp` (new)

**Interfaces (produces):**
```cpp
struct FLayoutGroup { int32 Parent = INDEX_NONE; FVector2f PaddingMin = FVector2f::ZeroVector; FVector2f PaddingMax = FVector2f::ZeroVector; };
int32 FLayoutNode::Group = INDEX_NONE;
TArray<FLayoutGroup> FLayoutInput::Groups;
TArray<FBox2f> FLayoutResult::GroupBounds;   // == Input.Groups.Num(); bIsValid == 0 for groups without members
int32 FLayoutStats::NumGroups, FLayoutStats::MaxGroupDepth;
```

- [ ] **Step 1: Write the tests** (`GraphAutoLayout.Core.Groups.*`). Shared checker `CheckGroups(Test, Input, Result, HSpace, VSpace)` returns the number of violations:
  - every member (a node whose group chain contains g) lies inside `GroupBounds[g]` shrunk by its padding (tolerance 0.5);
  - every non-member node is not within (HSpace, VSpace) of the frame, using the same predicate as `CountOverlaps`;
  - a child frame lies inside its parent frame shrunk by the parent's padding;
  - frames of unrelated groups don't come within spacing of each other.

  The tests:
  - `NoInterleave`: FromSinks sink S with three inputs fed by X (top), Y (middle) and Z (bottom). X and Z have one source each; group = {X, Z} with padding (30, 30+30)/(30, 30). Control: an ungrouped run puts Y inside the bounds of {X, Z}. Grouped run: `CheckGroups == 0`, `CountOverlaps == 0`.
  - `SplitGroup`: FromSources primary chain A→B→C→D→E with group {B, D}. Checks: no crash, `CheckGroups == 0`, C outside the frame.
  - `Nested`: chain A→B→C→D→E plus a lone F. Outer group = {B, D, F}, inner group = {C} (parent outer). B has AnchorPriority 10. Checks: `CheckGroups == 0`, B keeps its `OrigPos` (±0.5), `Stats.NumGroups == 2`, `Stats.MaxGroupDepth == 2`.
  - `Boundary`: (a) FromSources group {G1→G2→G3, G1→K} where K feeds an outside node T: `K.X == G3.X`. (b) FromSinks group {H1→H2→H3} where H3 feeds an outside sink O, and J (fed by outside source U) feeds H3: J's right edge == H1's right edge.
  - `Satellites`: FromSources; event E (AnchorPriority 10, exec out) → X (exec, inside group). Satellite P outside feeds X; satellite Q inside feeds X. Checks: P's right edge ≤ frame.Min.X − (SatelliteHorizontalSpacing − 0.5), Q inside the frame, `CountOverlaps` with satellite spacing == 0.
  - `Random`: seeds 1..5, `MakeRandomDag(150, seed, mode)` alternating modes, with 8 random groups (random earlier parent or none) and 60% of nodes assigned. Checks: `CheckGroups == 0`, `CountOverlaps == 0`, two runs give identical positions and bounds, and 500 nodes / 40 groups finish in under 250 ms.
  - `Validation`: groups {0: parent 1, 1: parent 0, 2: parent 7 (invalid), 3: no members}, one node with Group = 99. Checks: no crash, `GroupBounds.Num() == 4`, `!GroupBounds[3].bIsValid`, `CheckGroups == 0` (the checker uses the validated parents: cycle cut where it closes, invalid → top level).

- [ ] **Step 2: Implement.** `RunGrouped` follows spec §10.2 exactly:
  - validate (cut a cycle at the node where it closes);
  - file edges under the deepest common group;
  - set ExtIn/ExtOut flags per unit (node at level `NodeGroup[n]`, child group at level `Parent[c]`) and record primary-entry nodes per group;
  - collapse deepest-first (ties to the lower index), with direct node units first in input order and then child groups in index order;
  - boundary rule only where the unit's own constraint is None;
  - super node = frame size, OrigPos = frame min, flattened pins (`NodePinBase`), max AnchorPriority, satellite if all members are, First/Last if the members require only that one, and SatelliteMinTop from the primary entries;
  - top level with the real obstacles, then expand.

  Stats: NumIslands from the top level; NumBlocks, NumDummies, NumReversedEdges and crossings summed; MaxLayers as the max.

### Task 2: Setting + runner Keep Together path

**Files:**
- Modify: `GraphAutoLayout/Public/GraphAutoLayoutSettings.h`: `KeepTogether UMETA(DisplayName = "Keep Together")`, default `CommentMode = KeepTogether`, `CommentPadding` EditCondition `CommentMode != EGraphAutoLayoutCommentMode::Ignore`.
- Modify: `GraphAutoLayout/Public/GraphAutoLayoutRunner.h`: add `int32 NumCommentGroups = 0;` to `FGraphAutoLayoutRunResult`.
- Modify: `GraphAutoLayout/Private/GraphAutoLayoutRunner.cpp`

**Interfaces:** consumes Task 1 types. Runner-private helpers:
```cpp
struct FCommentGroup { UEdGraphNode_Comment* Comment; FBox2f Rect; double Area; int32 Parent; int32 Depth; int32 LayoutGroup; TArray<const UEdGraphNode*> Unselected; };
static TArray<FCommentGroup> BuildCommentTree(UEdGraph& Graph);                 // Group Movement comments only
static int32 FindInnermostComment(const TArray<FCommentGroup>&, FVector2f Point);
static float CommentTitleHeight(const UEdGraphNode_Comment&);                   // FontSize * 1.5f + 12.f (existing estimate)
static bool SetCommentRect(const IGraphLayoutAdapter&, UEdGraphNode_Comment&, int32 X, int32 Y, int32 W, int32 H);
```

- [ ] **Step 1:** Selection expansion: in Keep Together mode, each selected Group Movement comment adds the non-comment nodes whose measured centre lies inside it.
- [ ] **Step 2:** After building the input, Keep Together runs `BuildCommentTree` and assigns every node in `OrigBounds` to its innermost comment. A laid-out node gets `FLayoutNode::Group`; an unselected node goes into `FCommentGroup::Unselected`. Comments with laid-out content, plus all their ancestors, become `Input.Groups` with padding (`CommentPadding`, `CommentPadding + title`) / (`CommentPadding`, `CommentPadding`). Plain Comment-mode boxes go through `CollectCommentsToFit` (add a `bPlainCommentsOnly` filter).
- [ ] **Step 3:** After layout, resize the group comments deepest-first. Rect = `GroupBounds`, unioned with each grown unselected direct node and each grown child comment rect (final rect for groups, original otherwise). Use FloorToInt for the min and CeilToInt for the max, applied via `SetCommentRect`. The existing resize loop also switches to `SetCommentRect`, keeping its rounding.
- [ ] **Step 4:** Add `NumCommentGroups` and the group count/depth to the debug log line.

### Task 3: Integration tests

**Files:**
- Create: `GraphAutoLayoutTests/Private/GraphLayoutCommentTestUtils.h`. Move `AddComment`, `CommentBox`, `FCommentState`, `StateOf` and `TSettingOverride` here from `GraphLayoutPhase4Tests.cpp`. Add `CountNodeOverlaps(Result, H, V)` and `WrapsWithPadding(CommentBox, NodeBox, Padding, TitleHeight)`.
- Create: `GraphAutoLayoutTests/Private/GraphLayoutCommentGroupTests.cpp`
- Modify: `GraphLayoutPhase4Tests.cpp` (include the header), and the Material, Niagara and Anim test files only if helpers have to be shared.

- [ ] **Step 1:** `GraphAutoLayout.Integration.CommentGroups.Blueprint`. Setup at origin (20000, 20000): event E → Sequence with three outputs (`InsertPinIntoExecutionNode(GetThenPinGivenIndex(1), EPinInsertPosition::After)`, `K2Node_ExecutionSequence.h:66`) → X, Y, Z PrintString nodes, with a comment around X and Z only.
  - Control in Resize To Fit mode: the comment covers Y's centre. Then undo.
  - Keep Together:
    - the comment wraps X and Z with padding and title;
    - it doesn't intersect E, Seq or Y;
    - no node overlaps;
    - `NumCommentGroups == 1`.
  - Undo restores every node and the comment; redo works.
- [ ] **Step 2:** `CommentGroups.PartialAndSelected`:
  - **Partial comment:** E→X→Y with a comment around X and an unselected U. The comment wraps X and U, and U doesn't move.
  - **Selected comment:** select only comment C2 around unselected M1→M2. Both move (M2 right of M1, exec wire level), C2 wraps them, and `NumLaidOut == 2`. Undo restores M1, M2 and C2.
  - **Plain Comment-mode box selected alone:** gives "Nothing to lay out".
  - **Outer comment with an unselected node and an inner comment holding the selected node:** the outer comment wraps both.
- [ ] **Step 3:** `CommentGroups.Nested`: E→A→B→C, with an outer comment around A, B and C and an inner one around B. Checks: inner inside outer, each wraps its nodes, E outside. Then add two identical comments around one node: both wrap it, and the later one is inside the earlier one.
- [ ] **Step 4:** `CommentGroups.Material`: Lerp fed by ColorA → A, ColorB → B and Alpha → Alpha, with a comment around ColorA and Alpha.
  - Control in Resize To Fit mode: ColorB is covered.
  - Keep Together: ColorB is outside the comment, the comment expression's X/Y/SizeX/SizeY match the node, and undo restores the expression size.
- [ ] **Step 5:** `CommentGroups.NiagaraScratchPad` and `CommentGroups.AnimGraph`: the existing headless setups, with a comment around two interior nodes. It wraps them and contains no other node.

### Task 4: Verify, document, commit
- [ ] Check that no editor is running, build, then run all GraphAutoLayout tests. Expected: every test in `index.json` passes, with 23 existing plus the new ones.
- [ ] Update the design doc §10 status, add a "Comment groups" manual test checklist section to the spec, and update the project memory.
- [ ] Commit the spec, the plan and the code: "Keep comment groups together during layout".
