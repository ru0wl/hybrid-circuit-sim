# HCS Phase 5.4 — Implementation and Verification Report

## Post-release SDL3 keycode compatibility correction

A consumer build against installed SDL3 headers exposed use of the removed/deprecated lowercase letter key names in `gui/application.cpp` (`SDLK_s`, `SDLK_r`, etc.). SDL3 renamed these identifiers to uppercase (`SDLK_S`, `SDLK_R`, and so on). The source has been corrected for all 14 letter identifiers used by the GUI; non-letter key constants were already valid. The new names preserve the SDL3 keycode values. A source scan confirms there are no remaining lowercase `SDLK_[a-z]` references in the GUI source.

This correction was made after the Phase 5.4 report's original test run. The current host still has no SDL3 development package, so the corrected GUI translation unit could not be linked here against official SDL3 headers. The headless GCC/Clang build and test suites were rerun after the correction. A real `hcs_gui` build on the user's SDL3-equipped Arch Linux system remains the necessary final confirmation.

## 1. Archive contents and integrity

Input: `hcs-phase5.3.zip` (693,102 bytes; 120 regular files). `unzip -t` completed with `No errors detected in compressed data`. The source was extracted and inspected before implementation. Build directories and temporary SDL declaration stubs are not part of the deliverable archive.

The Phase 5.4 source tree contains the Phase 5.3 files plus 10 new implementation/example files and modifications to 20 existing files. There are no deleted source files. This report is an additional new file.

## 2. Initial source audit

The original Phase 5.3 tree already had:

- `CircuitEditor` with component, pin and wire models, stable editor-level IDs, selection and document revisions;
- `CircuitRuntime`, which converts the editor graph through the existing `CircuitBuilder` to `hcs::Simulator`;
- `.hcs` version-1 deterministic serialization and atomic parse/replace loading;
- an SDL3 application/renderer split, text-editing mode, path entry and New/Open/Save actions;
- CMake and Makefile targets for core, editor, tests and headless CLI.

There was no reusable custom-definition model, definition file, dependency library, hierarchy validation/expansion or centralized font manager. Components used the fixed built-in `ComponentType` set. The runtime was not modified to add a second custom-logic evaluator.

Before Phase 5.4 changes, the GCC Release CMake build succeeded and the three existing CTest targets passed (3/3).

## 3. Custom-component architecture

The new `hcs::editor::CustomComponentDefinition` contains a stable definition ID, display name, description, category, version, visual size, external port records and a private `CircuitEditor` implementation graph.

A `CustomPort` has a stable symbolic port ID, display name, direction and the internal terminal component that implements the boundary. Input ports map to internal built-in `Input` components; output ports map to internal built-in `Output` components. The internal graph otherwise uses existing built-in component types and may contain instances of other custom definitions.

`ComponentLibrary` owns definitions once in an ID-keyed ordered map and exposes a catalogue containing built-ins and user definitions. A root `ComponentType::Custom` instance stores the stable definition ID and instance-specific position, pins and connections. Its pins are keyed by stable external port IDs; the visible labels may change without changing those IDs.

## 4. Component definition file format

The new reusable extension is `.hcscomponent`, schema header `HCSC 1`. It is UTF-8, line-oriented text. Metadata and interface ports precede an `implementation` marker; the rest of the file is an ordinary embedded `.hcs` v1 internal graph.

Example header:

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
```

The parser validates definition version, stable IDs, unique port IDs and per-direction names, port directions, dimensions, metadata size, terminal existence/type, duplicate terminal mappings and unmapped internal Input/Output terminals. Definition files are capped at 16 MiB. Each definition may expose up to 64 input and 64 output ports.

The root `.hcs` format remains version 1. A custom instance is serialized as `component <id> CUSTOM definition=<stable-id> x=<x> y=<y>`. Its internal implementation is not duplicated in every instance. Wires use the definition's symbolic port IDs. No absolute library path is serialized.

## 5. Selection-to-component workflow

The UI-independent `create_custom_component_from_selection` operation is transactional. It verifies selected IDs, copies selected components and internal wires to a candidate definition, maps cut boundary wires entering/leaving the region to input/output terminals, maps selected Input/Output terminals, constructs a candidate root graph and replaces the selected region with one custom instance only after conversion succeeds.

In the GUI, Ctrl/Shift-click extends the component selection and **MAKE CUSTOM** opens the name prompt. The extracted definition is saved to the default library and the editor switches to definition-edit mode. The initial port list is generated from selected terminals and cut wires. Users can then add, rename or remove interface ports and connect the corresponding terminal components to arbitrary internal signals.

There is no separate pre-conversion wizard that presents every internal signal for arbitrary port selection. A selected Input terminal already driven by an external wire is rejected instead of silently dropping that connection. Invalid conversion leaves the original model unchanged.

## 6. Empty definition creation and editing

**NEW CUSTOM** creates an empty definition with a generated stable ID and opens definition-edit mode. The new definition is not published until **SAVE DEF** succeeds. The same canvas/runtime path is used for the internal graph, with built-in Input/Output terminals, gates, DFFs and library-backed nested custom components.

In definition-edit mode:

- **+PORT IN / +PORT OUT** or `I` / `O` create external ports and their internal terminal components;
- selecting a port terminal and pressing `P` renames the display name without changing the stable port ID;
- Delete removes a selected interface port or internal component/wire;
- `N`, `D` and `C` edit the definition name, description and category;
- **LIBRARY** remains available for nested custom placement;
- **SAVE DEF** validates and transactionally applies the candidate definition to all instances in the root document;
- **CANCEL EDIT** or Escape provides the unsaved-edit confirmation path.

**EDIT DEF** opens the shared library definition used by the selected custom instance. It does not create an instance-private definition copy. Port additions and display-name changes migrate compatibly by stable ID. Removing a connected port or changing a connected port's direction is rejected, leaving the published definition and root graph unchanged.

## 7. Component library

The library catalogue contains the seven current built-in components and user-defined definitions. Entries expose name, category, description, input/output counts and named input/output previews. The GUI library panel supports text search and basic category filtering (`All`, `Logic Gates`, `Arithmetic`, `Sequential`, `Memory`, `Control`, `Custom`). Selecting an entry arms it for canvas placement. The panel is also available while editing a definition, which enables nested composition.

Default definition storage is `$XDG_DATA_HOME/hcs/components`, or `~/.local/share/hcs/components` if `XDG_DATA_HOME` is unset, or relative `components` if neither variable is available. Directory loading is transactional and repeatedly resolves dependencies so a definition can depend on a file that sorts later alphabetically. A malformed or unresolved file prevents the candidate directory import from replacing the existing library.

The headless `hcs_circuit` tool now loads this same library. It uses the default directory or an explicit `--library-dir <directory>` argument. A custom-referencing circuit with an unavailable definition is rejected with an error naming the missing definition.

## 8. Nested hierarchy and validation

The hierarchy validator resolves all custom references and rejects missing definitions, duplicate definition IDs, duplicate or invalid port IDs, invalid terminal mappings, incompatible instance pin layouts, recursive/circular dependency graphs, excessive nesting and excessive expansion.

Default `HierarchyLimits` are:

| Limit | Default |
|---|---:|
| Maximum nesting depth | 16 |
| Maximum expanded built-in components | 100,000 |
| Maximum dependency/expansion contexts | 4,096 |

The public limits structure allows callers/tests to lower or raise these bounds. The definition shape validator caps each input/output port direction at 64. Error reports include definition ID and instance ID when available.

## 9. Runtime expansion and simulator integration

`CircuitRuntime::synchronize(editor, library)` validates and expands the hierarchy into a temporary flat `CircuitEditor`. Each definition instance receives a distinct component/pin identity space. Boundary input ports become aliases from the containing graph into the child implementation; boundary output ports alias the implementation signal back to the containing graph. Once flattened, the normal runtime rebuild path constructs the existing `hcs::Circuit` through `CircuitBuilder` and runs the existing `hcs::Simulator`.

There is no custom Boolean evaluator, alternate simulator, new backend or changed public simulator API. The expansion result currently exposes root-pin-to-expanded-pin and root-input mappings. A stable debugger-facing map for every internal component/signal back to a full hierarchy path is not exposed yet.

## 10. Instance isolation and sequential behavior

Every occurrence is expanded separately. A custom definition containing a DFF therefore creates separate flattened built-in DFF components and runtime state for each instance. The existing simulator remains responsible for DFF simultaneous capture/commit and existing combinational, feedback, event-budget and oscillation behavior.

The Half Adder custom instance is tested against an independently constructed reference graph containing ordinary Input, XOR, AND and Output components. The custom and manually expanded circuits are compared for all four input combinations on all six existing backends. The nested Full Adder truth table and two independently clocked DFF custom instances are also tested across all six backends.

## 11. Validation and safety

Automated coverage includes malformed/unsupported definition files, transactional library-directory loading, missing root and nested references, duplicate definition IDs, duplicate external port IDs, missing terminal components, incompatible instance layouts, self-recursion, two-definition dependency cycles, maximum depth, maximum expanded-component count and maximum dependency-context limits.

A root `.hcs` parse using a missing custom definition fails before replacing the active editor model. Definition edits and selection conversion use candidate models/libraries and publish only after validation. Instance port migration preserves existing pin IDs and wires for compatible changes, and rejects an incompatible removal of a connected port.

## 12. Typography and font licensing

`gui::FontManager` is the centralized text-rendering interface. It defines `Ui` and `Code` roles, caches TTF handles after one-time initialization, scales drawing by the window display scale, and closes fonts/shuts down SDL3_ttf during cleanup. The text editor uses the monospaced `Code` role; ordinary labels use `Ui`.

With SDL3_ttf available, the manager probes system-installed Inter/Noto Sans/DejaVu Sans candidates for UI and JetBrains Mono/Iosevka/IBM Plex Mono/Noto Sans Mono/DejaVu Sans Mono candidates for code. If one role is missing, it falls back to the other role; without an available font it uses the built-in ASCII bitmap renderer. The bitmap fallback substitutes unsupported characters and does not provide Cyrillic coverage. For Cyrillic and broad symbols, SDL3_ttf and a suitable installed font such as Noto Sans are required. Fonts are not bundled, so no third-party font binaries are redistributed.

Upstream licensing references are listed in `CUSTOM_COMPONENTS.md`: Inter, Noto Sans, JetBrains Mono, Iosevka and IBM Plex use SIL Open Font License 1.1; DejaVu retains its upstream Bitstream Vera/Arev-derived notices. If a future build bundles any font, its exact copyright notice, reserved-name requirements and license text must be included. Font resource initialization/rendering could not be exercised on this host because real SDL3_ttf was unavailable.

## 13. Tests and exact results

Toolchain versions used:

- GCC 14.2.0;
- Clang 17.0.0;
- CMake 3.31.6;
- GNU Make 4.4.1.

Final results after the last source/test changes:

| Build/test route | Result |
|---|---:|
| GCC Release CMake build + CTest | 4/4 CTest targets passed |
| Clang Release CMake build + CTest | 4/4 CTest targets passed |
| Makefile `test` target (Release) | 4/4 CTest targets passed |
| CLI custom-instance validate | Passed |
| CLI canonical print stability | Passed; two consecutive canonical prints compared byte-for-byte |
| CLI format output | Passed; formatted output matched canonical print |
| CLI missing-definition rejection | Passed; exited non-zero with a missing-definition diagnostic |
| GUI C++ syntax-only check | Passed against temporary SDL3/SDL3_ttf declarations only |

The four CTest targets are `hcs_tests`, `hcs_editor_tests`, `hcs_persistence_tests` and `hcs_custom_component_tests`. The custom-component test executable exercises definition creation and serialization, deterministic output, selection conversion, stable port renaming/migration, transactional edits and directory loading, Half Adder reference equivalence, nested Full Adder behavior, independent DFF instances, example definitions and hierarchy rejections.

The existing benchmark files were not regenerated. A SHA-256 comparison against the Phase 5.3 archive checked 35 historical benchmark files (`BENCHMARKS.md`, `bench/` and benchmark-named artifacts): 0 changed and 0 missing.

## 14. Actual SDL3 GUI verification status

The environment does not have real SDL3 or SDL3_ttf development packages (`pkg-config` could not find either and `/usr/include/SDL3` is absent). CMake therefore omitted the `hcs_gui` target. There was no actual GUI link, window launch, mouse/keyboard interaction, library placement, definition editing or font rendering test.

For an additional source-level check, Clang ran `-fsyntax-only` over `gui/application.cpp`, `gui/renderer.cpp` and `gui/font_manager.cpp` with temporary local stub declarations for SDL3 and SDL3_ttf. This only verifies parsing/type-checking against those stub declarations. It is not equivalent to compiling against official SDL headers, linking SDL, running the GUI or checking runtime behavior.

## 15. Files added and modified

New files:

- `include/hcs/editor/custom_components.hpp`
- `src/editor/custom_components.cpp`
- `tests/test_custom_components.cpp`
- `docs/CUSTOM_COMPONENTS.md`
- `gui/font_manager.hpp`
- `gui/font_manager.cpp`
- `examples/components/half_adder.hcscomponent`
- `examples/components/full_adder.hcscomponent`
- `examples/components/dff_cell.hcscomponent`
- `examples/half_adder_instance.hcs`
- `docs/PHASE5_4_REPORT.md` (this report)

Modified existing files:

- `include/hcs/editor/model.hpp`
- `src/editor/model.cpp`
- `include/hcs/editor/persistence.hpp`
- `src/editor/persistence.cpp`
- `include/hcs/editor/runtime.hpp`
- `src/editor/runtime.cpp`
- `include/hcs/editor/simulation_controller.hpp`
- `src/editor/simulation_controller.cpp`
- `gui/application.cpp`
- `gui/renderer.cpp`
- `gui/renderer.hpp`
- `src/circuit_tool.cpp`
- `CMakeLists.txt`
- `README.md`
- `CHANGELOG.md`
- `ROADMAP.md`
- `docs/CIRCUIT_FORMAT.md`
- `docs/EDITOR_MODEL.md`
- `docs/GUI_ARCHITECTURE.md`
- `ARCHITECTURE.md`

No Phase 5.3 source files were deleted. No historical benchmark artifacts were changed.

## 16. Known limitations

- Selection conversion automatically derives the initial interface from selected Input/Output terminals and cut wires. There is no dedicated wizard listing arbitrary internal pins before conversion; ports can be adjusted afterward in definition-edit mode.
- There is no self-contained circuit bundle/export. Copy the referenced `.hcscomponent` files with the `.hcs` document, or pass/use the correct component library directory.
- Only root-pin mappings are exposed from the expansion result; a debugger-oriented full internal hierarchy path map remains future work.
- The GUI and typography runtime were not verified against actual SDL3/SDL3_ttf. The syntax-only SDL stub check must not be treated as runtime validation.
- The system-font approach does not bundle fonts. The built-in fallback is ASCII-only, and no automated runtime tests for FontManager resource loading/glyph coverage could be run without a real SDL3_ttf environment.
- The default library folder is per-user. The CLI's `--library-dir` option supports project-local libraries but does not embed dependencies into `.hcs`.

## 17. Intentionally deferred work

- Arbitrary-signal selection-to-port mapping wizard;
- self-contained `.hcs` dependency bundle/export and import workflow;
- public full internal component/signal hierarchy inspection map;
- real SDL3/SDL3_ttf GUI interaction, rendering, DPI and font glyph verification on a desktop host;
- automated FontManager runtime/resource tests on a real SDL3_ttf backend;
- undo/redo, scripting, plugin system, new simulator backends, GPU/Vulkan, SIMD/multicore changes, networking and major GUI redesign.

Phase 5.4's model, persistence, hierarchy expansion, library and headless runtime behavior are implemented and covered by the available tests. The full acceptance list is not claimed as completely verified: GUI runtime and typography rendering require a real SDL3 desktop, and the selection mapping wizard, dependency bundle and full internal debug map are not implemented in this phase build.


## UI refinement follow-up

The post-Phase-5.4 interface refinement reduces the TTF base point size (UI 12.5 pt, code 12 pt before caller scale), adds exact `TTF_GetStringSize`-based measurement with bitmap-cell fallback metrics, centers toolbar labels and component captions using measured bounds, and aligns custom output-port labels outside their pin edge. The toolbar no longer contains the CPU/AVX2/SIMD/worker status string. A 46-pixel footer now contains the document/simulation status on its first row and system/acceleration information on its second row.
