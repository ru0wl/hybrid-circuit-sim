# HCS Phase 4.3 — Performance Report

## Test environment and reproducibility

| Item | Value |
|---|---|
| OS / kernel | Linux 6.18.57, x86-64 |
| Execution environment | KVM virtual machine, 3 vCPUs |
| CPU reported by guest | AMD EPYC 9V74 |
| GCC | 14.2.0 (Release) |
| Clang | 17.0.0 (Release) |
| C++ standard | C++20 |
| Benchmark target | 1 ms minimum sample duration |
| Full suite size | 256 |
| Full suite repetitions | 5 measured warm/interpreted runs; 1 cold-native run per scenario |
| Warmup | 2 runs |
| Targeted region experiments | size 512, 20 measured repetitions per backend |
| Hardware counters | unavailable (`perf`, Valgrind, Callgrind and Cachegrind not installed) |

The benchmark host is a constrained virtualized build environment, not the user's laptop. Absolute nanosecond timings should not be generalized to a different machine. The full suite uses the same per-scenario input sequence and number of `step()` calls for every backend within that scenario. The harness checks the checksum across all public signals and rejects mismatches.

## Full benchmark suite

The full run contains all 13 Phase 4.2 scenarios plus two targeted region workloads. It has 15 scenarios × 7 backend configurations = **105 summary rows** and **465 accepted raw samples**. All summary rows are accepted, every scenario shares the same step count among all seven backends, and every scenario's seven checksums match.

| Scenario | Hybrid median ms | vs IR dense throughput | vs IR event throughput | vs native warm throughput |
|---|---:|---:|---:|---:|
| small-combinational | 3.192 | +36.4% | +72.0% | −64.1% |
| large-combinational | 4.474 | +27.1% | +12.9% | −73.0% |
| large-sequential | 2.806 | −20.5% | −57.2% | −35.5% |
| sparse-toggle | 4.475 | +68.7% | −3.9% | −75.7% |
| high-switching | 8.179 | −26.7% | −8.2% | −85.3% |
| deep-chain | 5.829 | +113.1% | +33.0% | −82.3% |
| wide-fanout | 8.651 | −40.8% | −8.6% | −87.7% |
| repeated-expressions | 1.906 | +141.4% | +95.2% | −43.1% |
| redundant-logic | 5.611 | +20.7% | −15.5% | −80.2% |
| feedback-stable | 1.653 | +219.6% | −28.2% | −3.8% |
| feedback-dynamic | 6.729 | −26.7% | +5.0% | −82.1% |
| independent-regions | 5.377 | −10.5% | +32.5% | −79.9% |
| region-sparse-chains | 1.170 | +2560.6% | +40.8% | +1250.9% |
| region-high-activity-chains | 1.741 | −24.3% | +88.9% | −33.4% |
| binary-counter | 1.997 | −0.0% | −44.1% | −21.1% |

Positive percentages mean hybrid throughput is higher than the named backend; negative values mean it is lower. The comparison is computed from the per-scenario median elapsed time (step counts are identical within the scenario). The percentages do not combine or average different circuits.

### Interpretation

- The strongest consistent win is `region-sparse-chains`, a large bank of independent 4-operation acyclic regions where only one lane changes. Hybrid avoids touching the inactive regions.
- Hybrid also improves `small-combinational`, `large-combinational`, `deep-chain`, `repeated-expressions`, and `independent-regions` against selected backends, but this is not a universal result.
- It is clearly poor for `wide-fanout` and `high-switching` compared with optimized IR dense. In these workloads, scheduling/dispatch across many dirty regions loses to a straightforward dense scan.
- Sequential and feedback results are mixed. Hybrid is faster than optimized IR dense on the `feedback-stable` workload but slower than optimized IR event. For `large-sequential` and `feedback-dynamic`, it loses to optimized IR dense and native warm.
- Compiled native warm remains the fastest backend for most existing scenarios. The new backend is not a replacement for native execution when compile/cache constraints are acceptable.

## Targeted 20-repetition region experiments

Both targeted workloads use size 512, identical per-scenario input streams, 39,577 measured steps for sparse regions and 612 steps for high activity. Each backend has 20 accepted measured samples, plus a separate warmup; native uses warm cache.

### Sparse independent chains

| Backend | Median ms | Mean ms | Min ms | Max ms | Std. dev. ms | P95 ms | Steps/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| optimized-ir-event | 1.5683 | 1.6104 | 1.5082 | 1.8988 | 0.1050 | 1.7753 | 25,236,068 |
| optimized-ir-dense | 60.6391 | 61.2933 | 59.3378 | 66.9233 | 1.9466 | 63.9639 | 652,667 |
| optimized-ir-hybrid | **1.1632** | 1.1809 | 1.1207 | 1.2736 | 0.0443 | 1.2647 | **34,024,322** |
| compiled-native-warm | 33.2218 | 33.6478 | 32.0890 | 38.7401 | 1.7042 | 37.2924 | 1,191,296 |

Hybrid throughput is **34.8% higher** than optimized IR event, with median elapsed time reduced by 25.8%. Against dense IR, the workload design strongly favors skipping inactive regions. The same-workload checksums match for all backends.

### High activity across independent chains

| Backend | Median ms | Mean ms | Min ms | Max ms | Std. dev. ms | P95 ms | Steps/s |
|---|---:|---:|---:|---:|---:|---:|---:|
| optimized-ir-event | 2.9423 | 2.9747 | 2.7166 | 3.2791 | 0.1543 | 3.2577 | 208,001 |
| optimized-ir-dense | **1.2045** | 1.2365 | 1.1742 | 1.4484 | 0.0710 | 1.3305 | **508,094** |
| optimized-ir-hybrid | 1.7913 | 1.8501 | 1.6518 | 2.1591 | 0.1490 | 2.1202 | 341,711 |
| compiled-native-warm | 1.2212 | 1.2917 | 1.1350 | 2.1592 | 0.2414 | 1.6896 | 501,153 |

With all lanes active, hybrid is 64.3% faster than optimized IR event but **32.7% slower than optimized IR dense** and **31.8% slower than native warm**. This is the expected limit of dirty-region bookkeeping when nearly every region is active.

## Profiling counters

Opt-in profile exports cover `region-sparse-chains` and `feedback-stable` with three measured repetitions each. They are stored separately in `../benchmark-results/profiling-results-phase4.3.csv`, `../benchmark-results/profiling-results-phase4.3-raw.csv`, and `.json`.

For hybrid, the profile counters include region scheduling attempts, successfully scheduled/processed regions, region operation evaluations, fanout edges visited, duplicate operation scheduling, feedback queue sizes, and signal changes. In the sparse-chain workload, the maximum pending region queue was 64 and each processed acyclic region evaluated four operations. In the feedback-stable workload, the maximum feedback operation queue length was 32.

Do not use profiled elapsed time as an unbiased throughput result. Fine-grained profiling adds clock reads to existing event backends; hybrid's region counters are count-based and its queue/evaluation timing fields are not equivalent to the existing per-operation timer instrumentation. The unprofiled full/targeted runs above are the performance comparison.

## Phase 4.2 comparison

`../benchmark-results/benchmark-comparison-phase4.2-phase4.3.csv` contains the 78 matched historical scenario/backend pairs (13 scenarios × six existing backend modes). It is explicitly **directional and unpaired**: Phase 4.2 and Phase 4.3 independently calibrated iteration counts and checksums, and the number of samples differs. The file reports both step counts/checksums and a throughput ratio; do not interpret it as a controlled A/B experiment.

The Phase 4.2 benchmark and profiling files are preserved unchanged.

## Correctness and build verification

- GCC 14.2 Release CMake build: passed.
- Clang 17 Release CMake build: passed.
- `ctest` under GCC: 1/1 passed.
- `ctest` under Clang: 1/1 passed.
- Existing Makefile `make test` target: passed.
- Randomized combinational differential tests: passed; every public signal compared per step.
- Randomized sequential differential tests: passed; 12-DFF circuits, 4 deterministic circuits × 128 steps by default, including all-signal comparison after edges.
- Added randomized feedback differential test: 12 independent latch-like loops × 128 deterministic input steps; hybrid matches optimized IR event for all observed signals.
- Added connected multi-feedback-SCC test, reverse-order DAG test, dirty-region count test, DFF simultaneous-commit coverage, feedback-stable/dynamic coverage, and oscillation event-budget test: passed.
- Compilation cache tests remain enabled and passed with the full test suite.
- Benchmark checksums and step counts match across all seven backends for every scenario.

### Feedback semantic observation

The randomized latch-like test exposed an existing difference between dense and event-driven execution: the dense/interpreted/native family can settle to a different stable value from the event-driven family in a circuit with multiple valid fixed points. Hybrid's feedback regions deliberately match optimized IR event by using its FIFO successor scheduling. The dense/event difference predates Phase 4.3; existing executors were not changed. This is documented as an existing semantic limitation of feedback circuits without a unique fixed point, not hidden as an all-backend equivalence claim.

## Hardware profiling and SIMD

`perf`, Valgrind, Callgrind and Cachegrind were not available in the virtualized environment. No hardware cycle, instruction, branch, or cache counters are claimed. Internal operation/region counters are simulator instrumentation only.

No SIMD path was introduced. Hybrid's main win comes from avoiding whole inactive regions and one-pass topological evaluation, not from Boolean vector arithmetic. Operations use arbitrary signal IDs and the current `uint8_t` representation; gather/scatter and region queue costs remain more relevant than vectorizing individual Boolean operations. Scalar execution remains portable and no ISA requirement was added.

## Retention decision

Retain `Backend::HybridIR` as an explicitly experimental, separately selectable executor because the 20-run sparse independent-region result exceeds the normal measurement noise and the backend passes the extended differential suite. Do not make it default or automatically choose it. High switching, wide fanout, sequential and some feedback workloads still favor other backends. No existing backend was removed or changed.
