# Visual Component Designer and Symbol Format (Phase 5.5)

## Purpose and separation of concerns

A custom component has three independent layers:

1. **Logic** — the internal `CircuitEditor` implementation and the existing custom-component hierarchy/runtime rules.
2. **Interface** — stable logical input/output port IDs and directions stored in the `.hcscomponent` definition.
3. **Appearance** — a `SymbolDefinition` made of vector shapes, paths, text objects and visual port locations.

The symbol renderer never evaluates logic. The simulator never reads shape geometry or text labels. Renaming a symbol label, moving a visual port or changing a shape does not change the definition ID, the internal circuit or a logical port ID. A symbol-only library update increments the visual catalogue revision but not the logic revision; `CircuitRuntime` therefore does not rebuild or reset the simulator for appearance-only edits.

The visual port records in HCSS are matched by stable ID to the logical interface ports in the enclosing `.hcscomponent`. Direction remains in the logical interface (`port input` / `port output` in HCSC), not in the visual symbol record. Applying a symbol whose visual port ID set differs from the logical interface is rejected.

## Visual Component Designer

Open a custom component definition using **EDIT DEF**, then select **SYMBOL**. The designer contains a central grid canvas, a left object/port list and a right properties/help panel. The canvas, main editor and component-library thumbnail use the same `SymbolRenderer` implementation.

### Drawing and editing

- **RECT**, **ROUND**, **CIRCLE**, **ELLIPSE**, **LINE** and **ARC**: drag on the canvas to create the shape.
- **POLYGON** and **PATH**: click to add vertices, then press Enter to finish. A polygon is closed; a path starts open.
- **TEXT**: click the canvas to add a text object and enter its text. Select it and press **E** to edit its contents.
- **SELECT**: click an object or port to select it; drag to move it. Drag the lower-right selection handle to resize supported objects.
- The selected object is outlined on the canvas and its lower-right resize handle is visible. Dragging the handle resizes the shape, path, or text object while keeping geometry inside the symbol viewport.
- **P** with a selected polygon/path: enter vertex editing. Drag a vertex handle to move it; Ctrl+click a segment to insert a vertex; select a vertex and press Delete to remove it. A path must retain at least two points and a closed path at least three.
- **C** toggles an open path between open and closed when the selected object is a path.
- **R** rotates selected vector geometry by 90 degrees. Text objects are not rotated by this command.
- **D** duplicates the selected object. **[** and **]** change its painter order; **Home** and **End** send it to the back/front.
- **V** toggles visibility, **L** toggles the editing lock and **F** toggles fill on a selected shape.
- **Shift+Left/Right/Up/Down** aligns the selection to the corresponding canvas edge; **Shift+H/V** aligns its horizontal/vertical centre.
- **G** or **SNAP** toggles grid snapping. **ZOOM +/−** or the mouse wheel changes designer zoom; middle-button drag pans the designer view.

The fill/stroke toolbar buttons cycle a small built-in palette. **WIDTH − / WIDTH +** adjust the selected shape/path stroke in 0.5-unit increments; **SIZE − / SIZE +** adjust selected text in 1-unit increments. Exact fill/stroke colors, coordinates, text font role/alignment and all other stored properties can be edited in the HCSS source view. While drawing a polygon or path, the canvas shows a live preview from the last vertex to the pointer; drag-created shapes also show a preview before release.

### Ports

Use **+INPUT** / **+OUTPUT** to add logical interface ports to the definition. Generated `inputN` / `outputN` IDs advance monotonically during the current definition-edit session and skip collisions with existing IDs or names. This operation creates the corresponding internal terminal and visual port together. Select a port and drag its endpoint anywhere inside the symbol viewport; its position is not constrained to a side. The initial orientation is inferred from the nearest edge. Press **O** to cycle Left, Right, Top and Bottom. Press **Shift+O** to use arbitrary-angle orientation; Left/Right then change that angle by 15 degrees. Press **E** to edit the visual port label. Delete removes the selected logical port through the definition API, rather than leaving a visual-only orphan.

Port direction and orientation are different: `input`/`output` is the electrical interface direction; `left`/`right`/`top`/`bottom`/`angle` describes only how the endpoint is drawn. Moving or rotating an instance transforms the symbol geometry and the actual pin endpoint using the same model transform, so wires remain attached to that endpoint. Text anchors move with the symbol, but glyphs remain upright for legibility; text glyphs are not rotated or mirrored with the instance.

## HCSS text format, version 1

The symbol source is a UTF-8 line-oriented document with the distinct **`HCSS 1`** header. Blank lines and lines beginning with `#` are ignored. Floating-point numbers use the classic locale and deterministic round-trip precision. Quoted strings use the C++ `std::quoted` escaping convention. Point lists are quoted `x,y;x,y;...` sequences.

Canonical record order is the model's painter order (back to front), followed by visual ports in their model order. The serializer does not sort objects by ID because object order defines z-order; repeated serialization of an unchanged model is byte-identical.

### Document records

```text
HCSS 1
size width=180 height=124
```

`size` is required and occurs once. Both dimensions are finite and in the inclusive range 48–4096 symbol units.

### Shape records

```text
shape rounded_rect id=body x=8 y=8 width=164 height=108 radius=14 start=0 end=360 points="" fill=#202630 stroke=#D0D7E2 stroke_width=2 visible=1 locked=0
shape polygon id=contour x=0 y=0 width=0 height=0 radius=0 start=0 end=360 points="45,8;145,18;184,72;142,132;38,142;8,74" fill=#26384C stroke=#8DB8E8 stroke_width=3 visible=1 locked=0
```

Supported shape types are `rect`, `rounded_rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, `arc` and `path`. All shape records carry the same fields; fields not relevant to a shape type are still serialized to keep the grammar simple. `points` is empty for parameterized primitives, and is a point list for polyline/polygon/path contours. Polygon requires at least three points; polyline/path requires at least two. `arc` uses `start` and `end` angles in degrees and the ellipse described by x/y/width/height.

### Standalone editable paths

```text
path id=wireless_contour points="20,20;80,10;120,60;70,100" closed=1 fill=none stroke=#8DB8E8 stroke_width=2 visible=1 locked=0
```

A standalone path has two or more points when open and three or more when closed. Current paths are editable straight-segment polylines; cubic Bézier control handles are not implemented.

### Text records

```text
text id=title x=90 y=32 size=13 color=#E0E9F4 align=center role=ui visible=1 locked=0 value="HALF ADDER"
text id=subtitle x=90 y=65 size=11 color=#AFC5DD align=center role=code visible=1 locked=0 value="A XOR B"
```

`x` and `y` are the text anchor: for `align=left`, x is the left edge; for `center`, x is the horizontal centre; for `right`, x is the right edge. y is the top of the text run. `role` is `ui` or `code` and selects the centralized FontManager role. The renderer uses FontManager metrics for actual drawing; editor hit bounds use a conservative text-width estimate. Unicode can be stored and is displayed when the selected SDL3_ttf/system font supports the glyphs.

### Visual ports

```text
port id=a x=8 y=48 orientation=left angle=0 label="A" show_label=1 visible=1
port id=data x=145 y=18 orientation=angle angle=-25 label="DATA" show_label=1 visible=1
```

The `id` must match one logical `.hcscomponent` port ID exactly. The visual record contains no direction field. The enclosing HCSC logical port determines input/output direction. Valid orientations are `left`, `right`, `top`, `bottom` and `angle`; arbitrary angle is read from `angle` in degrees.

### Style and visibility

Colors are `#RRGGBB` or `#RRGGBBAA`; `none` is allowed for fill/stroke where meaningful. Stroke width is finite in the range 0–64. `visible` and `locked` are boolean `0`/`1` fields on objects; visual ports have `show_label` and `visible` fields.

## Validation and safety limits

The HCSS parser validates the complete temporary symbol before assigning it to the caller. An invalid document leaves the previous symbol model untouched and returns a `SymbolError` with a line number where available. The current limits are:

- HCSS input: 4 MiB maximum.
- Symbol dimensions: 48–4096 units per side.
- Objects: at most 4096; visual ports: at most 256 (the parser also caps total records at 4096).
- Point list: at most 1024 points per shape/path.
- Text string: at most 16,384 bytes; visual port label: at most 256 bytes.
- Coordinates must be finite and within the symbol viewport for persisted geometry; the generic coordinate parser rejects magnitudes above 1,000,000.
- Stroke width: 0–64; arc and arbitrary port angles: −3600 to +3600 degrees.
- Object and port IDs must be unique within their respective namespaces and use ASCII letters, digits, `.`, `_` or `-`.
- Unsupported versions, shape types, alignments, font roles, orientations, fields and malformed point lists are rejected.

The enclosing `.hcscomponent` file retains its existing 16 MiB file limit and HCSC 1 header. A missing symbol block is backward compatible: the parser constructs the default rectangle symbol and positions its visual ports from the existing logical interface.

## Persistence and runtime integration

`.hcscomponent` stores the symbol between `symbol-begin` and `symbol-end`, before the existing `implementation` section. The logic implementation and interface remain in their existing HCSC fields. Root `.hcs` files continue to store only the custom definition reference and instance connections; the symbol is not copied into each instance.

Changing only the symbol does not change the logical library revision and does not cause `CircuitRuntime` to rebuild. Changing logical ports or the internal circuit follows the existing definition validation/migration path and may rebuild the runtime. The renderer uses visual port positions to produce `CustomPortLayout` pin local positions. The circuit model's existing rotation/flip transform is applied to both symbol points and pin world positions.

## Shared rendering and hit testing

`gui/symbol_renderer.cpp` contains the shared SDL3 renderer used by the main circuit canvas, component designer and component-library previews. Filled contours are tessellated for `SDL_RenderGeometry`; linework and port endpoints are drawn through the same renderer. Port endpoints and labels are rendered above decorative geometry. Main-canvas hit testing first applies the component's broad-phase bounds and then tests actual symbol geometry in local coordinates; it does not treat every point inside a symbol's bounding rectangle as a hit.

Derived shape geometry is currently created during drawing rather than held in a persistent GPU/CPU geometry cache. This keeps the model renderer-independent and avoids stale geometry after text or visual edits, but very complex symbols may incur per-frame tessellation cost. No GPU backend or speculative rendering optimization is introduced in this phase.

## Examples

Complete standalone HCSS examples (each parses and reserializes deterministically in `hcs_symbol_tests`):

- [`half_adder.hcssymbol`](../examples/symbols/half_adder.hcssymbol)
- [`full_adder.hcssymbol`](../examples/symbols/full_adder.hcssymbol)
- [`circular_component.hcssymbol`](../examples/symbols/circular_component.hcssymbol)
- [`four_sided_ports.hcssymbol`](../examples/symbols/four_sided_ports.hcssymbol)
- [`polygon_contour.hcssymbol`](../examples/symbols/polygon_contour.hcssymbol)

These are visual documents, not standalone logic definitions. Put the symbol block in an HCSC definition or import it through the component's Symbol Designer to use it in a circuit.

## Current limitations

- No undo/redo is included; the data model is stable-ID based and remains suitable for a future command stack.
- Paths are straight-segment vector contours; no Bézier handles/curves are present.
- Exact coordinates, arbitrary colors, text alignment/role, and other detailed properties remain editable in HCSS text mode. The visual inspector is not a full numeric property grid.
- The visual toolbar's FILL/STROKE controls cycle a small palette; stroke width and text size have direct increment/decrement controls, while arbitrary colors and exact values are edited in HCSS.
- The canvas currently offers snapping to a regular grid; there is no snap-to-peer-object/contour-edge solver.
- Text hit bounds are estimated from string length and font size, while drawing uses FontManager's actual metrics. Text anchors follow instance transforms, but glyphs stay upright and are not rotated or mirrored.
- No geometry cache is currently retained between frames.
- The GUI is not compiled/linked or interactively tested against real SDL3 in this build environment because SDL3 development files and a graphical session are unavailable. Model, persistence and transformation tests are headless and do not claim to verify mouse interaction or real font rendering.
