## Phase 5.3 — circuit persistence and text editing

- Added the documented, versioned `.hcs` v1 human-readable circuit format with stable component IDs, symbolic pin names, component positions, Input values, DFF initial states and wire endpoints.
- Added deterministic canonical serialization, strict syntax/reference/pin/property validation, line-aware diagnostics and a 16 MiB input limit.
- Added parse-then-validate-then-replace loading, preserving the existing editor model on failed loads; added atomic temporary-file save/rename.
- Added `hcs_circuit validate`, `print` and `format` commands that use the same parser without SDL.
- Added editor document revisions so component movement and logic-input changes mark a document modified without unnecessarily rebuilding the simulation graph.
- Added GUI New/Open/Save/Save As, dirty-state title/status indicators, unsaved-change and overwrite confirmation, a path-entry fallback, and an editable text view with Apply/Reload/Graph actions and clipboard/key editing.
- Added persistence round-trip, deterministic-output, malformed-file, atomic-replacement, text-edit and loaded-runtime/DFF tests.
- Kept the serialized schema at the editor/model boundary; custom components, subcircuits, simulator memory and runtime signal IDs remain outside the format.

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


## Phase 4.2 — profiling-guided runtime investigation

- Audited the Phase 4.1 source tree and all execution backends, profiling, calibration, validation, signal observability and sequential commit semantics.
- Added a detailed Phase 4.2 source audit and performance report.
- Added a 13-scenario / 6-backend Phase 4.2 benchmark suite, raw samples, profiling samples, and a directional Phase 4.1 comparison without replacing historical artifacts.
- Added fixed-workload profile-off/profile-on A/B data and a 512-DFF sequential stress benchmark.
- Evaluated and reverted three independent candidates (profile-path splitting, byte scheduled flags, and CSR fanout) because none produced a reliable general improvement.
- Kept the final runtime implementation unchanged from Phase 4.1; no speculative optimization or semantic change was retained.
- Documented unavailable hardware-counter tools and deferred SIMD/bit-slicing based on current layout and compiler diagnostics.
- Verified GCC and Clang CMake Release builds and the existing Makefile test target.


## Phase 4.3 — experimental region-dirty hybrid execution

- Audited the complete Phase 4.2 archive, all execution backends, optimized IR/dependency analysis, SCC/region model, queue scheduling, benchmark harness, historical artifacts, and DFF transition ordering before implementation.
- Added separately selectable `Backend::HybridIR` (`optimized-ir-hybrid`); existing backends and defaults remain unchanged.
- Added deterministic topological one-pass execution for dirty acyclic regions and reusable local FIFO operation queues for cyclic regions.
- Recomputed region information from optimized IR after optimization to avoid stale operation-region mappings.
- Added dirty-region scheduling and feedback queue profiling counters while keeping the feature's profile disabled by default.
- Kept all public signals observable and reused the common two-phase simultaneous DFF update path.
- Added randomized feedback differential coverage, connected multi-SCC feedback tests, a dirty-region scheduling test, oscillation event-budget tests and expanded hybrid comparisons across combinational/sequential workloads.
- Preserved 13 historical benchmark scenarios and six backend modes; added two targeted region workloads and the hybrid backend as an additional seventh mode.
- Added full benchmark results, raw samples, JSON summary, opt-in profiling exports, targeted 20-repetition region samples, and a directional Phase 4.2 comparison.
- Retained hybrid only as an experimental backend: it shows a repeatable 34.8% throughput increase over optimized IR event on a sparse independent-region workload, but loses to optimized dense on high-activity region execution and has topology-specific regressions.
- Documented pre-existing dense/event fixed-point differences in some multi-stable feedback circuits; hybrid feedback regions intentionally follow optimized IR event FIFO semantics.
- Hardware counters were unavailable in the build environment; no fabricated PMU figures or mandatory ISA dependencies were added.
- Verified GCC 14.2 and Clang 17 Release CMake builds, CTest, Makefile test target and clean extraction build.


## Phase 5 — GUI foundation

### Added

- `hcs_core` explicit CMake target while preserving the legacy `hcs` link alias.
- UI-independent `hcs_editor` model/runtime/scheduler/camera library.
- SDL3 graphical editor source target `hcs_gui` (created when SDL3 is installed).
- Input, Output, AND, OR, NOT, XOR and D Flip-Flop editor components.
- Output-to-input wire validation, single-driver input pins, world-space hit testing and selection.
- Editor-to-HCS mapping through the existing `CircuitBuilder` and selected `Simulator` backend.
- Manual, 1/10/60/1000 Hz and bounded-unlimited simulation scheduling.
- Procedural renderer, grid, camera pan/zoom, component placement/movement, wire preview/creation, input toggling and output/wire state visualization.
- Editor integration tests and GUI architecture/model/integration documents.

### Compatibility and limitations

- Existing core source, simulator semantics, CLI tools, benchmarks and historical result files are unchanged.
- SDL3 is an optional build dependency for headless environments. The GUI target is omitted with a CMake warning when SDL3 is unavailable; `HCS_REQUIRE_SDL3=ON` makes this a configuration error.
- Save/load, undo/redo, auto-routing, multithreaded simulation, X/Z logic and custom components remain out of scope.
- Validation limitation: the current headless host did not provide the actual SDL3 development package or a desktop session. The GUI target was compile/link smoke-tested against a minimal SDL3 API stub; a real SDL3 window/input/rendering session remains to be verified on a desktop host.


## Phase 5.2 — experimental CPU acceleration and GPU audit

- Added `AccelerationConfig`, CPU architecture/AVX2 capability detection, and `hcs_demo --hardware-info`.
- Added an isolated runtime-detected AVX2 byte-lane kernel for large same-opcode topological batches in acyclic HybridIR regions. The project is not globally compiled with AVX2 flags; scalar fallback remains mandatory.
- Added a persistent worker pool with static range scheduling for sufficiently large batches of independent acyclic HybridIR regions. Feedback regions and DFF capture/commit remain serial.
- Added thread-local/per-region acceleration-stat accumulation before merging, avoiding shared counter writes during parallel execution. Added safe worker-pool construction cleanup and capped custom thread count at 64.
- Added scalar-vs-AVX2, scalar-vs-multicore and combined differential tests, including all supported opcodes, randomized input steps, feedback fallback and signal-by-signal comparisons.
- Added `hcs_hardware_bench`, four targeted workloads, twenty repetitions per configuration, checksums and operation-count validation, timing distributions, throughput and process CPU-time estimates.
- Added GUI execution controls and a truthful GPU-unavailable indication.
- Audited Vulkan but did not implement GPU compute: Vulkan headers/SDK, shader compiler and `/dev/dri` device access were unavailable. GPU enablement is rejected; there is no placeholder backend or CMake switch.
- AVX2 Auto remains scalar and multicore remains Off by default because no consistent speedup was established on the tested virtualized host. Historical benchmark files and existing simulator backends are preserved.
