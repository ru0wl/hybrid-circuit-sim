# HCS Phase 5.5 — implementation and verification report

## 1. Archive and initial audit

Input: `hcs-phase5.4-ui-refined.zip` (131 project files; ZIP archive integrity checked before extraction).

The Phase 5.4 source was inspected before changes. The custom definition already separated its internal `CircuitEditor`, stable logical `CustomPort` records and definition metadata. The visual model did not exist: custom components were drawn through a generic body renderer and the existing model derived custom input/output pin positions from the definition's size and the built-in side layout. The GUI used SDL3 and the existing `FontManager`; no alternate graphics framework was present. CMake and Makefile already provided headless editor, persistence and custom-component test targets.

The baseline GCC Release build completed and the four original CTest suites passed (4/4).

## 2. Architecture and separation of concerns

Phase 5.5 adds a renderer-independent `hcs::editor::SymbolDefinition` model in `include/hcs/editor/symbol.hpp` and `src/editor/symbol.cpp`.

A custom definition now has three independent parts:

- Logic: the existing internal `CircuitEditor` implementation.
- Interface: the existing stable logical input/output port IDs and directions.
- Appearance: the symbol model, including shapes, paths, text and visual port endpoints.

The simulator does not inspect the symbol. The symbol renderer does not evaluate logic. A visual-only library update increments the visual revision without changing the logic revision, so synchronizing the root model does not reset/rebuild the simulator for an appearance-only edit.

## 3. Visual Component Designer

The existing SDL3 definition editor now opens a dedicated symbol-design view. It uses the same application and renderer architecture: a central grid canvas, a left object/port list, a right property/help panel and a two-row toolbar.

The visual tools support:

- rectangle and rounded rectangle;
- circle and ellipse;
- line and arc;
- polygon and open/closed polyline path;
- editable text objects.

Click-based polygon/path drawing previews the unfinished contour up to the pointer and finishes with Enter. Drag drawing shows a preview until mouse release. Selected objects receive a visible outline and lower-right resize handle. Shape/path stroke width and text size have direct increment/decrement controls. The small fill/stroke palette also applies to standalone paths; exact colors and numeric coordinates remain available through symbol text editing.

Objects retain stable IDs and painter order. The editor supports move, resize, rotate, duplicate, delete, layer reorder, visibility and lock flags, grid snapping, basic alignment, and path vertex insertion/movement/removal/closure. Generated `inputN`/`outputN` suffixes are initialized above the highest existing numeric suffix and advance monotonically during the current definition-edit session; deleting a port during that session does not cause its generated ID to be reused. Candidate IDs and display names are also checked for collisions.

## 4. Ports and transformations

Visual port records are keyed by the stable logical port ID. Each stores a symbol-local position, visual orientation, label, label visibility and endpoint visibility. Logical direction remains in the `.hcscomponent` interface, independent of visual orientation.

Ports can be moved to any point inside the symbol bounds. Cardinal directions and arbitrary-angle orientation are supported. The existing custom port layout copies these coordinates into the editor's actual custom pin positions. The common symbol transform is applied to both the rendered symbol and the pin endpoints, preserving wire attachment after the existing quarter-turn rotation and horizontal/vertical flips. Moving or relabeling a visual port does not change its logical ID.

## 5. HCSS text symbol format

The symbol text is a separate UTF-8 line-oriented format with the version header `HCSS 1`; it is not confused with `.hcs` circuit text.

Canonical documents contain:

```text
HCSS 1
size width=180 height=124
shape rounded_rect id=body x=8 y=8 width=164 height=108 radius=14 start=0 end=360 points="" fill=#202630 stroke=#D0D7E2 stroke_width=2 visible=1 locked=0
text id=title x=90 y=32 size=13 color=#E0E9F4 align=center role=ui visible=1 locked=0 value="HALF ADDER"
port id=a x=8 y=48 orientation=left angle=0 label="A" show_label=1 visible=1
```

The format supports dimensions, shape primitives, paths/point lists, colors, stroke width, text, font role, alignment, visibility/lock flags, port references and layer order. Floats use classic-locale round-trip serialization. Object sequence is preserved because it defines painter order; ports are emitted in model order. Re-serializing an unchanged model is deterministic.

The parser reads into a temporary model, validates it and only then replaces the caller's destination. Errors contain a useful message and a source line where the parser can identify one.

## 6. Visual/text synchronization and definition persistence

Both modes use the same `SymbolDefinition` object.

- Entering symbol text mode serializes the current model into canonical HCSS.
- Applying text parses and validates a candidate; visual port IDs must still exactly match the logical interface IDs.
- A valid candidate replaces the existing model atomically.
- Invalid text remains in the text buffer and does not change the last valid symbol.
- Reload explicitly discards text edits and regenerates canonical text from the model.

The symbol is stored in an optional `symbol-begin` / `symbol-end` section inside `.hcscomponent`. Older HCSC definitions without the section remain loadable and receive a generated default symbol. Root `.hcs` documents still store custom definition references rather than embedding symbol or logic data into each placed instance.

## 7. Shared rendering and hit testing

`gui/symbol_renderer.cpp` and `gui/symbol_renderer.hpp` provide one renderer used by:

- custom instances in the main circuit editor;
- component-library previews;
- the Visual Component Designer canvas.

The renderer handles transformed shape contours, fills, strokes, paths, text through the existing `FontManager`, visual port endpoints, arrows and labels. Polygon fills use SDL3 `SDL_RenderGeometry` triangulation. Hit testing maps the cursor back to symbol-local coordinates and checks actual shapes/contours, ellipses, line segments and ports rather than treating the complete rectangular bounds as a hit.

Derived geometry is generated during rendering. No persistent geometry/GPU cache was added, so highly complex symbols may incur per-frame tessellation cost. This was intentionally not optimized without profiling evidence.

## 8. Examples and documentation

Five complete HCSS symbol examples are included:

- `examples/symbols/half_adder.hcssymbol`
- `examples/symbols/full_adder.hcssymbol`
- `examples/symbols/circular_component.hcssymbol`
- `examples/symbols/four_sided_ports.hcssymbol`
- `examples/symbols/polygon_contour.hcssymbol`

They cover arithmetic symbols, nested-component labels, a circular body, ports on all four sides and arbitrary polygon contours. `docs/SYMBOL_DESIGNER.md` documents the format, controls, limits and examples. README, architecture, GUI architecture, custom-component documentation, CHANGELOG and ROADMAP were updated.

## 9. Validation and safety limits

The current symbol validator enforces:

- symbol dimensions between 48 and 4096 units;
- at most 4096 objects and 256 visual ports;
- at most 1024 points per shape/path;
- finite in-bounds persisted coordinates;
- stroke widths between 0 and 64;
- bounded arc and arbitrary-angle port values;
- unique stable object IDs and unique visual port IDs;
- supported shape/orientation/alignment/font-role values;
- bounded text and port-label sizes;
- valid colors and geometry.

The HCSS text input limit is 4 MiB. The enclosing `.hcscomponent` retains its existing 16 MiB limit. Legacy files without symbol data use default geometry.

## 10. Tests and exact results

The five CTest suites are:

1. `hcs_tests`
2. `hcs_editor_tests`
3. `hcs_persistence_tests`
4. `hcs_custom_component_tests`
5. `hcs_symbol_tests`

Results on the final headless source state:

| Verification | Result |
|---|---:|
| GCC 14.2 Release CMake build + CTest | 5/5 passed |
| Clang 17 Release CMake build + CTest | 5/5 passed |
| `make test` (GCC Release build + CTest) | 5/5 passed |
| HCSS example parse/serialize determinism tests | passed within `hcs_symbol_tests` |
| Old HCSC definition without symbol block | passed within `hcs_symbol_tests` |
| Visual port ID/endpoint transform tests | passed within `hcs_symbol_tests` |
| Appearance-only update does not reset runtime | passed within `hcs_symbol_tests` |
| Negative-slope line, hit test, move/resize and round-trip regression | passed within `hcs_symbol_tests` |

The GUI source was also syntax-checked twice using temporary SDL3 declaration stubs, once with and once without `HCS_HAVE_SDL3_TTF`; both syntax checks produced zero diagnostics after adding the direct `<set>` include required by symbol text apply. This is only a source syntax check, not an SDL build or GUI test.

## 11. Real SDL3 GUI verification status

This host does not have SDL3 or SDL3_ttf development packages/headers. CMake was run with `HCS_BUILD_GUI=ON`, but it reported that SDL3 was unavailable and omitted the `hcs_gui` target. Therefore the GUI could not be linked against real SDL3, launched, or interactively tested here. The temporary stub syntax checks are not claimed as equivalent to a real GUI build. On an Arch desktop with SDL3 and SDL3_ttf installed, reconfigure with `-DHCS_REQUIRE_SDL3=ON`, build `hcs_gui` and manually test shape drawing, port placement/orientation, text apply/reload, library previews and transformed wire endpoints.

## 12. Files added and modified

Added:

- `include/hcs/editor/symbol.hpp`
- `src/editor/symbol.cpp`
- `gui/symbol_renderer.hpp`
- `gui/symbol_renderer.cpp`
- `tests/test_symbol.cpp`
- `docs/SYMBOL_DESIGNER.md`
- `examples/symbols/half_adder.hcssymbol`
- `examples/symbols/full_adder.hcssymbol`
- `examples/symbols/circular_component.hcssymbol`
- `examples/symbols/four_sided_ports.hcssymbol`
- `examples/symbols/polygon_contour.hcssymbol`
- `docs/PHASE5_5_REPORT.md`

Modified:

- `ARCHITECTURE.md`, `README.md`, `CHANGELOG.md`, `ROADMAP.md`
- `CMakeLists.txt`
- `docs/CUSTOM_COMPONENTS.md`, `docs/GUI_ARCHITECTURE.md`
- `gui/application.cpp`, `gui/renderer.cpp`
- `include/hcs/editor/custom_components.hpp`, `include/hcs/editor/model.hpp`
- `src/editor/custom_components.cpp`, `src/editor/model.cpp`, `src/editor/persistence.cpp`, `src/editor/runtime.cpp`
- `tests/test_custom_components.cpp`

## 13. Known limitations and deferred work

- Real SDL3/SDL3_ttf build, visual interaction and font-glyph rendering remain unverified on this host.
- Paths are straight-segment contours; cubic Bézier curves and control handles are not implemented.
- The property panel is not a complete numeric grid. Stroke width and text size have visual increment/decrement controls; precise coordinates, colors, alignment and font role remain edited through HCSS text.
- The color palette is deliberately small; there is no full color picker.
- There is no undo/redo command stack, peer-object snapping or persistent derived-geometry cache.
- Text hit bounds use an approximate width estimate, while actual rendering uses FontManager metrics. Text anchors follow component transforms, but glyph runs remain upright rather than rotating/mirroring with the symbol; the visual editor also does not rotate standalone text objects.

No separate simulator, new simulation backend, GPU path, SIMD optimization or unrelated benchmark experiment was introduced. All 55 benchmark/profiling-related files found by the final filename audit (data and associated documentation) were compared by SHA-256 and remain unchanged.
