# Graph Auto Layout

**Select nodes, press L, and they're laid out cleanly** — Houdini-style auto layout for Unreal Engine 5.8 node graphs.

Instead of dragging nodes around by hand, select a messy part of a graph (or Ctrl+A for all of it), press **L**, and the plugin arranges it into tidy columns that read left to right, with wires as straight as possible and nothing overlapping. One Ctrl+Z puts everything back.

It's editor-only: it adds nothing to your game, cooked builds or runtime.

---

## Where it works

| Graph | What stays put | How it's arranged |
|---|---|---|
| **Blueprint** (Event Graph, functions, macros) | The event / function entry node | Exec wires form a straight line left to right. Pure nodes (math, getters) hang as small trees below and to the left of the node that uses them. |
| **Animation Blueprint** (AnimGraph, states, transition rules, conduits) | Output Pose / State Result / Transition Result | Pose wires form the main line into the result. Variables and math feeding pins like Alpha hang below the anim node they drive. |
| **Material** and **Material Function** | Material output node / Function Outputs | Everything flows into the output from the left. A node used by several others sits just left of the earliest one. |
| **PCG** | The graph's Input node | Data flows right from the Input node; the Output node goes in the right-most column. |
| **Niagara System Overview** | The System node | There are no wires here, so the System node and its emitters are lined up in one row, in the order of the system's emitter list, with their tops aligned. |
| **Niagara Scratch Pad** | The Output node | The parameter map chain (InputMap → Map Set → Output) is the straight main line; Map Get nodes sit near what they feed. |

Not supported (pressing L shows a short message instead): standalone Niagara script assets (Module / Dynamic Input / Function editors), the Animation state machine overview, and other editors such as Behavior Trees.

---

## How to use it

### Lay out nodes
1. Click on an empty part of the graph so the graph has focus.
2. Select the nodes to tidy. **Ctrl+A** selects the whole graph.
3. Press and release **L**.

You can also **right-click a selected node → Auto Layout Selected**.

- Only selected nodes move. Unselected nodes stay exactly where they are, and the laid-out nodes are kept clear of them.
- If nothing is selected, nothing happens (a small message tells you to select nodes).
- The whole layout is **one undo step**: Ctrl+Z restores every node and comment.

### L still spawns Lerp
In the Material Editor and Niagara, **holding L and clicking** spawns a Lerp node, as normal. The layout only runs when you **press and release L without clicking**, so the two never clash. Dragging a wire and pressing L also still spawns a connected Lerp.

### Groups that aren't connected
If the selection contains separate groups (or loose single nodes), the main group stays put and the others are packed into neat rows underneath it. Turn this off with **Compact Disconnected Groups** to leave each group near where it was.

### Comment boxes
By default (**Keep Together**), the nodes inside a comment box are laid out as one group and the comment is resized around them, so other nodes never end up inside it. Nested comments work too, and selecting just a comment tidies everything inside it.

- This applies to comments set to **Group Movement** (the usual default for comments).
- Comments set to plain **Comment** movement are only resized to fit their nodes.
- Set **Comment Mode** to *Resize To Fit* (resize only) or *Ignore* (leave comments alone).

### Auto-place new nodes (optional, off by default)
Turn on **Auto Place New Nodes** and, when you add a node that gets wired to an existing one (for example by dragging off a pin), the new node snaps next to it with a straight wire, clear of other nodes. Existing nodes never move, and pasting several nodes is ignored. One Ctrl+Z puts the new node back where you dropped it.

### Change the key
**Editor Preferences → General → Keyboard Shortcuts → Graph Auto Layout → Layout Selected Nodes.**

---

## Settings

**Editor Preferences → Plugins → Graph Auto Layout.** Settings are per user and apply immediately.

| Setting | Default | What it does |
|---|---|---|
| Show Notifications | On | Small message when L does nothing (no selection, unsupported or read-only graph). |
| Horizontal Spacing | 80 | Gap between columns. |
| Vertical Spacing | 40 | Gap between nodes in the same column. |
| Pure Node Horizontal / Vertical Spacing | 40 / 20 | Blueprint only: tighter spacing inside pure-node trees. |
| Compact Disconnected Groups | On | Pack separate groups into rows under the main group. |
| Group Spacing | 80 | Gap between packed groups. |
| Comment Mode | Keep Together | Keep Together / Resize To Fit / Ignore (see above). |
| Comment Padding | 30 | Space between a comment's border and its nodes. |
| Auto Place New Nodes | Off | Snap newly wired nodes into place. |
| Enable for … Graphs | On | Turn the plugin off per graph type. |
| Mark Niagara Scratch Pad Changed After Layout | On | Needed so Scratch Pad's Apply keeps the new positions (the scratch script recompiles on Apply, like after any edit). |
| Debug Logging | Off | Detailed output in the log (same as the console command `GraphAutoLayout.Debug 1`). |

---

## Saving

Layouts are saved with the asset like any normal edit. A few editors work on a copy, and the plugin handles that for you:
- **Material Editor:** the material is marked as changed, so Save or Apply keeps the layout.
- **Niagara Scratch Pad:** the scratch script is marked as changed, so **Apply** keeps the layout (see the setting above).
- **PCG:** positions are written to the PCG nodes themselves, so they're saved with the graph.

---

## Tips

- **Lay out at normal zoom when you can.** Zoomed far out, Unreal draws simplified nodes, so the plugin uses the sizes it remembers from closer zoom levels (or an estimate for nodes it has never seen up close). Pressing L once at normal zoom makes later layouts at any zoom accurate.
- **Press L again** on an already tidy selection and nothing moves.
- **Partial selections** are a good way to tidy just one branch without disturbing the rest of a big graph.

## Known limitations

- Only one incoming wire per node can be perfectly straight; nodes with several inputs get some angled wires.
- Nodes without a wire between them — Save/Use Cached Pose, Named Reroutes — are treated as separate groups.
- A comment title that wraps onto several lines can overlap the nodes at the top of the comment.
- In Niagara Scratch Pad, a layout that only resizes comments (no nodes moved) may not light up Apply.
- PCG and Scratch Pad graphs are recognised by Unreal's internal class names; a future engine version could rename them, in which case L shows "doesn't support this graph type" until the plugin is updated.

## Troubleshooting

| Problem | Try |
|---|---|
| L does nothing | Click an empty spot in the graph first (the graph needs keyboard focus), make sure nodes are selected, and check the graph type is enabled in settings. |
| L does nothing while typing | Intended: it's ignored in text fields, search boxes and the Details panel. |
| Nodes overlap after zooming in | You laid out while zoomed far out on nodes never seen up close; press L again at normal zoom. |
| Something looks wrong | Run `GraphAutoLayout.Debug 1` in the console, press L again, and check `LogGraphAutoLayout` lines in `Saved/Logs`. |
| L feels slow | With `GraphAutoLayout.Debug 1` on, each press logs a `Timing:` line showing where the time went (measure, layout, apply, comments, finish). |

---

**Requirements:** Unreal Engine 5.8, Windows 64-bit. Depends on the Niagara and PCG plugins (both on by default in Unreal).
