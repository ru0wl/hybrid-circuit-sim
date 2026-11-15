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

Incremental compilation, nested custom components, GPU execution, automatic backend selection, runtime workload-threshold switching, and multi-bit buses remain postponed. Phase 5.2 adds opt-in AVX2 and multicore experiments limited to the HybridIR backend; AVX-512 and ARM NEON remain postponed.

## Completed foundation: IR event execution

Optimized IR supports dense and event-driven execution, including propagation after sequential state commits. Phase 4.3 adds an opt-in region-dirty experimental executor; automatic workload-threshold selection, SIMD and multithreading remain postponed pending further benchmark evidence.


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



## Phase 4.2: profiling-guided runtime optimization

- [x] Audit all five execution paths, profiling, benchmark calibration/validation, signal storage, dependency traversal, and DFF transition ordering.
- [x] Quantify profile-enabled overhead with fixed-workload A/B measurements for all backend families.
- [x] Run the full 13-scenario / 6-backend benchmark suite and preserve summary, raw and JSON data.
- [x] Run the full opt-in profile suite and a 512-DFF stress workload.
- [x] Test and reject more profile-path splitting, byte scheduled flags, and CSR fanout after mixed/non-repeatable results.
- [x] Verify GCC/Clang CMake Release and Makefile test builds.
- [x] Document why no SIMD or mandatory architecture-specific path is justified.
- [x] Preserve runtime semantics and keep the final Phase 4.1 runtime implementation unchanged.
- [ ] Collect PMU counters / call graphs and exact allocator counts on a physical host with accessible `perf` or Callgrind.
- [ ] Revisit microbenchmarks for private queue/evaluator primitives only if hardware counters or call graphs isolate a dominant primitive.


## Phase 4.3: experimental execution backend and hybrid scheduling

- [x] Audit Phase 4.2 source, architecture, profiles, benchmark calibration, regions/SCCs and DFF semantics before changes.
- [x] Add isolated `Backend::HybridIR` / `optimized-ir-hybrid` over optimized IR.
- [x] Re-analyze optimized IR for valid region IDs after optimizer rewrites.
- [x] Add deterministic topological single-pass execution for acyclic regions.
- [x] Add reusable per-region FIFO operation scheduling for feedback regions, with duplicate suppression and event budget handling.
- [x] Preserve the existing common simultaneous DFF capture/commit sequence and signal observability.
- [x] Add randomized combinational, sequential and feedback differential coverage, connected feedback SCC coverage, oscillation, dirty-region counts, and reverse-order DAG tests.
- [x] Preserve all 13 Phase 4.2 workloads/six existing backend modes; add two targeted region workloads and a seventh hybrid mode.
- [x] Run the full suite (105 summary rows / 465 raw samples) and 20-repetition size-512 targeted A/B experiments.
- [x] Add research, performance, benchmark, profile and Phase 4.2 comparison artifacts.
- [x] Verify GCC/Clang Release builds, CTest, Makefile test target, and clean extraction build.
- [ ] Consider finer than weak-component region partitioning only if a semantics-preserving graph decomposition is identified and separately measured.
- [ ] Hardware performance counters and allocator instrumentation remain pending on a host with `perf`/Callgrind support.

The hybrid backend remains opt-in. Sparse independent regions benefit; high activity, high fanout and some sequential/feedback workloads still favor existing backends. No automatic strategy selection or default change is planned.


## Phase 5 — GUI foundation and editor persistence

Implemented in the current source tree:

- Separate UI-independent `hcs_editor` library and SDL3 GUI source, including world-space model, pin/wire editing, camera, runtime integration and simulation controls.
- Version-1 human-readable `.hcs` persistence with deterministic ordering, symbolic port names, stable component IDs, strict validation, atomic parse-and-replace, and atomic file saves.
- GUI New/Open/Save/Save As, dirty-state tracking, path-entry fallback, unsaved-change/overwrite confirmation, and editable text mode with validate/apply/reload.
- Headless `hcs_circuit` validation/formatting tool and persistence/runtime regression tests.
- Format, architecture, GUI workflow and example documentation.

Remaining verification and later work:

- Build and launch the GUI against a real SDL3 development package and desktop session; exercise text input, clipboard, path entry, unsaved-change prompts and actual mouse/keyboard interaction.
- Exercise real window resize/high-DPI behavior with SDL3.
- Undo/redo, auto-routing, custom/user-defined components, component libraries and hierarchical subcircuits remain deferred to later phases.


## Phase 5.2 — experimental hardware acceleration

- [x] Audit existing SIMD, compiler-vectorization, threads, region analysis, DFF ordering, GPU dependencies and GUI settings before changes.
- [x] Add an isolated AVX2 kernel compiled with per-source `-mavx2` and guarded by runtime CPU/OS feature detection.
- [x] Add scalar fallback and scalar-only `HCS_ENABLE_AVX2=OFF` build configuration.
- [x] Add explicit AVX2 batches for eligible same-opcode topological levels in acyclic HybridIR regions; keep Auto scalar because no repeatable benefit was measured.
- [x] Add a persistent per-simulator worker pool with static partitioning for sufficiently large batches of independent acyclic HybridIR regions.
- [x] Preserve serial feedback/SCC settling and common simultaneous DFF capture/commit semantics.
- [x] Add differential tests for all LogicOp values, randomized signal sequences, worker execution, AVX2 + worker execution, feedback fallback and worker configuration errors.
- [x] Add a 20-repeat hardware A/B benchmark with 4 workloads, 4 configurations, checksums, operation counts, timing distribution, throughput, setup time, process CPU-time estimate and actual SIMD/parallel execution counters.
- [x] Add CLI hardware capability reporting and SDL3 GUI controls for actual CPU settings; GPU is clearly reported unavailable.
- [x] Document GPU as unimplemented rather than exposing a non-functional enable switch.
- [ ] Implement and test Vulkan compute on a host with Vulkan SDK, shader compiler and accessible compute-capable GPU.
- [ ] Revisit AVX2 packing/layout and thread scheduling only with new representative workloads and repeatable measured benefit.
- [ ] Add AVX-512 and ARM NEON only when platform-specific tests and workloads justify them.

The measured AVX2 and multicore implementations were slower or within noise on the current virtualized, CPU-quota-limited host. Both remain explicit opt-ins; no speedup is claimed and no existing backend/default was changed.
