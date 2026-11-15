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
Circuit persistence (.hcs text) -- parse/validate/atomic EditorModel replacement
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
- `CircuitRuntime`: translation from the editor model to `hcs::Circuit`, stable pin-to-signal mappings while structure is unchanged, and access to the real `hcs::Simulator`;
- `SimulationController`: paused/manual/fixed-frequency/unlimited scheduling;
- `Camera2D`: world/screen coordinate conversion, panning and cursor-anchored zoom.

This library can be tested without SDL3 or a display server.

### `hcs_gui`

When SDL3 is found by CMake, `hcs_gui` builds the SDL3 executable. `gui/application.cpp` owns the event loop and coordinates interactions. `gui/renderer.cpp` renders a read-only view of the editor model and synchronized runtime signal values. The renderer does not own or mutate circuit data.

The renderer is procedural: rectangles, lines, pin squares, a world-space grid and a small bitmap font. The application also provides a multiline text-editing view using SDL text input, keyboard editing and clipboard support. File paths are entered in a small in-window prompt, avoiding a native-dialog dependency. This keeps the rendering implementation replaceable.

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
- Middle drag pans; wheel zooms around the pointer.
- Escape cancels placement/wiring or clears selection; Space toggles run/pause.
- NEW / OPEN / SAVE / SAVE AS are on the second toolbar row; Ctrl+N, Ctrl+O, Ctrl+S and Ctrl+Shift+S are shortcuts. Replacing an unsaved document asks to Save, Discard or Cancel. Save As confirms before replacing a different existing file.
- TEXT / Ctrl+T opens the editable `.hcs` text. APPLY TEXT validates and atomically replaces the editor model; an invalid document is kept in the text buffer with an error. RELOAD TEXT explicitly discards text edits and regenerates from the graphical model. Ctrl+S in text mode applies and saves; Ctrl+R regenerates the text from the current graph.

The initial wire route is a straight segment. Routing is intentionally deferred.

## Dependency and build behavior

SDL3 is optional for core/editor-library builds but required for the windowed executable. With SDL3 available, CMake creates `hcs_gui`. If it is missing, CMake emits a warning and still builds the core, CLI tools and tests. Set `HCS_REQUIRE_SDL3=ON` to make absence a configuration error.

On Arch Linux install the dependency with:

```bash
sudo pacman -S sdl3
```

Then build and launch:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHCS_REQUIRE_SDL3=ON
cmake --build build --parallel
./build/hcs_gui
```

No SDL_ttf, image, audio or game-engine dependency is required.

## Current limitations

Undo/redo, auto-routing, bus/multi-bit signals, multi-driver logic, tri-state values, subcircuits, custom/user-defined components, simulation thread, networking or external devices are not included. Circuit persistence is implemented in the separate version-1 `.hcs` editor-document layer; it does not serialize simulator state or runtime signal IDs. Unconnected gate/DFF input pins are connected to logic 0 during HCS circuit construction. Only binary 0/1 values are produced by the current core; the presentation enum reserves Unknown, HighImpedance and Error for future expansion.

## Validation note for this development environment

The build host used for Phase 5 did not have the real SDL3 development package or a desktop display session available. The core/editor libraries and tests can be built without SDL3. The Phase 5.3 GUI source was syntax-checked against a minimal SDL3 API declaration stub in this environment; this is not a substitute for compiling/linking against the actual SDL3 library or launching the application. A real SDL3 window, renderer, file-path entry, keyboard text editing, clipboard, mouse interaction and high-DPI behavior remain to be verified on an SDL3-equipped Linux desktop.
