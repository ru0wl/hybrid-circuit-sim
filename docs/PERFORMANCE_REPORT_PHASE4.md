# Performance investigation report — phase 4

## Benchmark integrity findings

The phase3 CSV has 60 rows and `runs=1`. All 12 compiled rows are validated cache hits with zero compiler invocation time. Therefore phase3 contains only warm-cache runtime results. The old feedback workload has checksum zero because its initial zero is an absorbing state for `AND(x,input)`.

Phase4 fixes the benchmark lifecycle: warmup uses a separate simulator; measured samples start from a fresh simulator; native cold and warm modes are separate; the warm mode must validate the cache; and every backend is checked against the same final all-signal checksum. The phase4 summary has 72 rows and raw data has 1,236 measured samples. Interpreted/warm modes use 20 repetitions; cold native uses 3 repetitions.

## Before/after comparison

An exploratory same-container comparison was made against the phase3 source archive for five workloads whose circuit builders did not change: small-combinational, sparse-toggle, deep-chain, redundant-logic, and independent-regions. Both runs used size 256, 5,000 requested iterations and 10 samples. On those workloads, optimized-IR-dense throughput changed by approximately +65%, +34%, +229%, +11%, and +17%, respectively. The full backend-by-backend values are preserved in `../benchmark-results/benchmark-comparison-phase3-phase4.csv`.

This is directional rather than a strict paired A/B: the phase3 benchmark warmed and then measured the same simulator instance, while phase4 warms a separate instance and resets the measured state. The later phase4 benchmark results are the reliable current baseline; the comparison CSV explicitly records this methodology difference.

## Measured bottlenecks

- Event-driven backends process only changed fan-out. On binary counter, large sequential, sparse-toggle and feedback-heavy workloads, event counts are far below dense operation counts.
- Dense backends scan every operation on every settle pass. This is why they lose heavily on large sequential and sparse/feedback workloads.
- Wide fan-out and high switching increase event queue volume; the profiler shows maximum queue lengths up to 513 in the tested circuits.
- Duplicate scheduling is workload-dependent. In the profiling run, original event execution made 25,220 duplicate scheduling attempts across the 12 scenarios; optimized IR event made 5,501. This is the sum of one deterministic measured sample per scenario from the profiling summary, not a sum across all three repetitions or a per-step rate.
- Optimized IR dense previously built a temporary `Gate` and called `evaluate_gate()` for each IR operation. The IR executor now evaluates the operation directly through a local inline switch. This removes the temporary and function-call boundary; randomized differential tests protect opcode equivalence.
- Optimizer observability counters show every source operation output is observable in the current `Circuit` API. Consequently, DCE operation-count reduction is zero by design; forwarding-operation counts show where a computation was rewritten without deleting its public signal value.

## Measured execution comparisons

The phase4 result table is in `BENCHMARKS.md`, with full precision in `../benchmark-results/benchmark-results-phase4.csv` and raw samples in `../benchmark-results/benchmark-results-phase4-raw.csv`.

Event-driven execution is substantially faster than dense on counter, large sequential and feedback-heavy workloads. Dense can win on small combinational and large high-activity workloads. Optimized IR dense beats original dense on all committed scenarios in this run (about 1.5×–4.9×), while optimized IR event is close to original event on sequential counters and faster on multiple combinational workloads.

Native warm execution is fastest in 9 of 12 workloads. It is slower than interpreted-event for large sequential, feedback-heavy and binary-counter. The reason is visible in operation counts: the current native backend is a dense evaluator and cannot exploit sparse event propagation.

## Compilation amortization

Cold compiler invocation cost ranged from approximately 41 ms to 380 ms in the tested size-256 workload set. Break-even versus interpreted-event ranged from approximately 30k to 256k steps (approximately 101k to 627k versus optimized-IR-event) where compiled execution was faster. No break-even is reported where compiled runtime was slower. Compiled mode must remain manual and optional.

## O2 vs O3

A 64-bit counter test measured O2 and O3 with 20 warm repetitions. O2 was about 1.618 M steps/s and O3 about 1.607 M steps/s; this difference is within likely run-to-run noise. A separate three-sample cold compile test measured about 121 ms compiler time for O2 and 125 ms for O3. There is no evidence to change the default from `-O2`.

## CPU and memory profiling limitations

The container does not have `perf` or Valgrind/Callgrind installed. No hardware cycle, branch-miss, or cache-miss claims are made. The built-in profiler is opt-in and records operation/event counters, queue length, signal changes, dequeue bookkeeping time, and evaluation time. Its per-operation timestamps perturb the runtime, so throughput conclusions come from non-profiled benchmark runs.

RSS snapshots are process-wide `VmRSS` values from `/proc/self/status`, typically around 2.9 MiB in the profiling run. They do not isolate allocations for a Circuit, IR, dependency graph, or queue. Generated source and `.so` file sizes are reported independently.

## SIMD decision

See `SIMD_FEASIBILITY.md`. Bit-sliced batches of independent circuit instances are promising but require a batch API. Single-circuit AVX2 requires operation grouping and expensive signal-ID gathers; no measured result justifies implementing it in this phase.
