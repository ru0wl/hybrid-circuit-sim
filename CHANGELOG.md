# Changelog

## Unreleased — compiler foundation

- Added a standalone circuit IR with operation IDs, signal references, source component mappings, and separate DFF state descriptors.
- Added dependency graph construction, Tarjan strongly connected components, feedback-region classification, sequential-boundary regions, and a critical-path estimate.
- Added configurable constant folding/propagation, Boolean simplification, common-subexpression forwarding, and copy propagation passes.
- Added an optimized IR interpreter that does not invoke a native compiler.
- Changed the native compilation pipeline to lower and optimize IR before generating C++ while retaining the existing shared-library ABI and loader.
- Added IR and compiler preparation statistics APIs and tests for SCC detection, source mapping, optimization, and DFF equivalence.
- Added architecture, roadmap, and benchmark audit documents.
- Dead logic removal and operation fusion are deliberately inactive while all signal values remain externally observable.

## Phase 3

- Added direct event-driven execution of optimized IR.
- Added IR signal fanout scheduling and duplicate-event suppression.
- Added randomized deterministic differential testing.
- Added sequential differential coverage for IR event execution.
- Expanded benchmark scenario registration to the full planned workload set.
- Added `hcs_inspect` text/JSON/DOT developer inspection utility.
- Documented public signal observability constraints.


## Phase 4 — benchmark reliability and profiling

- Reworked benchmark sampling with separate warmup/measured simulator instances and adaptive common iteration counts.
- Added 20 default interpreted/warm repetitions and 3 cold native compile repetitions.
- Added median/min/max/mean/stddev/P95, median throughput, raw CSV, summary CSV and JSON output.
- Split validated cold native compilation from warm-cache loading; added break-even estimates.
- Added final all-signal checksum validation across backends.
- Replaced the zero-checksum feedback fixture with a stable feedback circuit that changes from 0 to 1.
- Made all 12 benchmark scenarios structurally distinct and added named scenario selection.
- Added optional runtime profiling counters, queue/evaluation micro-timings, and compiler topology/opcode statistics.
- Added process RSS snapshots and generated native source/shared-library size metrics.
- Added randomized sequential differential testing and profiler/statistics tests.
- Optimized IR evaluation to avoid constructing a temporary `Gate` and calling the external generic evaluator per operation.
- Added GCC/Clang build checks, Clang-generated native module checks, and O2/O3 comparison.
- Added SIMD/bit-slicing feasibility and performance investigation reports.


## Phase 4.1 — calibration and runtime hot-path corrections

- Replaced dense-only calibration with independent backend probes and one common iteration count derived from the fastest backend.
- Added post-sample duration validation, full-scenario retries with fresh simulators, a configurable iteration cap, and accepted/rejected sample metadata.
- Renamed the preserved fixed-point feedback workload to `feedback-stable` and added the continuously switching `feedback-dynamic` workload.
- Added direct feedback-state transition accounting and convergence reporting.
- Replaced per-step DFF next-state vector allocation with reusable private simulator scratch storage; simultaneous Q commits are unchanged.
- Expanded opt-in profiling with queue append/dequeue, exclusive fan-out traversal, scheduling, signal update, operation evaluation, settle iteration, and oscillation-limit metrics.
- Added feedback, event profiler, simultaneous DFF, and 256-DFF differential tests.
- Added Phase 4.1 benchmark, profiling, and before/after comparison artifacts without overwriting Phase 4 results.
- Kept oscillation as a correctness/limit-protection test rather than ranking non-converging circuits as throughput workloads.
