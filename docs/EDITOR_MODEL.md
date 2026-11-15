# Editor Model (Phase 5)

Namespace: `hcs::editor`.

## Identifiers and world space

`ComponentId`, `PinId` and `WireId` are monotonically assigned 32-bit IDs. A component stores a world-space `Vec2 position`, a logical `Vec2 size`, input/output pin vectors, and an external `input_value` only for Input components. Camera zoom/pan never changes model coordinates.

A `Pin` stores its owner, direction, local component-space position and a `simulation_signal` mapping. That mapping is a `SignalId` in the currently synchronized runtime; it is not a copy of the signal's logic value. The runtime remains authoritative for signal state.

A `Wire` is an output-pin ID and an input-pin ID. The editor rejects direction mismatches and more than one incoming wire per input pin. Output fanout is unrestricted. The model does not implement routing; the renderer joins pin endpoints with a straight line.

## Initial component pin layouts

| Component | Inputs | Outputs |
|---|---|---|
| Input | — | 1 |
| Output | 1 | — |
| AND | 2 | 1 |
| OR | 2 | 1 |
| NOT | 1 | 1 |
| XOR | 2 | 1 |
| D Flip-Flop | D, CLK | Q |

DFF clock is a normal one-bit input pin. An Input component can drive it. Positive-edge triggering and simultaneous DFF updates are provided by the existing HCS simulator.

## Structural revision

`CircuitEditor::structural_revision()` changes when components or wires are added/removed and when DFF initial state changes. A separate `input_revision()` changes only when an Input component value changes. Moving components change `document_revision()` but not the structural or input revisions, so they mark the document modified without rebuilding HCS. Selecting objects changes no document revision. `CircuitRuntime` uses structural/input revisions to avoid rebuilding HCS for camera movement, selection, rendering or input-level changes.

`document_revision()` advances for persistent graph edits: component/wire insertion or removal, movement, Input value changes and DFF initial-state changes. GUI save state compares it with the revision recorded after a successful load/save. `replace_with()` installs a complete validated model while advancing the destination's revisions, ensuring an already-synchronized `CircuitRuntime` rebuilds even when the new graph has a similar shape.

## Persistence identifiers

The `.hcs` file stores stable numeric component IDs, but it does not serialize `PinId`, `WireId` or `SignalId`. Connections use component ID plus a symbolic port name (`out`, `in0`, `d`, `clk`, etc.). Pin IDs are regenerated from the fixed component pin layout when parsing. See [`CIRCUIT_FORMAT.md`](CIRCUIT_FORMAT.md) for the complete version-1 schema and validation rules.

The editor currently supports one selected component or one selected wire. Deleting a component also removes attached wires.

## Incomplete circuits

Users can place components before completing their wiring. During synchronization, every unconnected required gate/DFF input receives a dedicated constant-zero signal. This provides deterministic behavior for incomplete circuits without inventing a second simulation model. Adding/removing a wire changes the structural revision and rebuilds the HCS circuit.

## Selection and hit testing

The model contains component rectangle, pin-radius and point-to-wire-segment hit testing helpers. Screen-to-world conversion belongs to `Camera2D`; the model hit tests always receive world coordinates.
