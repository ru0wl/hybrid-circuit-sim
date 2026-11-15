# HCS Phase 5.3 — Implementation Report

## 1. Source archive and audit

The supplied Phase 5.2 ZIP contained **113 files**. It was extracted and inspected before implementation. The existing editor model, `CircuitRuntime`, component/pin/wire layouts, runtime construction path, CMake targets, Makefile and documentation were reviewed. A baseline GCC Release build completed and the two original CTest targets passed before changes.

No circuit-file persistence implementation was present in the audited source. `CircuitEditor` owned the editable graph and its internal component, pin and wire IDs; `CircuitRuntime` translated that graph through the existing `CircuitBuilder` into the selected `hcs::Simulator`. This provided a clean persistence boundary at the editor-document layer.

The final archive contains the original project files, six Phase 5.3 implementation/example files and this report (120 files total before build artifacts). Generated build directories are not included.

## 2. File format

The official extension is **`.hcs`** and the current schema is **version 1**. The format is readable line-oriented UTF-8 text:

```text
HCS 1
circuit "Example"

component 10 Input x=80 y=150 value=0
component 20 NOT x=280 y=150
component 30 Output x=500 y=150

wire 10.out -> 20.in
wire 20.out -> 30.in
```

Supported component tokens are `Input`, `Output`, `AND`, `OR`, `NOT`, `XOR` and `DFF`. Each component has a stable unsigned 32-bit component ID and finite `x`/`y` coordinates. Input components may store `value=0|1`; DFF components may store `initial=0|1`. Ports are symbolic names: `out`, `in`, `in0`, `in1`, `d`, `clk` and `q`, according to component type.

Serialization sorts components by component ID and wires by their symbolic endpoint text. Property order is fixed, numeric formatting uses the classic locale and enough precision to round-trip a `float`. Component IDs are retained across restarts. Pin IDs, wire IDs, simulator signal IDs, pointers, memory layout, backend choice and transient simulator state are intentionally not serialized. Pin identities are recreated from the existing component layouts when parsing.

See [`CIRCUIT_FORMAT.md`](CIRCUIT_FORMAT.md) for the complete grammar, validation rules and XOR example.

## 3. GUI save/load workflow

The SDL editor now exposes **NEW**, **OPEN**, **SAVE** and **SAVE AS**. File paths are entered in an in-window prompt, avoiding a new native-dialog dependency. Missing `.hcs` extensions are added for Save As. Save As asks before overwriting a different existing file. New/Open/Demo/Quit operations request Save, Discard or Cancel when the document or text buffer has unsaved changes.

The current filename is shown in the status bar and window title. A `*` marks unsaved changes. The editor tracks document revisions for component insertion/removal, wire insertion/removal, component movement, Input value changes and DFF initial-state changes. Successful load/save marks the model clean.

## 4. Text editing workflow

Choose **TEXT** or press `Ctrl+T`. The application generates canonical text from the current graphical model. The built-in multiline text view supports keyboard movement/selection, text input, clipboard copy/cut/paste, Apply, Reload and return to Graph.

**APPLY TEXT** parses and validates the edited buffer. A valid document atomically replaces the editor model, updates the circuit name, synchronizes `CircuitRuntime` and resets the simulation against the new graph. An invalid document leaves the graphical model/runtime unchanged, keeps the user's edited text and displays the parser error. **RELOAD TEXT** intentionally discards the text buffer and regenerates it from the current model. In text mode `Ctrl+S` applies and saves, `Ctrl+R` reloads from the model, and `Ctrl+T` returns to the graph while preserving unapplied text.

## 5. Validation and atomicity

The parser validates the full document before installing any new model. It detects unsupported versions, malformed statements and escapes, duplicate component IDs or fields, unknown component types/fields, missing required coordinates, invalid/non-finite/out-of-range positions, illegal state fields, missing component references, nonexistent or directionally invalid pins, and multiple drivers to an input pin. Errors include a line number when one is available and explain the failing field or endpoint. Files are limited to 16 MiB; circuit names are limited to 256 bytes.

Parsing constructs a temporary `CircuitEditor`. Only after parsing, reference validation and model construction all succeed is the active model replaced. A failed parse or file load preserves both the open model and circuit name. File saving serializes first, writes a sibling `.tmp` file, flushes it and renames it over the destination on the supported POSIX build host.

## 6. CLI tool

The new headless `hcs_circuit` executable provides:

```bash
./build/hcs_circuit validate example.hcs
./build/hcs_circuit print example.hcs
./build/hcs_circuit format example.hcs [output.hcs]
```

It uses the same parser/validator as the editor and does not require SDL. Validation returns `0` for a valid file and non-zero for an invalid file; diagnostics are written to stderr and successful output is English.

## 7. Tests and verification

Executed in this environment:

- Baseline before implementation: GCC Release build; original CTest suite **2/2 passed**.
- GCC 14.2.0, CMake 3.31.6, Release: all available non-SDL targets built; CTest **3/3 passed** (`hcs_tests`, `hcs_editor_tests`, `hcs_persistence_tests`).
- Clang 17.0.0, CMake Release: all available non-SDL targets built; CTest **3/3 passed**.
- Existing Makefile `test` target (GCC Release): build completed; CTest **3/3 passed**.
- GUI application/renderer C++ sources passed a Clang `-fsyntax-only` check using a minimal SDL3 API declaration stub. This verifies C++ syntax only, not compatibility with the real SDL3 headers or linker.
- CLI validated `examples/xor.hcs`, printed and formatted it; canonical output matched byte-for-byte. An unsupported-version file was rejected with exit code `1`.
- Persistence tests cover NOT/XOR/DFF and multi-input/multi-wire round trips, moved components, deterministic serialization, text edits, malformed inputs, parse/file-load atomicity, file save/load, Input changes and DFF behavior. The loaded XOR and DFF cases were executed across all six configured runtime backends: Interpreted Event, Interpreted Dense, Optimized IR Dense, Optimized IR Event, Hybrid IR and Compiled Native.

No benchmark suite was rerun and no historical benchmark artifacts were edited.

## 8. Limitations

SDL3 development files and a desktop display session were not available in the build environment. Consequently CMake correctly omitted `hcs_gui`; the GUI was not linked against real SDL3 and was not launched or interactively tested. The in-window path prompt, SDL text input and clipboard behavior need verification on an SDL3-equipped desktop. The text view is deliberately a small built-in editor, not a full syntax-highlighting editor. File saving uses the atomic rename behavior available on the POSIX build host.

The format stores the editor graph and persistent component values, not the current transient simulator state or runtime backend selection. DFF `initial` stores the reset/initial value; the live Q state is runtime state and is not part of a circuit document.

## 9. Files added or changed

**Added**

- `include/hcs/editor/persistence.hpp`
- `src/editor/persistence.cpp`
- `src/circuit_tool.cpp`
- `tests/test_persistence.cpp`
- `docs/CIRCUIT_FORMAT.md`
- `examples/xor.hcs`
- `docs/PHASE5_3_REPORT.md`

**Changed**

- `include/hcs/editor/model.hpp`
- `src/editor/model.cpp`
- `gui/application.cpp`
- `CMakeLists.txt`
- `Makefile`
- `README.md`
- `ARCHITECTURE.md`
- `CHANGELOG.md`
- `ROADMAP.md`
- `docs/EDITOR_MODEL.md`
- `docs/GUI_ARCHITECTURE.md`
- `docs/HCS_GUI_INTEGRATION.md`

Historical benchmark files and result tables were left unchanged.

## 10. Intentionally deferred

Custom/user-defined components, hierarchical subcircuits, component libraries, undo/redo, auto-routing, GPU compute, new simulation backends, SIMD/multicore changes and automatic backend selection are not part of Phase 5.3. Real SDL3 GUI build/interaction verification is still required on a desktop host. Future incompatible file schemas must use an explicit new format version and migration policy.
