# HCS Roadmap

## Current milestone: compiler foundation

- [x] Compact IR independent of `Circuit` storage.
- [x] Separate sequential state descriptors.
- [x] Source component IDs on IR operations.
- [x] Dependency graph and Tarjan SCC analysis.
- [x] Combinational and sequential-boundary region records.
- [x] Configurable optimization pass list.
- [x] Backward reachability and dead-logic elimination for IRs with explicit observable outputs.
- [x] Exact shared-expression detection and unused-output reporting.
- [x] Redundant double-inversion elimination.
- [x] Constant folding/propagation and Boolean identities.
- [x] Common-subexpression forwarding while preserving observable signals.
- [x] Copy forwarding for acyclic operations.
- [x] Optimized IR interpreter without native compilation.
- [x] Native code generation from optimized IR.
- [x] IR/dependency dumps and compiler preparation statistics API.
- [x] Native codegen, cache lookup, compiler invocation, library load, and total preparation timings.
- [x] Repeated benchmark runs with median throughput and separate phase-timing columns.
- [x] GCC and Clang project builds; generated native code compiled with Clang.
- [x] Basic tests for IR, SCC, optimization, source mapping, and DFF equivalence.

## Phase 4: reliability and profiling

- [x] Separate warmup from measured simulator state.
- [x] Add adaptive sample duration and repeated measurements (20 normal/warm, 3 cold by default).
- [x] Add summary statistics and raw CSV/JSON output.
- [x] Separate cold compilation from validated warm-cache execution.
- [x] Verify checksums across backends for every benchmark scenario.
- [x] Add all 12 benchmark workloads and six backend modes.
- [x] Add opt-in runtime event/dense profiling counters.
- [x] Add IR opcode/dependency/SCC/fan-out statistics.
- [x] Add process RSS snapshots and native source/library size metrics.
- [x] Add randomized sequential differential tests.
- [x] Add O2/O3 comparison and GCC/Clang build/native compiler checks.
- [x] Document SIMD and bit-sliced feasibility; defer implementation.
- [ ] Add hardware-counter/call-graph profiling on a host with `perf` or Callgrind.
- [ ] Add per-subsystem allocation accounting rather than process RSS snapshots.
- [ ] Add optimizer-level randomized pass-order/property tests.
- [ ] Consider iterative Tarjan for very deep circuits.

## Explicitly postponed

SIMD/AVX/NEON, multithreading, incremental compilation, nested custom components, GPU execution, adaptive scheduling, and multi-bit buses remain postponed.

## Completed foundation: IR event execution

Optimized IR supports dense and event-driven execution, including propagation after sequential state commits. Full adaptive scheduling, SIMD and multithreading remain postponed pending benchmark evidence.


## Phase 4.1: calibration and hot-path corrections

- [x] Probe every backend independently and derive one common iteration count from the fastest backend.
- [x] Validate each measured sample duration; retry all backends with fresh instances and a shared larger iteration count.
- [x] Add a configurable maximum calibration iteration count and explicit sample acceptance metadata.
- [x] Preserve the old stable feedback workload as `feedback-stable`; add deterministic active `feedback-dynamic`.
- [x] Validate feedback state transitions independently of the final all-signal checksum.
- [x] Reuse simulator-owned DFF next-state scratch storage while preserving simultaneous commits.
- [x] Extend optional queue, fan-out, scheduling, signal-update, evaluation, settling, and oscillation profiling.
- [x] Add 256-DFF sequential differential coverage and profiler/feedback tests.
- [x] Run full Phase 4.1 benchmark and preserve separate summary/raw/JSON artifacts.
- [x] Build and test Release configurations with GCC and Clang.
- [ ] Hardware performance counters and exact allocator instrumentation remain pending on a suitable host.

