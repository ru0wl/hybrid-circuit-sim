# Phase 4.2 performance report — profiling-guided runtime optimization

## Executive result

Phase 4.2 completed the source audit, default-off profiling overhead investigation, full scenario/backend benchmark, profiling run, 512-DFF stress workload, and three isolated optimization experiments. **No runtime optimization was retained.** The candidates produced mixed changes within measurement variability or caused workload regressions; retaining them would not satisfy the project's evidence threshold. Final simulator C++ and public headers are unchanged from Phase 4.1. The result is an evidence-backed decision not to add speculative optimizations.

The Phase 4.2 benchmark suite contains 13 scenarios × 6 backend configurations (78 summary rows, 364 accepted raw samples). All scenarios contain all six backends; steps and public-signal checksums match across a scenario's backends. The full throughput suite uses 5 measured repetitions for interpreted and warm-native configurations and 3 cold-native repetitions. A separate profile suite contains 78 summary rows and 208 raw samples (3 repetitions for non-cold modes and 1 cold-native sample). Profile-on throughput is intentionally not mixed into the unprofiled benchmark.

## Environment and reproducibility

- Linux 6.18.57, x86-64, KVM VM, AMD EPYC 9V74 host, 3 vCPUs visible.
- GCC 14.2.0, Clang 17.0.0, C++20 Release / `-O3 -DNDEBUG`.
- CMake 3.31.6.
- Benchmark circuit size parameter: 256; minimum iterations: 100.
- Full throughput command:

```bash
./build-gcc/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 5 --cold-runs 3 --warmup 1 --min-sample-ms 1 \
  --max-calibration-iterations 1000000 --compile-mode both \
  --cache .bench-cache-phase4.2-final \
  --output-prefix benchmark-results-phase4.2-summary
```

A 20-measured-repetition full-suite attempt exceeded the execution time limit before completing the final scenario; its incomplete output was discarded. The complete suite was rerun with 5 normal/warm repetitions and 3 cold repetitions. Controlled candidate A/B experiments still used 20 repetitions per block (40 baseline plus 40 candidate samples for each scenario/backend row). The lower repetition count applies only to the full 78-row suite and is explicitly present in the CSV fields.

The benchmark runner performs backend-aware iteration calibration, shares the resulting iteration count across backends within a scenario, validates sample duration, records accepted/rejected status, validates checksums over all public signals, and verifies cold cache misses/warm cache hits.

## Full-suite results

The results below are median-derived rates from this virtualized environment (steps/s). They are descriptive, not a performance promise for another CPU. The CSV/JSON files contain all min/max/mean/standard-deviation/P95, throughput, sample count, calibration and topology fields.

| Scenario | Interpreted event | Interpreted dense | Optimized IR dense | Optimized IR event | Native warm |
|---|---:|---:|---:|---:|---:|
| Small combinational | 4.11 M | 5.56 M | 10.61 M | 8.45 M | 39.63 M |
| Large combinational | 0.274 M | 0.196 M | 0.962 M | 1.036 M | 5.079 M |
| Binary counter | 2.12 M | 0.273 M | 1.178 M | 2.329 M | 1.488 M |
| Large sequential | 0.602 M | 0.071 M | 0.247 M | 0.581 M | 0.417 M |
| Sparse toggle | 1.002 M | 0.551 M | 0.812 M | 1.180 M | 4.644 M |
| High switching | 0.361 M | 0.390 M | 0.568 M | 0.479 M | 3.199 M |
| Deep chain | 0.215 M | 0.166 M | 0.349 M | 0.584 M | 4.121 M |
| Wide fan-out | 0.502 M | 0.596 M | 0.930 M | 0.618 M | 4.182 M |
| Repeated expressions | 0.458 M | 0.246 M | 1.043 M | 0.803 M | 2.724 M |
| Redundant logic | 0.243 M | 0.280 M | 0.444 M | 0.573 M | 2.450 M |
| Feedback stable | 10.25 M | 5.60 M | 13.22 M | 57.81 M | 46.87 M |
| Feedback dynamic | 3.464 M | 4.275 M | 6.703 M | 4.121 M | 27.93 M |
| Independent regions | 0.237 M | 0.167 M | 0.908 M | 0.606 M | 4.309 M |

The `compiled-native-cold` column is retained in the CSV with compile/setup measurements and is not included in the table above; it is a cold-cache preparation configuration, not equivalent to steady-state warm throughput. Native execution has no per-operation runtime profile counters because evaluation occurs inside generated code.

### Sequential stress test

Additional size-512 DFF benchmark output was generated with 158 iterations (316 step calls per sample). Median elapsed time:

| Backend | Median elapsed time |
|---|---:|
| Interpreted event | 1.355 ms |
| Interpreted dense | 9.479 ms |
| Optimized IR dense | 2.519 ms |
| Optimized IR event | 1.102 ms |
| Compiled native cold | 1.794 ms |
| Compiled native warm | 1.756 ms |

The benchmark also includes 64-DFF binary-counter and 256-DFF large-sequential scenarios. Sequential tests preserve simultaneous next-state sampling, full Q commit, then propagation.

## Profiling overhead A/B

This experiment isolates the cost of enabling optional detailed profiling. Each of four scenarios (`large-combinational`, `large-sequential`, `wide-fanout`, `high-switching`) uses 1,000 steps and identical input sequences. Warm backend pairs use 10 repetitions with profiling disabled and 10 enabled; cold configurations use 3 repetitions per state. Checksums and step counts match for every pair.

| Backend | Median wall-time increase when profiling is enabled |
|---|---:|
| Interpreted event | +2,905% |
| Interpreted dense | +1,609% |
| Optimized IR dense | +4,446% |
| Optimized IR event | +4,780% |
| Compiled native warm | +18.5% |
| Compiled native cold | +12.4% |

The high overhead is expected from clock reads around individual operations, signal writes, and event scheduling. It is the cost of **profile-enabled** execution, not profile-disabled execution. Profiling remains disabled by default. The unprofiled event/dense loops still contain some conditional branches; an experiment to split more loops and remove those checks did not demonstrate repeatable gains and was reverted.

The full profiling run (100 iterations per sample, 3 repeats for non-cold, 1 cold) writes `profiling-results-phase4.2.csv`, `profiling-results-phase4.2-raw.csv`, and `profiling-results-phase4.2.json`. Component timings are instrumentation timings, not PMU counters. In some paths accounting intervals can overlap; summing them or interpreting each as an exclusive share of total wall time is invalid. Native operation-level metrics are zero by design because generated evaluation is not instrumented internally.

## Controlled optimization experiments

Each candidate was applied alone to a disposable source copy, built with the same compiler/flags, measured in alternating baseline/candidate blocks, correctness-checked, then reverted. Four end-to-end workloads were used: large-combinational, large-sequential, wide-fanout, high-switching; the candidate suite records baseline and candidate observations for five relevant backend paths plus the native warm control.

| Candidate | Median throughput change across 20 scenario/backend rows | Observed range | Decision |
|---|---:|---:|---|
| Split more profiled/unprofiled loops | +0.78% | −8.96% to +6.53% | Reverted: mixed, no robust backend-wide improvement. |
| `queued_` byte flags instead of `vector<bool>` | −1.18% | −14.46% to +35.80% | Reverted: noisy and strongly workload-dependent. |
| Flatten interpreted fanout to CSR | +0.22% | −16.95% to +9.38% | Reverted: no reliable improvement on fanout workloads. |

The fast-path split showed modest positive results on some dense/large-sequential rows but negative changes on event/high-switching rows. Byte flags helped some interpreted-event cases but worsened or did not change other workloads and controls. CSR fanout had a −2.2% interpreted-event change on wide-fanout and a −17.0% optimized-IR-event change on large-combinational, while other rows were slightly positive. None was retained. `optimization-experiments-phase4.2.csv` and its `-raw.csv` companion contain full per-row statistics and raw samples.

### Why there are no standalone queue/opcode microbenchmarks

The queue, scheduled flags, and operation evaluator are private implementation details rather than reusable public subsystems. Extracting them solely for microbenchmarks would create a new benchmarkable abstraction whose allocation, cache, and call behavior differ from the actual simulator hot path. The existing `ExecutionProfile` records queue/fanout/scheduling/evaluation counts and diagnostic intervals, while isolated changes were tested with end-to-end circuits. A standalone microbenchmark is deferred until hardware-counter or call-graph evidence identifies a specific isolated primitive as a dominant cost.

## Phase 4.1 comparison

`benchmark-comparison-phase4.1-phase4.2.csv` includes all 78 scenario/backend matches. It is deliberately labeled `directional_unpaired_different_run_counts_and_calibration`: Phase 4.1 used 20 repetitions, 4 warmups and a different sample calibration; Phase 4.2 used 5 normal/warm repetitions, 1 warmup and a 1 ms target. It is useful for spotting gross changes, but cannot establish a causal regression or speedup. No runtime code changed in Phase 4.2, so it is not an optimization before/after comparison.

## Correctness and builds

- GCC 14.2.0 CMake Release build: passed.
- GCC CMake `ctest`: 1/1 test target passed.
- Clang 17.0.0 CMake Release build: passed.
- Clang CMake `ctest`: 1/1 test target passed.
- Existing Makefile `make test` (GCC Release): passed.
- Full benchmark: 13 scenarios × 6 backend configurations; all 78 summary rows accepted, all 364 raw samples accepted, per-scenario step counts/checksums match.
- Profiling benchmark: 78 rows / 208 samples; profile-off/on A/B checksum and step counts match.
- Existing randomized combinational/sequential, feedback, oscillation limit, simultaneous DFF, all-signal observability, IR/native differential and native cache tests passed unchanged.
- 512-DFF benchmark completed for all six backend modes.

The runtime code was not modified in the final tree, so no test was weakened or behavior changed to accept an optimization.

## Hardware counters, compiler reports and SIMD

`perf`, Valgrind/Callgrind and Cachegrind were unavailable; `/proc/sys/kernel/perf_event_paranoid` was 2. No hardware cycles, instructions, branch misses, cache misses or allocator counts are claimed. GCC's `-fopt-info-vec-all` report showed that major simulator loops were not vectorized due to control flow/multiple exits or lack of profitability; simple initialization/basic blocks were vectorized. The current 1-byte signal array with arbitrary ID gathers/scatters is not lane-contiguous. AVX2, AVX-512, NEON and bit-sliced execution are deferred; no architecture-specific runtime path or dependency was introduced.

## Artifacts

- `benchmark-results-phase4.2-summary.csv` — 78-row summary.
- `benchmark-results-phase4.2-raw.csv` — 364 raw measured samples.
- `benchmark-results-phase4.2-summary.json` — structured benchmark result.
- `profiling-results-phase4.2.csv` / `-raw.csv` / `.json` — profiler summary and raw samples.
- `profiling-overhead-phase4.2.csv` / `-raw.csv` — profile off/on A/B.
- `optimization-experiments-phase4.2.csv` / `-raw.csv` — rejected candidate comparisons.
- `benchmark-comparison-phase4.1-phase4.2.csv` — directional historical comparison.
- `benchmark-results-phase4.2-512dff.csv` / `-raw.csv` / `.json` — additional sequential stress test.
