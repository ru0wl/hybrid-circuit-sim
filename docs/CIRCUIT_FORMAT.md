# HCS Circuit Text Format

The official editable circuit-file extension is **`.hcs`**. The current format version is **1**. The format stores the editor graph, not an `hcs::Circuit`, `Simulator`, pointer, internal signal ID, backend choice or runtime cache.

## Syntax

A document is UTF-8 text with one statement per line. Blank lines and `#` comments are ignored. The first non-comment statement is the version header:

```text
HCS 1
circuit "Circuit name"

component <id> <type> x=<number> y=<number> [value=0|1] [initial=0|1]
wire <source-component>.<source-pin> -> <destination-component>.<destination-pin>
```

The circuit name is quoted. The parser accepts `\\`, `\"`, `\n`, `\r` and `\t` escapes inside the name. Component IDs are unsigned decimal 32-bit values; `4294967295` is reserved. Positions are finite decimal floating-point values in the inclusive range `[-10000000, 10000000]`.

### Supported component types and pin names

| Type token | Input pins | Output pins | Component-specific field |
|---|---|---|---|
| `Input` | — | `out` | `value=0` or `value=1` |
| `Output` | `in` | — | — |
| `AND` | `in0`, `in1` | `out` | — |
| `OR` | `in0`, `in1` | `out` | — |
| `NOT` | `in` | `out` | — |
| `XOR` | `in0`, `in1` | `out` | — |
| `DFF` | `d`, `clk` | `q` | `initial=0` or `initial=1` |

`value` and `initial` are optional when reading and default to `0`; the canonical writer always emits them for the applicable component types. `value` is only valid on `Input`, and `initial` is only valid on `DFF`. Pin names are symbolic model ports, not runtime `SignalId` values. A DFF's `clk` pin is connected and evaluated by the existing simulator in exactly the same way as before this format was introduced.

A single input pin may have at most one incoming wire. Output fanout is unrestricted. An unconnected input remains valid; the existing `CircuitRuntime` ties such an input to logic zero when building the simulator.

## Complete example

[`examples/xor.hcs`](../examples/xor.hcs) is a full file. Its contents are:

```text
HCS 1
circuit "XOR example"

component 10 Input x=80 y=150 value=0
component 20 Input x=80 y=310 value=1
component 30 XOR x=320 y=215
component 40 Output x=560 y=225

wire 10.out -> 30.in0
wire 20.out -> 30.in1
wire 30.out -> 40.in
```

## Canonical serialization

The writer sorts components by stable component ID and wires by their symbolic endpoint representation. Property order is fixed (`x`, `y`, then `value` or `initial`), and positions are written with enough decimal precision to round-trip a C++ `float`. Wire IDs, pin IDs, storage order, memory addresses and runtime signal IDs are not serialized. Pin IDs are re-created from the component layouts when loading; connections resolve by stable component ID and symbolic pin name.

Formatting a file and saving the same unchanged model produce the same canonical text. Moving a component changes only its serialized coordinates; changing input or DFF initial state changes the corresponding field.

## Validation and safety

Parsing is strict and reports the source line when applicable. It rejects unsupported versions, malformed/unknown statements, unquoted circuit names, duplicate component IDs/fields, unknown component types/fields, missing coordinates, invalid/non-finite/out-of-range numbers, illegal state fields, missing component references, nonexistent or directionally invalid pins, and multiple wires targeting one input. File loading is limited to 16 MiB.

Parsing and model construction happen in a temporary `CircuitEditor`. The active editor model and circuit name are replaced only after the entire document has parsed, validated and constructed successfully. A failed parse leaves the current model unchanged. File saving writes a sibling `.tmp` file and atomically renames it over the destination after a successful write.

## CLI

The `hcs_circuit` executable validates or canonicalizes files without SDL:

```bash
./build/hcs_circuit validate examples/xor.hcs
./build/hcs_circuit print examples/xor.hcs
./build/hcs_circuit format examples/xor.hcs formatted.hcs
```

Validation returns exit code `0` for a valid file and non-zero for errors. Diagnostics go to stderr; successful status/format output goes to stdout in English.

## Versioning and extension boundary

The header version identifies the on-disk schema, not the simulator version. A future incompatible schema must use a new format version and explicit migration/compatibility handling; version 1 readers reject unknown versions rather than guessing. New fields must be added with documented semantics and strict type/component applicability. Custom/user-defined component types, nested subcircuits and component libraries are deliberately not represented by version 1 and remain future work.
