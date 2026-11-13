# HCS Performance Report — Phase 4.1

## Scope

Phase 4.1 corrects benchmark calibration, separates stable and active feedback workloads, removes repeated DFF scratch-vector allocation from `Simulator::step()`, and expands optional event/runtime profiling. The existing Phase 4 architecture and Phase 4 historical reports are preserved. No SIMD, multithreading, GPU execution, JIT redesign, or IR redesign was introduced.

## Build and test environment

- Linux 6.18.57 x86-64 container.
- GCC 14.2.0 and Clang 17.0.0.
- C++20 Release, `-O3 -DNDEBUG`, warning flags `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- GCC and Clang builds and complete CTest suites passed. Reduced randomized test configurations also passed.
- Hardware `perf` and Valgrind/Callgrind were unavailable.

## Benchmark methodology

- 13 scenarios, six backend modes, 20 measured repetitions for interpreted and warm-native modes, three cold-native repetitions.
- Size parameter 256; minimum starting iteration count 100; four warmups; requested minimum sample duration 1 ms.
- Backend-specific calibration probes all use fresh simulator instances. The fastest backend determines a common iteration count.
- After measurement, if any backend sample is below 1 ms, all backend samples for that scenario are discarded and rerun with a larger shared iteration count and fresh simulators. Maximum common count: 100,000,000.
- All 78 summary rows were accepted; all 1,339 raw samples reached the duration target. Every scenario had the same measured step count and checksum across all six backends. Cold-native rows were cache misses; warm-native rows were validated cache hits.
- Construction, simulator setup, IR preparation, native compilation, and library loading are outside the timed execution interval. Warmup and measurement use separate simulator instances.
- Throughput below is median-derived steps per second. One `step()` means one `Simulator::step()` call; sequential benchmark iterations perform two step calls (clock high and low).

## Phase 4.1 throughput results

Rates are in millions of `Simulator::step()` calls per second.

| Scenario | Interp. event | Interp. dense | IR dense | IR event | Native cold runtime | Native warm |
|---|---:|---:|---:|---:|---:|---:|
| small-combinational | 4.415 | 5.812 | 14.326 | 8.531 | 41.051 | 40.431 |
| large-combinational | 0.284 | 0.226 | 1.194 | 1.151 | 5.296 | 5.301 |
| large-sequential | 0.590 | 0.073 | 0.296 | 0.574 | 0.390 | 0.430 |
| sparse-toggle | 1.025 | 0.576 | 1.046 | 1.133 | 4.451 | 4.829 |
| high-switching | 0.368 | 0.366 | 0.762 | 0.483 | 3.134 | 3.095 |
| deep-chain | 0.220 | 0.162 | 0.282 | 0.589 | 4.027 | 4.052 |
| wide-fanout | 0.526 | 0.594 | 1.094 | 0.626 | 4.426 | 4.368 |
| repeated-expressions | 0.471 | 0.247 | 1.133 | 0.834 | 2.690 | 2.570 |
| redundant-logic | 0.254 | 0.271 | 0.473 | 0.610 | 2.525 | 2.509 |
| feedback-stable | 51.867 | 8.442 | 14.540 | 57.014 | 45.881 | 45.402 |
| feedback-dynamic | 3.737 | 3.934 | 7.973 | 4.702 | 22.821 | 26.745 |
| independent-regions | 0.223 | 0.157 | 1.019 | 0.616 | 4.368 | 4.213 |
| binary-counter | 2.030 | 0.286 | 1.029 | 2.110 | 1.661 | 1.592 |

Cold-native throughput here measures execution time after compilation; compiler preparation time is reported separately in the CSV/JSON. It is not a claim that cold compilation is free.

## Feedback validation

`feedback-stable` preserves the former `feedback-heavy` fixed-point equation `x = x OR set`: the state starts at zero, changes once to one, then stays one. In the 20-run Phase 4.1 suite each backend recorded one feedback-state transition per sample (20 total) and converged.

`feedback-dynamic` uses `x = (x AND hold) OR set`, with `hold=0` and `set` alternating each step. The initial state is zero and the state changes on every measured step. With 33,843 iterations per sample and 20 measured repetitions, each interpreted/warm backend recorded 676,860 transitions of the feedback state; the final state is one because the iteration count is odd. Cold-native recorded the same per-run state-transition behavior over its three samples. All backend checksums matched and all samples converged.

No `feedback-oscillation` throughput scenario was added. Non-converging circuits are not ranked as successful throughput. Existing inverter-loop tests now additionally verify the event limit and `oscillation_limit_hits` profiler counter.

## Sequential scratch-buffer optimization

The previous `step()` allocated a local `std::vector<uint8_t>` and called `reserve()` on every step. Phase 4.1 moves this storage to private `Simulator::next_states_`, sized at construction and reused. Every next-state slot is overwritten before use. All D inputs are sampled before any Q is committed, preserving simultaneous DFF updates. There is no reset pin/API in the current circuit model.

### Controlled Phase 4 versus Phase 4.1

These runs used the same fixed workload size, 10,000 benchmark iterations (20,000 `step()` calls for sequential circuits), 20 repetitions, and warm native cache. The table reports measured throughput changes; it is not a claim that every backend benefited from the scratch-buffer change alone.

| Scenario | Backend | Phase 4 M steps/s | Phase 4.1 M steps/s | Change |
|---|---|---:|---:|---:|
| binary-counter | interpreted-event | 1.844 | 2.086 | +13.1% |
| binary-counter | interpreted-dense | 0.280 | 0.305 | +8.7% |
| binary-counter | optimized-ir-dense | 0.852 | 1.148 | +34.8% |
| binary-counter | optimized-ir-event | 2.013 | 2.103 | +4.5% |
| binary-counter | compiled-native-warm | 1.389 | 1.434 | +3.2% |
| large-sequential | interpreted-event | 0.547 | 0.530 | -3.1% |
| large-sequential | interpreted-dense | 0.067 | 0.073 | +7.9% |
| large-sequential | optimized-ir-dense | 0.196 | 0.285 | +45.1% |
| large-sequential | optimized-ir-event | 0.511 | 0.553 | +8.2% |
| large-sequential | compiled-native-warm | 0.396 | 0.400 | +0.9% |

The optimized-IR dense path improved by about 34.8% on the binary counter and 45.1% on the 256-DFF large sequential workload in this controlled Phase 4-to-4.1 comparison. The other backend changes are smaller and workload-dependent.

A separate scratch-enabled/disabled A/B with the same Phase 4.1 fast path isolates the buffer more closely. On the 64-bit binary counter, IR dense throughput increased about 5.7%; the estimated two-standard-error relative spread was about 3.9%. On the 256-DFF sequential workload, IR dense throughput was effectively unchanged (about -0.1%, with uncertainty about 2.6%). This indicates that the allocation removal is real, but its speed benefit is workload-dependent and can be hidden by dense operation evaluation. The buffer is retained because it removes guaranteed per-step allocation churn without changing semantics. No exact allocator counts are claimed.

## Event/runtime profiling

Profiling remains disabled by default. The unprofiled optimized IR dense executor has a separate fast path so profiling instrumentation does not introduce per-operation profiling branches in ordinary execution. The extended fields are defined in `BENCHMARKS.md`; all new fields are present in the Phase 4.1 profile CSV and JSON.

Representative values from the three-run profile suite (aggregated profile counters for the sample set):

- `wide-fanout`, interpreted event: 76,800 processed events, 76,800 queue enqueues/dequeues, 76,800 fan-out edges visited, 77,568 scheduling attempts, 768 duplicate attempts, queue maximum 256.
- `feedback-dynamic`, interpreted event: 9,903 processed events, 9,900 fan-out edges, 9,999 scheduling attempts, 96 duplicate attempts, 9,900 signal changes, 300 settle calls, maximum 34 event iterations per settle, zero limit hits.
- `binary-counter`, interpreted event: 2,151 processed events, 2,151 enqueues/dequeues, 2,352 fan-out edges, 2,739 scheduling attempts, 588 duplicates, queue maximum 129, 900 settle calls, zero limit hits.

The profile suite uses only three runs and is intentionally not used for throughput rankings. Native generated execution does not expose these internal counters. Queue counts include initial event-queue seeding during simulator construction. Timings are micro-instrumentation, not hardware counters; fan-out timing excludes nested scheduling and queue-append intervals.

## Phase 4 comparison and uncertainty

`benchmark-comparison-phase4-phase4.1.csv` contains 72 scenario/backend pairs. The historical `feedback-heavy` workload is mapped to `feedback-stable`. This is a directional comparison rather than a paired A/B: Phase 4 and Phase 4.1 were collected at different times, with 5 ms and 1 ms sample targets respectively. The CSV includes old/new medians, throughput deltas, and an approximate two-standard-error filter based on per-run standard deviations. Because it is not paired and the sample target changed, the filter must not be interpreted as a formal significance test. Use the controlled sequential comparison for stronger conclusions.

## Event queue metrics: Phase 4 versus Phase 4.1

`profiling-comparison-phase4-phase4.1.csv` compares the shared Phase 4 counters against the final raw sample of the Phase 4.1 three-run profile batch. The profile runs used the same 100-step iteration count, size 256, and warmup setting. For the preserved feedback workload, `feedback-heavy` maps to `feedback-stable`.

For the shared deterministic workloads, event counts and queue maxima match: for example, wide-fanout has 25,600 enqueues/dequeues and a maximum queue length of 256 in both versions; binary-counter has 717 processed events and a maximum queue length of 129; the stable feedback workload has 132 processed events and a maximum queue length of 32. Small differences in the old dequeue/evaluation micro-timings are not interpreted as performance changes. Phase 4.1 adds enqueue, fan-out, scheduling, signal-update, and settle timings for which Phase 4 has no directly comparable baseline.

## Correctness coverage

- Existing scalar/reference, interpreted, optimized IR, and native differential tests passed.
- Existing randomized combinational and sequential differential tests passed.
- Added deterministic dynamic-feedback tests across event, dense, optimized IR, and native backends.
- Added a simultaneous cascaded-DFF test and a 256-DFF shift-register differential test over 300 clock cycles.
- Added queue/fan-out/scheduling/signal/evaluation/settling profiler assertions.
- Extended oscillation tests to check event-budget exhaustion and the profiler limit-hit counter.
- GCC Release: build and CTest passed. Clang Release: build and CTest passed. Reduced randomized settings passed on both.

## Remaining limitations

- No hardware cycles, cache-miss, branch-miss, or call-graph data (`perf`/Callgrind unavailable).
- No exact per-subsystem allocation counts; RSS remains process-wide.
- Native generated code does not yet expose internal event/settle profiling.
- Profiling timings are intrusive and diagnostic.
- No dedicated oscillation throughput benchmark; only correctness/limit tests.
- SIMD, AVX2/AVX-512, ARM NEON, bit-sliced execution, multithreading, incremental compilation, GPU execution, and major IR changes remain deferred.
