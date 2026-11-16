# HCS Architecture and implementation audit

## Current architecture

HCS is a one-bit digital logic simulator. The simulation-core architecture described in this document predates the Phase 5 editor layer; the current editor boundary is documented at the end of this file. `CircuitBuilder` creates a compact immutable `Circuit` containing signal IDs, primary-input IDs, a contiguous gate vector, a stable execution-order vector, and D flip-flop descriptors.

### Existing implementation audit

1. **Circuit representation:** `Circuit` stores a signal count and contiguous vectors for inputs, gates, execution order, and DFFs. IDs are 32-bit integers.
2. **Components and connections:** each `Gate` stores an opcode, up to two input signal IDs, and one output signal ID. Each DFF stores D, clock, Q, and initial state. At the simulation-core layer there is no general editor-component object model; Phase 5 adds a separate `hcs_editor` model above it.
3. **Signals:** runtime values are normalized `uint8_t` values in a vector indexed by `SignalId`. There are no X/Z values or multi-driver resolution.
4. **Dependencies:** `CircuitBuilder::build()` maps each driven signal to its gate, derives gate dependencies from input signal drivers, removes duplicate dependency edges, and builds dependent lists.
5. **Topological sorting:** Kahn's algorithm uses a min-priority queue, making tie order stable by gate ID. Nodes not emitted by Kahn are appended in gate-ID order.
6. **Feedback:** cyclic gates are not explicitly grouped in the original runtime. Dense execution repeatedly scans the order until stable or the pass limit. Event execution uses a FIFO queue and a per-settle event budget. Native execution also repeats dense passes. Oscillation is limit-detected, not classified by cycle-state detection.
7. **DFFs:** `Simulator::step()` settles combinational logic, samples every rising-edge D input into a private simulator-owned scratch vector, updates all Q values simultaneously, and settles again if any Q changed. The scratch vector is allocated/resized outside the repeated step path and reused. Clock history is updated each step.
8. **Event scheduling:** a signal's fan-out is precomputed. Changed inputs schedule dependent gates; a `queued` bit suppresses duplicates. The queue is FIFO and is drained with a head index.
9. **C++ generation:** the previous generator emitted one C++ statement per gate in blocks of 64 helper calls, with a dense fixed-point loop. The current generator accepts compiler IR and emits the optimized IR operation sequence in bounded-size helper blocks. It does not yet schedule separate native functions per dependency region.
10. **Native compilation/loading:** the compiler is launched with `fork`/`exec`; it builds a PIC shared object. The loader uses `dlopen(RTLD_NOW|RTLD_LOCAL)` and resolves `hcs_eval` with `dlsym`. This remains Linux-specific.
11. **Cache validation:** the key includes generated source, compiler executable name/version, optimization flag, architecture tag, and native ABI version. A text manifest is checked and the `.so` is hashed with FNV-1a before `dlopen`. This detects accidental corruption, not malicious cache replacement.
12. **Existing optimizations:** stable topological order; event-driven fan-out scheduling and duplicate-queue suppression; generated-code block splitting; compiler optimization flags. The original backend had no whole-circuit Boolean optimizer.
13. **Execution overhead:** dense and native paths scan every gate per settle pass; event-driven cost is queue operations, fan-out traversal, scheduling, and gate evaluation. DFF next-state storage is reused across calls to `step()`, preserving simultaneous commit semantics without a per-step vector allocation. Actual dominant cost is workload-dependent; Phases 4 and 4.1 add opt-in runtime counters and benchmark-based analysis, while hardware-counter profiling remains pending on a host with `perf` or Callgrind.

## Compiler pipeline

```text
Circuit
  -> IR lowering
  -> dependency graph + Tarjan SCC analysis
  -> configurable IR passes
  -> optimized IR
      -> OptimizedIR interpreter (no native compiler)
      -> C++ code generator -> C++ compiler -> .so -> dlopen
```

### IR model

`hcs::ir::CircuitIR` owns compact vectors for operations, primary inputs, observable signals, and sequential state elements. `Operation` carries opcode, input/output signal IDs, original gate/component ID, and region ID. Sequential state is separate in `StateElement`, with D, clock, Q, initial value, and source identity. Every current signal is marked observable because `Simulator::signal(id)` publicly exposes all signal values.

The current circuit supports only one-bit signals and at most two-input gates. Multi-input gates and buses are not represented because the source simulator does not implement them.

### Dependency analysis

`hcs::analysis::analyze()` builds predecessor/successor adjacency lists from signal drivers and uses Tarjan's algorithm to find strongly connected components. Cyclic SCCs are marked and excluded from transformations that assume acyclic dataflow. Connected combinational operations are grouped into larger weakly connected regions, avoiding one-region-per-gate fragmentation. DFFs are represented as separate sequential-boundary regions, with state producer/consumer dependencies retained. The analysis also reports backward reachability from observable outputs/state, unused unobservable outputs, exact shared-expression groups, and a critical-path metric over the acyclic SCC condensation edges.

### Optimization passes

Passes are independently configurable and ordered by `optimizer::Options`:

- Constant folding and propagation: evaluate known constant expressions and simplify identities.
- Boolean simplification: handle repeated operands, including `x AND x`, `x XOR x`, `x NAND x`, and `x NOR x`.
- Common-subexpression elimination: replace duplicate acyclic expressions with a forwarding operation so every original signal remains observable.
- Copy propagation: forward aliases into downstream acyclic operations while retaining the original output driver.
- Dead logic elimination: traces backwards from `observable_signals` and sequential inputs, then removes unobservable operations. The current `Circuit` lowering marks every signal observable because of the public signal-inspection API, so this pass removes nothing for ordinary simulator circuits; it works when an IR producer supplies a narrower observable-output contract.
- Operation fusion: intentionally disabled by default and reports a skip reason. Intermediate signal values are observable, so fusing away their writes is not semantics-preserving in the present API. A future explicit output/inspection contract is needed before this can be enabled safely.

Cyclic SCC operations are not rewritten by the current passes. Source component IDs remain attached to rewritten operations and are retained as a source map by the loaded native module. For normal `Circuit` lowering, operation count is preserved because all signals are observable; a narrower IR output contract allows DCE to compact the operation vector.

## Simulation semantics and compatibility

The original `Interpreted` Dense and EventDriven modes are retained. `OptimizedIR` is an additional opt-in backend with dense and direct event-driven strategies selected through `InterpreterStrategy`. `HybridIR` is an experimental region-dirty executor that operates directly on optimized IR; acyclic regions use one topological pass and feedback regions use local FIFO operation queues. `Compiled` explicitly prepares optimized IR and uses the existing native shared-library loader. DFF edge detection and simultaneous state commit remain in the common `Simulator` core for all backends.

## Known limitations

- One-bit signals and binary gates only.
- No explicit primary-output declaration: all signal IDs are observable.
- No operation fusion; ordinary simulator circuits do not benefit from DCE because every signal is observable.
- No incremental compile, SIMD, multithreading, buses, or nested custom components in the simulation core.
- `HybridIR` has topology-dependent performance and remains experimental; weakly connected regions can be large, high fanout/activity can make dirty scheduling more expensive than dense execution, and feedback behavior follows optimized-IR event ordering.
- Tarjan traversal is recursive; exceptionally deep graphs may require an iterative implementation.
- Native module loading/building remains Linux-specific.

## Phase 3: IR event-driven execution and observability

`Backend::OptimizedIR` now has two internal strategies selected by `InterpreterStrategy`:

- `Dense`: repeated passes over optimized IR operations.
- `EventDriven`: a direct IR executor with a precomputed signal-to-operation fanout table, FIFO queue and per-operation scheduled flags.

The event executor propagates only changed outputs. D flip-flop state remains committed by the common simulator step boundary; changed `Q` signals are then injected into the IR event queue. Cyclic SCCs intentionally retain queue-based repeated propagation and the existing `max_events_per_settle` convergence limit; no DAG-only shortcut is applied to cyclic logic.

### Signal observability

Every current circuit signal is observable through `signal(id)`. Consequently an optimization may rewrite a computation but must still preserve a driver/value for every observable signal. DCE is therefore normally unable to remove operations from the public circuit IR. Forwarding operations are used for safe copy/redundancy rewrites. Future compiler-generated temporary values should be distinguished from logical observable signals, but that is not an implicit release-mode semantic change.

## Phase 4: benchmark reliability and runtime profiling

### Benchmark lifecycle

`hcs_bench` creates each circuit once, records circuit-construction time, and uses the same input pattern and iteration count for every backend in a scenario. A warmup simulator is separate from the measured simulator, so warmup never changes the measured initial state. The harness can calibrate a common iteration count per scenario against a minimum sample duration. It records raw samples and summary statistics.

Native execution is split into `compiled-native-cold` and `compiled-native-warm`. Cold samples use an isolated cache and must report a validated cache miss plus a non-zero compiler invocation time. Warm samples prime the cache before measurement and must report validated hits. Interpreted backends never use the native cache. The benchmark compares a checksum over all public signal values at the end of each sample and rejects backend rows that differ from the interpreted-event reference.

### Runtime profiling

`SimulatorConfig::enable_profiling` enables optional counters. With profiling disabled, the simulator does not collect per-operation timestamps or increment profiling counters. `Simulator::execution_profile()` exposes:

- Dense/event operation counts.
- Events enqueued and processed.
- Duplicate scheduling attempts and dependent scheduling attempts.
- Signal writes, actual changes, and unchanged writes.
- Settle-call count and maximum pending queue length.
- Queue enqueue/dequeue timings and counts.
- Exclusive fan-out traversal, scheduling, signal update, and operation evaluation timings.
- Fan-out edge visits, scheduling attempts, duplicates, successful schedules, and operation evaluations.
- Settle calls/iterations, maximum iterations per settle, queue maximum length, and oscillation-limit hits.

The profiling API is still opt-in and micro-timings are diagnostic. Queue append/dequeue, scheduling, evaluator, and signal update timings are separately accounted; fan-out traversal subtracts nested scheduling and append intervals. Initial event queue seeding is included in queue counters. Use `--profile` to understand relative work and event counts, not as the primary throughput benchmark. See `BENCHMARKS.md` for field definitions and known timing limitations.

`CompilerStatistics` now includes signal and IR counts, opcode histogram, dependency edge count, SCC/cyclic-SCC count, sequential operation count, and maximum operation fan-out. Native timing data also records generated source bytes and shared-library bytes.

### Dense executor optimization

The optimized IR dense/event executors now evaluate `ir::Operation` directly through a local inline opcode switch. The earlier implementation materialized a temporary `Gate` and called the out-of-translation-unit `evaluate_gate()` for every IR operation. The new path avoids that temporary and call boundary. The original interpreted backend remains unchanged. Differential tests cover all supported opcodes and randomized sequential circuits.

### Memory metrics

The benchmark records `/proc/self/status` `VmRSS` snapshots after simulator setup and after runtime. These are process-wide resident-set snapshots, not exact per-object allocations; allocator retention and already loaded modules can affect them. Native source and `.so` sizes are reported separately from RSS. Unsupported systems report zero for the RSS fields.


## Phase 4.2: profiling-guided runtime investigation

The Phase 4.2 source audit found no single unambiguous runtime hot-path change that justified altering the Phase 4.1 architecture. The original interpreted event queue is a reusable vector FIFO with a head index and duplicate-scheduling flags; interpreted fanout remains per-signal contiguous dependency lists. The IR event backend uses byte flags. Both dense interpreters repeatedly traverse their execution order until settle. Optimized IR already evaluates `ir::Operation` directly through its local opcode switch, while native evaluation is generated specialized C++.

`enable_profiling` remains false by default. Detailed profiling is intentionally intrusive because it reads `steady_clock` around fine-grained operation, queue, scheduling, fanout, and signal-update activity. The Phase 4.2 fixed-workload A/B measured a substantial penalty only when profiling was enabled. The unprofiled paths do not perform per-operation clock reads. Additional split profiled/unprofiled loops were tested for the remaining backends but not retained because end-to-end gains were small, mixed, and not repeatable across workloads.

Three independent candidates were rejected after measurement: split profile paths, byte-sized interpreted-event scheduled flags, and CSR fanout storage. The final runtime implementation and public headers are unchanged from Phase 4.1. See [`docs/PHASE4.2_AUDIT.md`](docs/PHASE4.2_AUDIT.md) and [`docs/PERFORMANCE_REPORT_PHASE4.2.md`](docs/PERFORMANCE_REPORT_PHASE4.2.md) for evidence and exact limitations.

The profile's component timers are not hardware performance counters and may overlap; they must not be summed as exclusive percentages. Hardware PMU tools were unavailable. GCC compiler-vectorization diagnostics did not show the main simulator loops as suitable for automatic SIMD, so scalar execution remains the only runtime path.


## Phase 4.3: region-dirty hybrid execution

`Backend::HybridIR` (`optimized-ir-hybrid`) is a separate executor over optimized IR. It does not alter `OptimizedIR` Dense/EventDriven or `Compiled` behavior. Region metadata is recalculated from the optimized operation vector because optimization can change operation indices.

The outer scheduler queues unique dirty regions. Acyclic region operation lists are topologically ordered at construction and evaluated once per dirty activation. Feedback regions use a reusable local FIFO of dirty operations; output changes enqueue successor operations with duplicate suppression. The region is not treated as a DAG merely because the analyzer exposes a region object: each component may contain multiple SCCs.

The common DFF two-phase capture/commit logic is unchanged. After every next state has been captured and all Q values committed, changed Q outputs schedule their consumer regions/feedback operations. Public signal values continue to live in the shared signal array and every signal ID remains observable.

The full research, counter definitions, workload results, and known limitations are in [`docs/PHASE4.3_RESEARCH.md`](docs/PHASE4.3_RESEARCH.md) and [`docs/PERFORMANCE_REPORT_PHASE4.3.md`](docs/PERFORMANCE_REPORT_PHASE4.3.md). The backend is opt-in; no auto-selection threshold or default change is made.


## Phase 5 GUI and editor boundary

Phase 5 adds the separate `hcs_editor` library and optional SDL3 `hcs_gui` executable without changing the simulation core. `CircuitEditor` stores world-space component geometry, pins, wires and selection. `CircuitRuntime` is the only layer that translates this model into `CircuitBuilder`/`Simulator`, maintains editor-pin to `SignalId` mappings, and exposes live values to the renderer. `SimulationController` schedules calls to the real `Simulator` independently from rendering logic. `Camera2D` owns screen/world transforms. The SDL renderer is a read-only consumer of the model/runtime APIs.

The existing `hcs` CMake link name is preserved as an alias of the explicit `hcs_core` target. `hcs_editor` has no SDL dependency and can be built/tested headlessly. `hcs_gui` is only added when SDL3 is found. See `docs/GUI_ARCHITECTURE.md`, `docs/EDITOR_MODEL.md`, and `docs/HCS_GUI_INTEGRATION.md`.


## Phase 5.2: optional hardware execution paths

Phase 5.2 leaves the existing backend architecture intact. `AccelerationConfig` is independent of `Backend`; the new acceleration implementation is wired only into the opt-in `HybridIR` executor. `SimdMode::Off` and `SimdMode::Auto` currently execute scalar code (`Auto` remains scalar because no repeatable AVX2 throughput gain was established). Explicit `SimdMode::AVX2` uses a separately compiled x86 GCC/Clang translation unit. CMake applies `-mavx2` only to `src/hardware/simd_avx2.cpp`; runtime CPU/OS feature detection is required before the kernel can be selected.

For acyclic HybridIR regions, construction precomputes topological levels and opcode-homogeneous batches. Batches of 32 or more operations gather their operands by signal ID, evaluate 32 byte lanes with AVX2, and scatter to the original observable output signals. Other operations and all cyclic regions remain scalar. The core still stores binary `uint8_t` values only.

An opt-in per-simulator persistent `WorkerPool` can execute multiple independent acyclic weak components concurrently. Dispatch uses static contiguous region ranges; the caller participates as a worker. The pool is used only when all queued regions are acyclic, the batch fits the event budget, the region count is at least twice the worker count, and total queued operations are at least 4,096. Any feedback region or smaller workload takes the existing serial path. DFF capture and simultaneous Q commit stay in the common serial `Simulator::step()` implementation. Per-region SIMD statistics are accumulated locally and merged after the worker barrier.

No Vulkan compute executor, shaders, device enumeration or GPU memory transfer path is present. GPU capability fields remain false, the GUI reports GPU unavailable, and requesting GPU execution fails explicitly. No placeholder CMake switch is provided. See [`HARDWARE_ACCELERATION_AUDIT.md`](HARDWARE_ACCELERATION_AUDIT.md), [`docs/HARDWARE_ACCELERATION.md`](docs/HARDWARE_ACCELERATION.md), [`docs/SIMD_IMPLEMENTATION.md`](docs/SIMD_IMPLEMENTATION.md), [`docs/MULTICORE_EXECUTION.md`](docs/MULTICORE_EXECUTION.md), and [`docs/GPU_COMPUTE.md`](docs/GPU_COMPUTE.md).

The Phase 5.2 20-repetition A/B benchmark did not establish a reliable AVX2 or multicore speedup across the tested workloads. Both paths therefore remain opt-in and are not selected by automatic heuristics. Historical Phase 4/4.1/4.2/4.3 and Phase 5 artifacts remain unchanged.


## Phase 5.3: editor document persistence

The version-1 `.hcs` text format belongs to `hcs_editor`, above the simulator boundary. It stores circuit name, stable component IDs/types/positions, Input values, DFF initial values and symbolic component-pin connections. It deliberately excludes internal `SignalId`, `WireId`/`PinId` allocations, backend choice, simulation ticks, native module cache paths and memory addresses. The parser validates syntax, component properties, IDs, pin direction, references and single-driver input constraints before constructing a temporary editor model. Only a fully valid candidate replaces the active model; the replacement advances model revisions so `CircuitRuntime` rebuilds the ordinary `CircuitBuilder`/`Simulator` path. The serializer sorts by stable IDs/endpoints to produce canonical deterministic text. `hcs_circuit` exposes the same validation/formatting path headlessly. Custom definitions were outside the scope of Phase 5.3 and are added in Phase 5.4 without changing the `.hcs` document version. See [`docs/CIRCUIT_FORMAT.md`](docs/CIRCUIT_FORMAT.md), [`docs/GUI_ARCHITECTURE.md`](docs/GUI_ARCHITECTURE.md) and [`docs/HCS_GUI_INTEGRATION.md`](docs/HCS_GUI_INTEGRATION.md).


## Phase 5.4: custom component definitions and hierarchy

Custom components are part of the editor layer, not new simulator opcodes. `CustomComponentDefinition` owns a reusable internal `CircuitEditor` graph and stable external port IDs. `.hcscomponent` (`HCSC 1`) stores that definition; the existing `.hcs` v1 format stores custom instances by stable definition reference. `ComponentLibrary` resolves reusable definitions from the per-user data directory and rejects missing or malformed dependencies. Directory imports and definition edits use candidate libraries/models and publish only after validation.

Before normal runtime construction, `CircuitRuntime::synchronize(editor, library)` validates and flattens nested custom instances into distinct ordinary editor components. The existing `CircuitBuilder` and `hcs::Simulator` remain the sole execution path. Each custom instance expands into its own internal built-in components, preserving independent DFF state; the six existing backends and simulator semantics are retained. Hierarchy limits bound nesting depth, expansion size and dependency contexts. The expansion currently exports root pin mappings but not a public full internal hierarchy/source map.

The optional SDL3 GUI adds a component library panel, selection extraction, empty-definition creation and a separate shared-definition editor. `FontManager` centralizes proportional UI and monospaced code font roles, optional SDL3_ttf loading and the ASCII fallback. Fonts are system-installed rather than bundled. The GUI target was unavailable in the Phase 5.4 build environment because real SDL3 development files were missing; only a syntax-only check against temporary declarations was possible. See [`docs/CUSTOM_COMPONENTS.md`](docs/CUSTOM_COMPONENTS.md), [`docs/GUI_ARCHITECTURE.md`](docs/GUI_ARCHITECTURE.md) and [`docs/PHASE5_4_REPORT.md`](docs/PHASE5_4_REPORT.md).


## Phase 5.5: visual component symbols

The editor layer now has a renderer-independent `SymbolDefinition` model for custom component appearance: parameterized vector shapes, editable polygon/polyline paths, text objects, layer order and visual port endpoints. `HCSS 1` is a separate deterministic text format parsed and validated transactionally. `.hcscomponent` files can embed an optional `symbol-begin` / `symbol-end` HCSS block; older HCSC 1 files without the block receive a generated default symbol. Symbol visual port IDs must match the definition's logical port IDs, while logical direction remains exclusively in the HCSC interface metadata.

`gui/symbol_renderer.cpp` is shared by the root editor, component library and Visual Component Designer. It transforms and renders the same symbol model, and the editor's custom pin layouts use those port coordinates, so wire endpoints follow symbol rotations/flips. Geometry hit-testing is performed in symbol-local coordinates rather than treating the whole symbol bounding rectangle as filled. A visual-only library update does not advance the logic revision, so `CircuitRuntime` does not rebuild or reset the existing HCS simulator when only appearance changes. The existing CircuitBuilder/Simulator path and all backend algorithms remain unchanged.

The Visual Component Designer provides shape/path/text tools, visible object selection/resize handles, live path construction previews, port drag/orientation/labels, object ordering/visibility/locking, snapping, basic transforms, stroke-width/text-size controls, and a synchronized HCSS source editor. Paths currently use editable straight segments (not Bézier curves); exact coordinates, arbitrary colors and detailed alignment/font properties are authored through HCSS, while the visual inspector is not a full numeric property grid. The GUI was not built or launched against real SDL3 on the Phase 5.5 host; headless model, persistence, transform and runtime-independence tests were run. See [`docs/SYMBOL_DESIGNER.md`](docs/SYMBOL_DESIGNER.md), [`docs/CUSTOM_COMPONENTS.md`](docs/CUSTOM_COMPONENTS.md) and [`docs/GUI_ARCHITECTURE.md`](docs/GUI_ARCHITECTURE.md).
