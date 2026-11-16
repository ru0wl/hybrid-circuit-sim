# GUI Architecture (Phase 5 foundation)

## Layer boundaries

The GUI is an SDL3 application around the existing HCS simulator. It does not implement logic evaluation.

```text
SDL3 Application / Input
        |
        v
CircuitRenderer ---- Camera2D
        |                |
        v                v
Editor Circuit Model (world-space components, pins, wires, selection)
        |
        v
Circuit persistence (.hcs + .hcscomponent) -- parse/validate/atomic model replacement
        |
        v
Custom hierarchy validation / flattening
        |
        v
CircuitRuntime / SimulationController
        |
        v
Existing hcs_core (CircuitBuilder, Simulator, IR, native module loader)
```

### `hcs_core`

The `hcs_core` CMake target is the existing simulation library (the legacy CMake link name `hcs` remains an alias). Phase 5 does not modify its gate evaluation, IR, backend implementations, DFF semantics, native compilation or historical benchmark data.

### `hcs_editor`

`hcs_editor` is a UI-independent C++20 library. It contains:

- `CircuitEditor`: editor component/pin/wire model, hit testing, selection, structural/input/document revision tracking and atomic content replacement;
- `persistence.cpp`: deterministic `.hcs` text serialization, strict parser/validator, atomic file save and parse-then-replace loading;
- `custom_components.cpp`: stable-ID definitions, `.hcscomponent` persistence, component catalogue, hierarchy validation, transactional selection extraction and flattening;
- `CircuitRuntime`: translation from the editor model to `hcs::Circuit`, stable pin-to-signal mappings while structure is unchanged, and access to the real `hcs::Simulator`;
- `SimulationController`: paused/manual/fixed-frequency/unlimited scheduling;
- `Camera2D`: world/screen coordinate conversion, panning and cursor-anchored zoom.

This library can be tested without SDL3 or a display server.

### `hcs_gui`

When SDL3 is found by CMake, `hcs_gui` builds the SDL3 executable. `gui/application.cpp` owns the event loop and coordinates interactions. `gui/renderer.cpp` renders a read-only view of the editor model and synchronized runtime signal values. The renderer does not own or mutate circuit data.

The renderer is procedural: rectangles, lines, pin squares and a world-space grid. All labels go through a centralized `FontManager` with proportional UI and monospaced code roles. The font manager also exposes text measurement using actual SDL3_ttf glyph metrics (or the bitmap fallback cell size), so toolbar labels, component captions and external pin labels can be centered against their real rendered bounds. Hardware/CPU/SIMD information is displayed on a dedicated second status-bar line rather than over the toolbar controls. SDL3_ttf is optional; installed Inter/Noto Sans and JetBrains Mono/Iosevka/IBM Plex Mono/Noto Sans Mono/DejaVu Mono candidates are probed once. TTF fonts are cached by role and effective point size, so text is rasterized at its final size and rendered without stretching a small font texture. The fallback snaps glyphs and coordinates to whole pixels to avoid fractional-rectangle blur, but remains an ASCII pixel font. The application also provides a multiline text-editing view using SDL text input, keyboard editing and clipboard support. File paths are entered in a small in-window prompt, avoiding a native-dialog dependency. This keeps the rendering implementation replaceable.

## Application update sequence

Each frame performs these independent stages:

1. Poll SDL3 events and update the editor model/camera/UI state.
2. Call `CircuitRuntime::synchronize`. It rebuilds only if the structural revision or backend changed; ordinary geometry and selection changes do not rebuild.
3. Call `SimulationController::update(delta_time)`. It schedules zero or more simulation steps independently of rendering logic.
4. Render grid, wires, components and overlays.

The fixed-frequency controller uses an accumulator for 1 Hz, 10 Hz, 60 Hz and 1 kHz. Unlimited mode runs in a bounded main-thread time slice so the UI can continue to process events and render. Manual step is available while paused. No worker thread is introduced in this phase.

## Interaction

- Left click on a palette action selects a component to place.
- Left click on the canvas places the selected component.
- Drag a component body to move it.
- Click an Input component without dragging to toggle its stored external input level.
- Drag from an output pin to an input pin to connect them.
- Select a wire/component and press Delete or Backspace to remove it.
- Drag an empty area of the canvas with the left mouse button or drag with the middle mouse button to pan; wheel zooms around the pointer. Left-dragging a component still moves it, and pins/wires retain their existing gestures.
- Escape cancels placement/wiring or clears selection; Space toggles run/pause. Click the backend button to cycle the six existing runtime backends; its label displays the active backend. `B` is also a shortcut.
- NEW / OPEN / SAVE / SAVE AS are on the second toolbar row; Ctrl+N, Ctrl+O, Ctrl+S and Ctrl+Shift+S are shortcuts. Replacing an unsaved document asks to Save, Discard or Cancel. Save As confirms before replacing a different existing file.
- Ctrl/Shift-click extends component selection; **MAKE CUSTOM** extracts a selected region, saves an initial `.hcscomponent` definition in the default library directory and enters definition-edit mode.
- **NEW CUSTOM** prompts for a name and opens an empty definition in the same editor. **EDIT DEF** opens the selected custom instance's shared library definition, not a copy scoped to that placed instance.
- In definition-edit mode, **+PORT IN / +PORT OUT** or `I` / `O` add interface terminals; select a terminal and press `P` to rename its display name; Delete removes a selected port or internal component/wire. `N`, `D` and `C` edit name, description and category. **SAVE DEF** validates and transactionally migrates all instances; **CANCEL EDIT** or Escape prompts before discarding dirty work.
- The definition editor retains the normal canvas/runtime. **LIBRARY** stays available in this mode so nested custom components can be placed. The panel shows name, category, description, named input/output preview and counts, and supports search/category filtering.
- TEXT / Ctrl+T opens the editable `.hcs` text. APPLY TEXT validates and atomically replaces the editor model; an invalid document is kept in the text buffer with an error. RELOAD TEXT explicitly discards text edits and regenerates from the graphical model. Ctrl+S in text mode applies and saves; Ctrl+R regenerates the text from the current graph.

The initial wire route is a straight segment. Routing is intentionally deferred.

## Visual Component Designer (Phase 5.5)

The shared custom-definition editor opens **SYMBOL** to enter Visual Component Designer mode. It uses a left object/port list, central symbol canvas, right selection/property/help panel and a toolbar for vector primitives, path/text tools, logical port creation, snapping, zoom, preset, fill/stroke palette, source view and Save Definition. This is a view over `CustomComponentDefinition::symbol`; it does not make a second visual model or touch the internal simulator graph for appearance-only edits.

`SymbolDefinition` and its `HCSS 1` serializer/parser live in `hcs_editor`. `SymbolRenderer` in the SDL3 GUI is shared between main-canvas custom instances, library thumbnails and designer canvas. It transforms shape points, text anchors and port endpoints through the same instance rotation/flip transform. `CircuitEditor` custom pin layouts take their local positions from these visual port records, so the pin's world position and drawn endpoint remain coincident after component transforms. Hit testing works back in untransformed symbol-local coordinates and uses object contours/ellipse geometry, not just the overall rectangle.

In the designer, drag creates basic shapes and shows a live preview; polygon/path tools collect clicked vertices with a pointer-following contour preview and finish with Enter. Select and drag moves objects/ports; the selected object has a visible lower-right resize handle. `P` enables path vertex editing, Ctrl+click inserts a point on a segment, Delete removes the selected vertex and `C` toggles path closure. `O` cycles cardinal port orientation; Shift+O sets angle mode and left/right adjust by 15 degrees. `R`, `D`, `[ ]`, Home/End, `V`, `L`, `F` and Shift+arrows/H/V provide rotate, duplicate, layer, visibility, lock, fill and canvas-alignment operations. Toolbar buttons adjust stroke width and text size; exact numeric values and arbitrary colors remain available in HCSS text. `E` edits selected text/port labels. `SYMBOL TEXT` or Ctrl+T opens HCSS source; Ctrl+S applies a temporary parsed/validated candidate, Ctrl+R reloads canonical text and Ctrl+T returns to visual mode. The HCSS editor enforces exact correspondence between visual port IDs and the existing logical interface IDs.

The symbol-only update path changes the catalogue's visual revision, not its logic revision. The main frame can therefore render a new appearance without rebuilding or resetting `CircuitRuntime`. Logic/port edits continue through the existing definition validation and runtime synchronization path. See [`SYMBOL_DESIGNER.md`](SYMBOL_DESIGNER.md) for the format and limits.

## Dependency and build behavior

SDL3 is optional for core/editor-library builds but required for the windowed executable. With SDL3 available, CMake creates `hcs_gui`. If it is missing, CMake emits a warning and still builds the core, CLI tools and tests. Set `HCS_REQUIRE_SDL3=ON` to make absence a configuration error.

On Arch Linux, install SDL3, SDL3_ttf and the suggested system fonts with:

```bash
sudo pacman -S sdl3 sdl3_ttf noto-fonts ttf-jetbrains-mono
```

Then build and launch:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHCS_REQUIRE_SDL3=ON
cmake --build build --parallel
./build/hcs_gui
```

SDL3_ttf is optional. For sharp antialiased fonts, install SDL3_ttf plus a UI font such as Inter/Noto Sans and a code font such as JetBrains Mono/Iosevka/DejaVu Sans Mono from your distribution. On Arch Linux, `sdl3_ttf`, `noto-fonts` and `ttf-jetbrains-mono` provide a supported setup. If SDL3_ttf or the font files are missing, HCS uses a whole-pixel ASCII fallback. No fonts are bundled. Image/audio/game-engine dependencies are not used.

## Current limitations

Undo/redo, auto-routing, bus/multi-bit signals, multi-driver logic, tri-state values, simulation thread, networking and external devices are not included. Custom components are defined in `.hcscomponent` files, can be authored/edited in the GUI and are flattened before the existing simulator path. A dedicated arbitrary-signal selection-port mapping wizard and self-contained dependency export remain unimplemented. Circuit persistence is implemented in the separate version-1 `.hcs` editor-document layer; it does not serialize simulator state or runtime signal IDs. Unconnected gate/DFF input pins are connected to logic 0 during HCS circuit construction. Only binary 0/1 values are produced by the current core; the presentation enum reserves Unknown, HighImpedance and Error for future expansion.

## Validation note for this development environment

The build host used for Phase 5 did not have the real SDL3 development package or a desktop display session available. The core/editor libraries and tests can be built without SDL3. The GUI source files passed a Clang syntax-only check against temporary local SDL3/SDL3_ttf declarations. This is not a build against real headers and does not establish SDL API compatibility or linkability. The GUI was not compiled or linked against the real SDL3 API in this Phase 5.4 environment because SDL3 development files were unavailable. No claim of real GUI interaction or font rendering is made. A real SDL3 window, renderer, file-path entry, keyboard text editing, clipboard, mouse interaction and high-DPI behavior remain to be verified on an SDL3-equipped Linux desktop.
