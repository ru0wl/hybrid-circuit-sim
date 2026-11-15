# Phase 4.2 source audit — profiling-guided runtime investigation

## Scope and baseline

This audit was performed against the supplied Phase 4.1 source archive before changing runtime code. The original Phase 4.1 source tree and all historical Phase 4 / 4.1 result files were preserved. Three isolated optimization candidates were applied to disposable working copies, measured, and reverted because none showed a repeatable benefit without regressions. The final C++ sources and public headers are byte-for-byte identical to the Phase 4.1 baseline.

Build and measurement environment:

- Linux x86-64, kernel 6.18.57, KVM virtual machine.
- AMD EPYC 9V74 host CPU; 3 vCPUs exposed to the guest.
- GCC 14.2.0 and Clang 17.0.0.
- C++20 Release, `-O3 -DNDEBUG` through the project CMake configuration.
- CMake 3.31.6.

These are measurements in the supplied build environment, not results from the developer's Acer laptop. Virtualized CPU scheduling and frequency behavior introduce noise; absolute nanosecond timings should not be generalized to another machine.

## Existing execution paths

| Backend | Actual implementation | Profiling observations |
|---|---|---|
| `interpreted-event` | Original `Circuit` gates, signal fanout, FIFO vector queue with head index, per-gate `queued_` flags | Queue, fanout, scheduling, gate-evaluation and signal-update measurements are conditional on profiling. |
| `interpreted-dense` | Repeated passes over `Circuit` execution order until settling | The per-gate loop contains a profiling-enabled conditional around operation/signal-update timing. |
| `optimized-ir-dense` | Direct `ir::Operation` evaluation using a local opcode switch; repeated dense settling passes | Already has separate profiled and unprofiled execution loops. No temporary `Gate` is materialized per IR operation. |
| `optimized-ir-event` | Direct IR evaluation, precomputed signal-to-operation fanout, FIFO vector queue and byte-sized scheduled flags | Event/fanout/scheduling work is conditionally timed; event-loop profiling checks remain present. |
| `compiled-native` | Generated C++ compiled to a shared object and invoked through the existing native module API | Native evaluation is opaque to `ExecutionProfile`; no per-operation native counters are emitted. Common simulator-level profiling can still account for signal changes around step boundaries. |

The six benchmark configurations remain `interpreted-event`, `interpreted-dense`, `optimized-ir-dense`, `optimized-ir-event`, `compiled-native-cold`, and `compiled-native-warm`.

## Confirmed findings

### 1. Profiling instrumentation is expensive when enabled

`SimulatorConfig::enable_profiling` defaults to false. In the disabled state the implementation does not take per-operation `steady_clock` readings or increment the diagnostic counters. The profiling branches remain in some interpreted/event hot loops; the separate unprofiled `optimized-ir-dense` loop is an existing Phase 4.1 optimization.

When enabled, the profiler reads the clock around many small operations. A fixed-workload, profile-off/profile-on A/B (1,000 steps per sample) showed substantial wall-time expansion for interpreted and optimized-IR execution. Median overhead across the four measured workloads was approximately:

| Backend | Median wall-time overhead with profiling enabled | Observed range across four workloads |
|---|---:|---:|
| `interpreted-event` | +2,905% | +68% to +4,090% |
| `interpreted-dense` | +1,609% | +705% to +3,075% |
| `optimized-ir-dense` | +4,446% | +3,124% to +4,736% |
| `optimized-ir-event` | +4,780% | +58% to +4,869% |
| `compiled-native-warm` | +18.5% | +9.3% to +34.3% |
| `compiled-native-cold` | +12.4% | +5.6% to +30.2% |

Every off/on pair used the same scenario, 1,000 steps and input sequence; output checksums and step counts matched. Warm configurations used 10 repetitions per state; cold configurations used 3. These numbers quantify **the cost of enabling detailed instrumentation**, not the overhead of the disabled profiler. The disabled path performs no per-operation clock reads.

### 2. Event-driven execution has real queue and dependency work

The event implementations perform queue insertion/removal, duplicate-schedule suppression, scheduled-flag updates, and fanout traversal when a signal changes. The profiler records counts and timing intervals for those activities. In wide-fanout and high-switching profiled scenarios, fanout traversal and queue/scheduling counters are substantial. However, the component timers are diagnostic intervals, not hardware CPU counters; their accounting may overlap with surrounding activity and must not be summed or interpreted as exclusive percentages of wall time.

The interpreted event queue is a reusable `std::vector` with a head index. Clearing the queue retains the vector allocation/capacity; the queue grows only when an execution exceeds its current capacity. `queued_` uses `std::vector<bool>`. IR event execution has a byte-sized flag vector. Fanout is represented as vectors of dependent gate/operation IDs, with contiguous storage within each signal's dependency list.

### 3. Dense execution scales with the number of operations and settling passes

Dense modes deliberately visit the execution order on each settle pass. This is useful for high-activity and some small circuits but does unnecessary work when only a sparse subset is affected. In this environment `interpreted-dense` was much slower than the event variants on the 256-DFF `large-sequential` workload (9.20 ms median versus 1.09 ms for interpreted event over its calibrated sample). This is a workload characteristic, not a universal ranking.

### 4. Operation evaluation is already specialized where it matters

The interpreted gate evaluator uses a `LogicOp` switch. Optimized IR directly evaluates `ir::Operation` with its own local opcode switch and does not construct a temporary `Gate` or make an out-of-translation-unit generic evaluator call for every operation. Native code generation emits specialized operation expressions, so it does not pay the same runtime switch cost. Replacing the switch with function pointers or speculative templates would add complexity without measured evidence.

### 5. Signal access and observability constrain safe transformations

Signal values are kept in a contiguous `std::vector<std::uint8_t>`; public `signal(id)` access makes every current circuit signal observable. `SignalId` is a 32-bit identifier. Optimizations must preserve the value of intermediate signals as well as final outputs. The current storage does not reveal an obvious redundant copy or removable intermediate signal. Dead-code elimination and operation fusion therefore cannot freely remove observable writes.

### 6. Sequential state handling is already allocation-conscious and simultaneous

The simulator owns and reuses its DFF next-state scratch buffer. At each positive edge it samples every DFF next-state value from the pre-commit state, then commits all Q values, then propagates changed Q outputs. This preserves simultaneous flip-flop semantics and avoids per-step scratch-vector allocation. Replacing this sequence with sequential read/update would be incorrect.

## Suspected bottlenecks requiring stronger measurement

- Allocator activity when queue/fanout storage grows: process RSS does not identify per-object allocation counts.
- Cache locality of nested fanout vectors versus a compact CSR table: no cache counters were available, and the isolated CSR experiment showed no reliable throughput benefit.
- Whether the `std::vector<bool>` queue flags materially cost CPU time: the byte-flag experiment did not show a consistent gain.
- Opcode switch cost independent of memory/dependency traversal: no direct isolated microbenchmark showed it to dominate end-to-end execution.
- Branch prediction, cache misses, TLB pressure and actual retired instruction mix: hardware counters were unavailable.
- Deep feedback / very high fan-out behavior on physical non-virtualized hardware: current results are from a 3-vCPU VM.

## Existing optimizations confirmed as intact

- Default-off profiling; clock reads and detailed counters are only collected when profiling is enabled.
- Separate profiled/unprofiled optimized-IR dense loops from Phase 4.1.
- Direct IR opcode evaluation without per-operation temporary `Gate` construction.
- FIFO event queue storage re-use and head-index consumption.
- Duplicate scheduling suppression through per-operation scheduled flags.
- Precomputed signal-to-operation fanout tables for event execution.
- Reusable DFF next-state storage and two-phase simultaneous commit.
- All-signal checksum validation across benchmark backends.
- Explicit cold-native cache-miss and warm-native cache-hit validation.
- Existing public signal observability and feedback convergence safeguards.

## Correctness-sensitive areas

1. **DFFs:** all next states must be read before any Q state is committed.
2. **Feedback:** cyclic SCCs must continue to use bounded iterative/event propagation; no DAG-only shortcut is valid for arbitrary feedback.
3. **Event queue:** each changed signal must schedule its dependents, duplicate scheduling must remain suppressed only while an operation is pending, and flags must be reset at the correct dequeue/settle boundary.
4. **Public observability:** all original signal IDs and their post-settle values must remain accessible.
5. **Dense convergence:** preserving execution order and settle limits is necessary for deterministic feedback behavior.
6. **Native equivalence:** generated code, cache keys, module load, and public signal results must remain consistent with the interpreted reference.
7. **Profiling:** instrumentation may affect timing but must never change values, scheduling, clock edges, or convergence.

## Measurement plan and outcome

The plan was to (a) establish the Phase 4.1 baseline, (b) collect default-off end-to-end throughput and opt-in diagnostic profiles, (c) A/B one candidate at a time with fixed workloads and alternating baseline/candidate blocks, (d) run all existing differential and integration tests after each candidate, (e) retain only repeatable wins without broad regressions, and (f) preserve all historical data.

Three candidates were measured and reverted:

1. Split profiled/unprofiled loops for the interpreted dense and event executors. A pair of 20-run blocks per variant showed mixed changes across large-combinational, large-sequential, wide-fanout and high-switching workloads.
2. Replace interpreted-event `std::vector<bool>` scheduled flags with byte flags. Changes varied by scenario/backend, including substantial positive and negative outliers; no robust gain.
3. Flatten interpreted-event fanout into CSR offsets and edge storage. Results were near zero or negative on key fanout workloads and ranged both directions on other scenarios.

No candidate met the retention criterion. The final runtime code therefore remains unchanged from Phase 4.1. The detailed A/B sample data is in `optimization-experiments-phase4.2.csv` and `optimization-experiments-phase4.2-raw.csv`.

## Hardware profiling and SIMD feasibility

`perf`, Valgrind, Callgrind and Cachegrind were not available in the execution environment. `perf_event_paranoid` was 2. No cycle, instruction, branch, or cache-counter values are claimed. GCC's vectorization report was generated for the simulator source: major execution loops were not vectorized due to control flow, multiple exits, or profitability analysis; some simple library/basic-block operations were vectorized. No runtime SIMD dependency or architecture-specific code was added.

The current byte-per-signal layout and arbitrary signal-ID gather/scatter pattern are not a natural lane-contiguous SIMD workload. Bit-slicing across independent circuit instances would require a distinct batch-oriented data layout and API and is not justified by the measured single-circuit bottlenecks. AVX2, AVX-512, NEON and bit-sliced execution remain deferred; scalar execution stays the only required path.
