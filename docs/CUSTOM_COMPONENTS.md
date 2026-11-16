# HCS Custom Components (Phase 5.4)

## Scope and architecture

Custom components are editor-layer definitions, not new simulator opcodes. A definition contains a private `CircuitEditor` graph built from the existing built-in components and optionally other custom-component instances. A root `.hcs` document stores only a custom instance's stable `definition` ID and its ordinary instance position/connections.

`CircuitRuntime::synchronize(editor, library)` validates the hierarchy, expands it into a temporary flat `CircuitEditor`, and passes that graph to the existing `CircuitRuntime::rebuild` path. `rebuild` uses the normal `CircuitBuilder` and `hcs::Simulator`. No custom Boolean evaluator or second simulation engine is present. Each occurrence receives separately allocated flat component and signal IDs, so two occurrences of one DFF-based definition have independent sequential state.

The existing public simulator API and six runtime backend choices are unchanged. The expansion maps root-level pins back to the flattened signals. Internal-to-root debugging/inspection maps are not yet exposed as a public inspection API.

## Definition file format

The reusable definition extension is **`.hcscomponent`**, schema **`HCSC 1`**. It is a UTF-8, line-oriented text header followed by the existing `.hcs` version-1 internal circuit after the `implementation` line.

```text
HCSC 1
id arithmetic.half-adder
name "Half Adder"
description "Adds two one-bit values and exposes Sum and Carry."
category "Arithmetic"
size width=180 height=112
port input a "A" terminal=0
port input b "B" terminal=1
port output sum "Sum" terminal=4
port output carry "Carry" terminal=5
implementation
HCS 1
circuit "Half Adder implementation"
component 0 Input x=-180 y=24 value=0
component 1 Input x=-180 y=76 value=0
component 2 XOR x=40 y=24
component 3 AND x=40 y=96
component 4 Output x=280 y=24
component 5 Output x=280 y=96
wire 0.out -> 2.in0
wire 1.out -> 2.in1
wire 0.out -> 3.in0
wire 1.out -> 3.in1
wire 2.out -> 4.in
wire 3.out -> 5.in
```

Header fields:

- `id`: stable definition identifier, independent of the file name and display name. IDs contain ASCII letters, digits, `.`, `_` or `-` and are at most 128 characters.
- `name`, `description`, `category`: quoted display metadata.
- `size`: finite visual width and height in the range 48–10,000,000 editor units.
- `port input|output <port-id> "<display-name>" terminal=<component-id>`: external port and the internal `Input`/`Output` terminal component representing it.
- `implementation`: begins an embedded ordinary `.hcs` v1 graph. The parser treats everything after this line as that graph, including its own version header.

Port IDs are stable symbolic identities. Port display names can be changed without changing those IDs. Every internal `Input` and `Output` component must map to exactly one external port; an input port maps to an internal `Input`, and an output port maps to an internal `Output`. Internal implementation component and wire IDs are local to that definition. Simulator signal IDs are never written.

Definition files reject unsupported versions, invalid metadata or dimensions, duplicate port IDs/names in the same direction, duplicated terminal mappings, missing terminals, wrong terminal component types and unmapped interface terminals. File reading has a 16 MiB limit.

## Root `.hcs` references

The `.hcs` version remains **1**. A custom instance is stored as:

```text
HCS 1
circuit "Top Level"
component 10 Input x=80 y=120 value=1
component 20 CUSTOM definition=arithmetic.half-adder x=280 y=100
component 30 Output x=560 y=110
wire 10.out -> 20.a
wire 20.sum -> 30.in
```

The custom instance has only an external definition reference, its own stable component ID and position. It does not duplicate the implementation. Wire endpoints use the stable custom port IDs as symbolic pin names. A file is not self-contained unless its referenced definitions are available in the component library. There is no absolute path embedded in the document.

An application loads definitions from `$XDG_DATA_HOME/hcs/components`, or `~/.local/share/hcs/components` when `XDG_DATA_HOME` is unset. If neither environment variable is available it uses `components` relative to the working directory. Place all dependencies in this directory. The loader processes files repeatedly so dependencies may be alphabetically later than their parents. Directory import is transactional: unresolved or malformed files are reported in diagnostics and no partial dependency set is published. A root `.hcs` load that references a missing definition fails without replacing the active model. The headless `hcs_circuit` tool uses the same default library directory, and accepts `--library-dir <directory>` to validate or format a project-local dependency set:

```bash
./build/hcs_circuit validate examples/half_adder_instance.hcs \
  --library-dir examples/components
./build/hcs_circuit print examples/half_adder_instance.hcs \
  --library-dir examples/components
```

Copying a `.hcs` file alone does not copy dependencies. To move a circuit to another machine, copy the required `.hcscomponent` files as well. No absolute paths or environment-specific paths are serialized. A self-contained bundle/export command is not implemented yet.

## Documented examples

- [`examples/components/half_adder.hcscomponent`](../examples/components/half_adder.hcscomponent) — built from XOR and AND.
- [`examples/components/full_adder.hcscomponent`](../examples/components/full_adder.hcscomponent) — nested composition using two Half Adders and an OR gate; requires `arithmetic.half-adder` in the same library.
- [`examples/components/dff_cell.hcscomponent`](../examples/components/dff_cell.hcscomponent) — a sequential wrapper around the built-in DFF.
- [`examples/half_adder_instance.hcs`](../examples/half_adder_instance.hcs) — a root circuit using a reusable Half Adder definition.

The automated component tests load all three example files, including the dependency-order case, and check that each serializes deterministically.

## Creating from a selection

The editor model exposes `create_custom_component_from_selection`. It works transactionally:

1. Validate that selected component IDs are unique and exist.
2. Copy the selected components and internal wires to a private implementation, remapping by symbolic pin keys.
3. Turn cut wires entering the selected region into internal `Input` terminals and external input ports.
4. Turn cut wires leaving the selected region into internal `Output` terminals and external output ports.
5. Map selected `Input` and `Output` components to interface ports.
6. Construct a candidate root graph with the selected region replaced by one custom instance and preserve all unaffected components and wires.
7. Add the definition to the library and replace the root graph only after conversion succeeds.

The current GUI exposes multi-selection with Ctrl/Shift-click and a **MAKE CUSTOM** action. The initial port names are generated from component order/boundaries. A dedicated port-mapping dialog for selecting arbitrary internal signals and customizing the new port list is not yet available. A selected Input terminal driven by an external wire is rejected rather than silently discarded. A failed conversion leaves the source graph intact.

## Creating and editing definitions in the GUI

The GUI provides two entry paths:

- **MAKE CUSTOM** starts from the current Ctrl/Shift multi-selection. The conversion maps selected Input/Output terminals and cut boundary wires to stable external port IDs, replaces the selection with one custom instance, saves the initial definition to the default library and opens the definition editor.
- **NEW CUSTOM** prompts for a name, creates an empty definition and opens the same editor. The empty definition is not published to the library until **SAVE DEF** succeeds.

In definition-edit mode, the canvas displays the implementation graph. Add built-in gates and DFFs from the toolbar, connect the external Input/Output terminal components to the internal graph, and use **LIBRARY** to place already-defined custom components inside the current definition. The canvas remains the same editor/runtime path used by root circuits.

External port controls are:

- **+PORT IN / +PORT OUT**, or `I` / `O`: add a stable-ID external terminal and matching port record.
- Select an external terminal and press `P`: rename its display name. The stable ID remains unchanged.
- Select an external terminal and press Delete: remove that port from the working definition. If an existing root instance has a wire connected to that port, **SAVE DEF** rejects the update and preserves the old library definition and root circuit.
- Select an internal component or wire and press Delete to remove it from the working implementation.
- `N`, `D`, and `C` edit the definition name, description and category. The stable definition ID is not changed by a rename.
- **SAVE DEF** applies the edited definition transactionally to a candidate library and every instance in the saved root graph. Existing port pins/wires are retained by stable port ID; incompatible connected-port changes are rejected.
- **CANCEL EDIT** or Escape leaves the definition editor. Unsaved edits trigger a Save / Discard / Cancel prompt.

A definition can also be opened by selecting one of its instances in the root graph and choosing **EDIT DEF**. Editing affects the library definition, not only that one instance. The library panel shows the custom definition name, category, description, named input/output preview and counts; search and category filtering remain available during definition editing so nested components can be placed.

The initial port list extracted from a selection is automatically derived from its Input/Output terminals and cut wires. A separate wizard that lets the user choose arbitrary internal pins from a list is not provided; users can add, rename or remove terminals in the definition editor and wire them to the desired internal signals.

The UI-independent API also exposes `create_empty_component_definition`, `add_custom_component_port`, `rename_custom_component_port`, `remove_custom_component_port` and `apply_component_definition_transactionally`. New definitions and edits are validated before publication. Each definition may expose at most 64 input ports and 64 output ports.

## Library and hierarchy

`ComponentLibrary` owns each custom definition once, keyed by stable definition ID. It returns a metadata catalogue for built-ins and custom definitions. The GUI panel supports text search and category filtering (`All`, `Logic Gates`, `Arithmetic`, `Sequential`, `Memory`, `Control`, `Custom`); selecting a catalogue item arms it for placement on the canvas. It displays named input/output previews. The same panel is available while editing a definition, which supports nested component placement.

Before runtime expansion, validation rejects missing definitions, malformed definitions, incompatible instance port layouts, recursive/circular dependencies, excessive nesting and excessive expansion. Default limits are:

| Limit | Default |
|---|---:|
| Maximum custom nesting depth | 16 |
| Maximum expanded built-in components | 100,000 |
| Maximum dependency/expansion contexts | 4,096 |

The limits are configurable through `HierarchyLimits`. Each recursive instance is expanded into a distinct set of ordinary editor components. Built-in DFFs are copied independently per occurrence, so their state is not shared. The existing simulator continues to own combinational propagation, feedback settling, event-budget/oscillation protections and simultaneous DFF capture/commit.

A malformed or missing dependency is reported with a definition ID and, for an instance reference, its component ID when available. Expansion happens before the runtime rebuild; a failed hierarchy check does not install a partially flattened graph.

## Typography

`gui::FontManager` centralizes `Ui` and `Code` font roles. It initializes once per GUI renderer, opens fonts once, caches the handles, applies display-scale factors, and closes handles during shutdown. When SDL3_ttf is available CMake enables TTF rendering. The UI font candidates include Inter and Noto Sans; the code role includes JetBrains Mono, Iosevka, IBM Plex Mono, Noto Sans Mono and DejaVu Sans Mono. Paths are probed from common Linux font directories.

No font binaries are bundled. The application therefore does not redistribute font files; users install a font through their distribution. The candidate families' upstream license references are:

- [Inter — SIL Open Font License 1.1](https://github.com/rsms/inter/blob/master/LICENSE.txt), Copyright © Rasmus Andersson.
- [Noto Sans — SIL Open Font License 1.1](https://github.com/notofonts/NotoSans/blob/main/LICENSE), Copyright © The Noto Project Authors.
- [JetBrains Mono — SIL Open Font License 1.1](https://github.com/JetBrains/JetBrainsMono/blob/master/OFL.txt), typeface by Philipp Nurullin and JetBrains contributors.
- [Iosevka — SIL Open Font License 1.1](https://github.com/be5invis/Iosevka/blob/main/LICENSE.md), Copyright © Renzhi Li.
- [IBM Plex — SIL Open Font License 1.1](https://github.com/IBM/plex/blob/master/LICENSE.txt), Copyright © IBM Corp.; “Plex” is a Reserved Font Name.
- [DejaVu — upstream Bitstream Vera / Arev-derived license notices](https://dejavu-fonts.github.io/License.html).

The application only opens system-installed files from known Linux font paths and does not embed or redistribute them. SDL3_ttf uses separate UI and code roles, falls back from an unavailable role to the available role and finally to the centralized bitmap renderer. The bitmap fallback covers ASCII only; Cyrillic requires SDL3_ttf plus a suitable installed font. UI scale is multiplied by the current window display scale. The real SDL3/SDL3_ttf build and font rendering were not available to verify on the Phase 5.4 host. If fonts are bundled in a later release, the exact upstream copyright notices, reserved-name conditions and license files must accompany each redistributed font package.

## Validation and current limitations

The headless test suite covers definition creation, port operations, deterministic serialization, the three example files, selection conversion, library dependency loading, missing definitions, recursion rejection, depth/expansion limits, port-layout migration and runtime behavior across the existing backends. The Half Adder and nested Full Adder truth tables are compared at their observable outputs. Independent DFF instances are tested.

The definition storage layer uses `.hcscomponent` files and the library directory described above. Optional self-contained export/bundle workflow and a dedicated selection port-mapping wizard remain future work. The GUI source files passed a Clang syntax-only check against temporary local SDL3/SDL3_ttf declarations. This only checks C++ syntax against the stubbed symbols; it does not verify actual SDL API compatibility, linking, window creation, interaction or font rendering. Those must be verified on a desktop with real SDL3 and SDL3_ttf development packages installed.


## Visual symbols (Phase 5.5)

A custom component may include an independent vector symbol in an optional `symbol-begin` / `symbol-end` block before `implementation`. The block contains a complete `HCSS 1` document. Its `port id=...` records must match the existing logical external port IDs; the HCSC interface still owns input/output direction and implementation terminal mapping. Older files without this block remain compatible and receive the generated default rectangle appearance. The symbol renderer and designer workflow are documented in [`SYMBOL_DESIGNER.md`](SYMBOL_DESIGNER.md), with full HCSS examples under [`../examples/symbols/`](../examples/symbols/).
