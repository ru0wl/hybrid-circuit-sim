# HCS Phase 4.3 — Research: Region-Dirty Hybrid Scheduling

## Executive result

Phase 4.3 adds an explicitly selectable experimental backend, `Backend::HybridIR` (`optimized-ir-hybrid`). It does not replace any existing executor and is not selected automatically.

The experiment combines two execution modes at the existing dependency-region boundary:

- Acyclic combinational regions are evaluated once in deterministic topological order after being marked dirty.
- Feedback regions use a reusable per-region FIFO of dirty operations with duplicate suppression. This retains event-driven propagation order inside a cyclic component instead of treating a feedback component as a DAG.

The strongest measured result is a 34.8% throughput increase over `optimized-ir-event` on the 512-size sparse independent-chain workload (20 repetitions). The same backend is slower than optimized dense and native warm execution on the high-activity version of that workload. The result supports keeping the backend as an opt-in experiment, not promoting it to a default.

## Phase 4.2 source audit

The Phase 4.2 ZIP was extracted and inspected before modifications. The audit covered the CMake/Makefile build, public headers, source files, test harness, benchmark harness, historical result files, and Phase 4.2/4.1/4 architecture and performance reports.

### Existing executors

| Backend | Execution model | Main runtime work |
|---|---|---|
| `Interpreted` + `Dense` | Repeated pass over the circuit's stable execution order | Evaluate all gates until a pass makes no writes |
| `Interpreted` + `EventDriven` | FIFO queue of gates, per-gate duplicate flag | Pop gates, evaluate, traverse gate fanout after output changes |
| `OptimizedIR` + `Dense` | Repeated pass over optimized `ir::Operation` | Direct operation evaluation and full IR scan per pass |
| `OptimizedIR` + `EventDriven` | FIFO operation queue and byte scheduled flags | Pop operations, evaluate, traverse IR fanout and schedule dependents |
| `Compiled` | Generated native C++ shared module | Native dense fixed-point passes; optional persistent compilation cache |

The original interpreted executors, optimized-IR dense/event executors, compiled-native path, cache, and their selection semantics remain available.

### Dependency and region representation

`analysis::analyze()` returns predecessor/successor lists, Tarjan strongly connected components, and `Region` descriptors. A `Region` is a weakly connected component of the operation dependency graph; it is **not** a single SCC and it is not a topological level. One region can contain multiple SCCs and intervening acyclic operations. Sequential boundary descriptors are also present.

This matters for scheduling. A region containing a cycle cannot be processed as an ordinary DAG. Phase 4.3 uses the existing graph/SCC analysis to classify a component, but keeps the full component as the unit of dirty scheduling.

`prepare_circuit()` runs optimizer/dependency preparation on its original input IR. Since optimization can alter the operation vector, Phase 4.3 re-runs `analysis::analyze()` on the final optimized IR before building its runtime region tables. It does not reuse potentially stale operation-region indices from the pre-optimization graph.

### Confirmed runtime characteristics

- Dense backends scan their full operation/gate order even when no input changed.
- Event backends avoid evaluating unaffected operations, but pay operation-level queue and dependency-scheduling costs.
- Optimized IR already evaluates the `Operation` record directly through the local opcode switch; it does not construct temporary `Gate` records for each evaluation.
- Both event backends use reusable vector-backed FIFO storage and duplicate-scheduling flags.
- Public signal observability prevents deleting intermediate signal writes as a general optimization.
- DFF next-state storage is reused. `step()` samples all next states first, commits all Q values in a separate loop, then propagates changed Q signals.
- Existing feedback workloads show that dense and event execution can have different fixed-point outcomes for some multi-stable combinational feedback networks. The difference exists between Phase 4.2 backends; it is not introduced by the hybrid executor.

## Experimental backend design

### Outer dirty-region queue

At construction, the backend builds its region metadata from optimized IR and allocates reusable region scheduling state:

- signal-to-operation consumers;
- region operation lists;
- one byte flag per region for duplicate scheduling;
- one reusable FIFO region queue and head index;
- one operation scheduled flag per optimized IR operation;
- one reusable local event queue per cyclic region.

All non-empty combinational regions are scheduled for the initial settle. This ensures that constant-only and otherwise input-independent regions receive their initial evaluation.

When a primary input or DFF Q value changes, its consumer operations are inspected. Their owning regions are scheduled once. Acyclic regions need no per-operation dirty queue; feedback regions also enqueue the exact consumer operations that became dirty.

### Acyclic regions

Acyclic region operations are ordered once during simulator construction using a stable Kahn topological traversal. A min-priority queue makes tie-breaking deterministic by operation ID. The resulting operation list is then evaluated in a single pass when the region is dirty.

This is safe because every producer in the component precedes its consumers. Intermediate and final signal values are still written to the normal signal vector, so `signal(id)` observability is unchanged.

If an ostensibly acyclic region cannot be topologically ordered, it is conservatively handled as a feedback region.

### Feedback regions

Feedback regions are not treated as DAGs. Each has a reusable FIFO operation queue and duplicate-scheduling flags. The initial settle seeds the region's operations in operation-ID order. A changed operation output schedules only its successors within the same component. A successor already in the queue is not added again.

The queue preserves the optimized-IR event executor's deterministic FIFO update order within a feedback component. `max_events_per_settle` bounds work in the feedback operation queue; an unfinished queue remains pending and the `StepResult` is marked non-converged. The cyclic region does not use `max_dense_passes` because it follows event-queue settling semantics.

### Sequential logic

The common `Simulator::step()` DFF path was not redesigned:

1. settle current combinational values;
2. compute every DFF next state from current D and Q values and current clock edge;
3. commit all Q values in a separate loop;
4. schedule regions/feedback operations dependent on changed Q signals;
5. settle the affected regions.

No DFF can observe another DFF's newly committed state during the same edge.

### Determinism and observability

- Region operation tables are derived from optimized IR once.
- Acyclic tie-breaking uses operation IDs.
- Consumer lists are sorted and deduplicated.
- Feedback queues use FIFO order.
- The standard signal vector remains the source of truth.
- No operation, signal, or intermediate output is removed from the public signal space.
- No native compiler is required to select `Backend::HybridIR`.

### Counters

Hybrid profile counters expose region schedule attempts, regions successfully scheduled, regions processed, operation evaluations within regions, and maximum dirty-region queue length. Existing queue counters are also populated for the feedback-operation queues. `maximum_queue_length` describes the local feedback operation queue; `maximum_dirty_region_queue` describes the outer region queue.

For `StepResult`, `operations` counts operation evaluations. The hybrid `events` work unit is intentionally backend-specific: one acyclic region activation counts as one event, while each dequeued operation in a feedback region counts as one event. This distinction is documented rather than pretending that region activations and operation events are directly comparable across backends.

## Experiments and design decisions

### Candidate A — dirty regions with dense settling in every region

An initial implementation used region dirty flags but evaluated even acyclic regions with repeated dense passes. It duplicated operation evaluations and did not provide enough benefit over optimized event execution. This form was replaced.

### Candidate B — topological single-pass acyclic regions

The acyclic region pass was replaced with a deterministic topological order. On the targeted sparse independent-chain workload, a 20-run experiment showed a repeatable improvement over optimized IR event. This became the DAG path of the retained experimental backend.

### Candidate C — dense feedback-region settling

Randomized latch-like feedback circuits exposed an existing difference between dense fixed-point and event-driven execution: some circuits have multiple stable fixed points, and evaluation order selects one. The initial hybrid followed dense settling and therefore did not match optimized IR event on those circuits. Rather than silently choosing dense semantics, the feedback path was changed to a local FIFO operation queue. The new randomized feedback test checks hybrid against optimized IR event for these circuits.

A separate multi-SCC component test also checks the hybrid feedback path against optimized IR event. Existing dense/event divergence remains documented; existing backend implementations were not modified to force them to agree.

## Known limitations and risks

1. **Weak-component granularity.** A long connected chain is still one region. This backend does not split a single large weakly connected DAG into independently dirty subregions.
2. **High activity.** If most independent regions become dirty each step, region scheduling can cost more than a single optimized dense pass.
3. **High fanout.** A changed input that fans into many operations/regions requires consumer traversal and duplicate checks; the `wide-fanout` scenario regresses against optimized IR dense and event in this environment.
4. **Feedback scheduling.** Cyclic regions retain event-style queue costs. They can be slower than the optimized event executor because the outer region queue and per-region queue are both maintained.
5. **Dense/event feedback differences.** Existing dense/native and event backends can settle multi-stable feedback circuits differently. Hybrid deliberately follows optimized IR event ordering in cyclic regions. Users should select an executor with awareness of this pre-existing difference.
6. **Event budget units.** For hybrid, `max_events_per_settle` counts region activations for acyclic regions and individual operation events inside cyclic regions. `StepResult.events` is correspondingly not a cross-backend apples-to-apples counter.
7. **Setup cost.** Region analysis, topological ordering, and queue reservation occur at simulator construction. The benchmark runtime samples measure execution, not a full application lifecycle that repeatedly constructs simulators.
8. **Experimental API.** `Backend::HybridIR` is opt-in and not a stable performance recommendation. No automatic selection threshold is introduced.

## Research conclusion

The data supports a narrow conclusion: region-level dirty scheduling is useful when a large circuit is composed of many independent, mostly inactive acyclic regions. It does not dominate dense or event execution for all circuit topologies. The backend is retained as a separate experiment because it provides a substantial win on a well-defined workload class and passes expanded differential tests. It is not promoted to the default and should be reconsidered if future workloads show that region partitioning can be finer without changing semantics.
