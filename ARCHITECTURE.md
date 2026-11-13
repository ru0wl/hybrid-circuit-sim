# HCS Architecture and implementation audit

## Current architecture

HCS is a one-bit digital logic simulator. The editor is not part of this repository. `CircuitBuilder` creates a compact immutable `Circuit` containing signal IDs, primary-input IDs, a contiguous gate vector, a stable execution-order vector, and D flip-flop descriptors.

### Existing implementation audit

1. **Circuit representation:** `Circuit` stores a signal count and contiguous vectors for inputs, gates, execution order, and DFFs. IDs are 32-bit integers.
2. **Components and connections:** each `Gate` stores an opcode, up to two input signal IDs, and one output signal ID. Each DFF stores D, clock, Q, and initial state. There is no general editor-component object model.
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

The original `Interpreted` Dense and EventDriven modes are retained. `OptimizedIR` is an additional opt-in backend with dense and direct event-driven strategies selected through `InterpreterStrategy`. `Compiled` explicitly prepares optimized IR and uses the existing native shared-library loader. DFF edge detection and simultaneous state commit remain in the common `Simulator` core for all backends.

## Known limitations

- One-bit signals and binary gates only.
- No explicit primary-output declaration: all signal IDs are observable.
- No operation fusion; ordinary simulator circuits do not benefit from DCE because every signal is observable.
- No incremental compile, SIMD, multithreading, buses, nested components, or editor integration.
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
