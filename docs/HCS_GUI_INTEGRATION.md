# HCS GUI Integration

## Runtime construction

`CircuitRuntime::synchronize(CircuitEditor&)` translates the current editor graph into the existing `hcs::CircuitBuilder` API:

1. Allocate all source and component output signals first. Input components use `CircuitBuilder::add_input`; logic and DFF outputs use `add_signal`.
2. Resolve each connected input pin to its upstream output pin's `SignalId`.
3. Allocate a constant-zero signal for each unconnected required input.
4. Add NOT/AND/OR/XOR drivers with `add_gate`, and DFFs with `add_d_flip_flop`.
5. Build the immutable `hcs::Circuit`, construct an ordinary `hcs::Simulator` with the selected configuration, and assign the `SignalId` mapping to each editor pin.
6. Synchronize Input component values with `Simulator::set_input`.

All output IDs exist before gate drivers are added, so feedback wires do not depend on component storage order. The existing `CircuitBuilder` remains responsible for its normal validation and ordering logic.

## Mapping lifetime

Pin-to-signal mappings are stable while the editor's structural revision and selected backend remain unchanged. A structural edit or backend change rebuilds the circuit and replaces the mapping. Component geometry, selection and external Input value changes do not rebuild the circuit. Input changes are propagated through `set_input` and become observable after a scheduled/manual simulator step.

`Pin::simulation_signal` mirrors the mapped signal ID for inspection. Actual values are only queried through `CircuitRuntime::signal_for_pin`, which reads `Simulator::signal`.

## Backend choices

The editor explicitly exposes:

- Interpreted Event (`Backend::Interpreted`, event strategy)
- Interpreted Dense (`Backend::Interpreted`, dense strategy)
- Optimized IR Dense (`Backend::OptimizedIR`, dense strategy)
- Optimized IR Event (`Backend::OptimizedIR`, event strategy)
- Hybrid IR (`Backend::HybridIR`)
- Compiled Native (`Backend::Compiled`)

Compiled Native uses the existing persistent native compilation cache at `.hcs-gui-cache`. The GUI does not silently select a backend or enable compilation for the user.

## Simulation scheduling

`SimulationController` wraps `CircuitRuntime`; the GUI event/render loop never calls `Simulator::step` directly. It supports pause, manual stepping, 1/10/60/1000 Hz fixed scheduling and bounded unlimited time slices. While paused, an input or structural change schedules one propagation step so combinational outputs refresh without requiring the user to press Run. Sequential clock edges still follow the core's ordinary `step()` semantics.

## Sequential and feedback semantics

The integration layer does not evaluate gates or DFFs itself. It delegates every propagation and clock step to `hcs::Simulator`. In particular, DFF next-state sampling/commit remains simultaneous in the core, and feedback settling uses the backend selected by the user.

## API sketch

```cpp
hcs::editor::CircuitEditor editor;
const auto source = editor.add_component(hcs::editor::ComponentType::Input, {0, 0});
const auto inverter = editor.add_component(hcs::editor::ComponentType::Not, {160, 0});
const auto sink = editor.add_component(hcs::editor::ComponentType::Output, {340, 0});

// Connect the source output -> inverter input -> sink input using the pin IDs
// obtained from the corresponding components.
hcs::editor::CircuitRuntime runtime(hcs::editor::RuntimeBackend::OptimizedIREvent);
runtime.synchronize(editor);
(void)runtime.step();
```

The SDL renderer reads model geometry and asks the runtime for pin values. It has no access to `Simulator` internals.


## Circuit persistence boundary (Phase 5.3)

The `.hcs` reader creates an `EditorModel`-level graph from stable component IDs, component types, positions, Input values, DFF initial-state properties and symbolic pin connections. `PinId`, `WireId`, `SignalId`, runtime backend state, simulator tick counters and native cache entries are not written to disk. After the validated model is installed, the existing `CircuitRuntime::synchronize()` path allocates fresh signals and builds a normal `Circuit`/`Simulator` with the currently selected backend and acceleration configuration.

Parsing/validation and temporary model construction complete before `CircuitEditor::replace_with()` changes the active document. A failed parse leaves the current graph intact. The same parse path is used by the GUI's Apply Text action. The CLI `hcs_circuit validate` invokes the same reader without requiring SDL3. See [`CIRCUIT_FORMAT.md`](CIRCUIT_FORMAT.md).
