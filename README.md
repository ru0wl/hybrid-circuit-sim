# Hybrid Circuit Simulator

A small, buildable C++20 foundation for experimenting with a hybrid interpreted/native-compiled digital logic simulator.

## Implemented

- Compact signal IDs and contiguous gate/state tables.
- Stable topological ordering for acyclic dependencies; cyclic nodes retain deterministic fallback order.
- One-bit constants, buffer, NOT, AND, OR, XOR, NAND, NOR, XNOR.
- Positive-edge D flip-flops with simultaneous state updates.
- Original interpreted Dense and FIFO EventDriven backends.
- New standalone compiler IR with operation/source IDs and separate sequential state descriptors.
- Dependency graph construction, Tarjan SCC analysis, feedback regions, sequential-boundary regions, and critical-path estimate.
- Configurable IR passes: constant folding/propagation, Boolean simplification, common-subexpression forwarding, copy propagation, dead-logic elimination under an explicit output contract, and redundant double-inversion elimination.
- Additional `Backend::OptimizedIR`, which runs optimized IR without invoking a native compiler.
- Experimental `Backend::HybridIR` (`optimized-ir-hybrid`): dirty-region scheduling, one-pass topological evaluation for acyclic regions, and local FIFO scheduling inside feedback regions.
- Compiled backend now lowers and optimizes IR before generating C++ and retains the existing `.so` / `dlopen` architecture.
- Preparation statistics and IR dump APIs.
- Headless correctness tests and benchmark comparisons for interpreted, optimized IR, experimental hybrid IR, and compiled backends.

- Persistent native compilation cache and a calibrated multi-backend benchmark suite.
- Phase 5 editor foundation: world-space component/pin/wire model, camera transforms, HCS runtime integration and simulation scheduling.
- Optional SDL3 graphical circuit editor (`hcs_gui`) with placement, dragging, wiring, input toggles, output display, backend selection, run/pause and manual stepping.
- Phase 5.3 `.hcs` version-1 human-readable circuit persistence, deterministic serialization, atomic model/file loading, GUI New/Open/Save/Save As, a multiline text-edit/apply view, and the headless `hcs_circuit` validator/formatter.
- Phase 5.4 UI-independent custom-component definitions (`.hcscomponent`), library/dependency resolution, hierarchical flattening into the existing runtime, stable external ports, selection extraction, and centralized UI/code font roles.
- Phase 5.2 experimental AVX2 batches for eligible wide acyclic HybridIR regions, runtime CPU feature detection, and an opt-in persistent worker pool for independent acyclic HybridIR regions.
- Hardware capability reporting (`hcs_demo --hardware-info`) and controlled CPU acceleration A/B benchmark (`hcs_hardware_bench`).
Interpreted execution never invokes native compilation. `Backend::Compiled` remains an explicit choice. `Backend::OptimizedIR` is also interpreted execution; it does not create a shared library.

## Deliberate constraints

The current API exposes every signal through `Simulator::signal(id)`. Consequently, ordinary circuits mark every signal observable and dead-logic elimination keeps every driver. The DCE pass itself is implemented and can remove logic when an IR producer supplies a narrower `observable_signals` set. Operation fusion is disabled by default and reports why it was skipped; discarding intermediate writes would change current inspection behavior. CSE preserves duplicate output signals by turning duplicate computations into forwarding operations.

The simulation core still supports one-bit signals and gates with at most two inputs. Multi-input gates, buses, delays, explicit latches, X/Z values and incremental compilation are not implemented. Phase 5.2 adds opt-in AVX2 and multicore execution only to eligible acyclic `HybridIR` work; other backends remain unchanged. AVX2 Auto currently selects scalar because benchmarks did not show a consistent win. Multicore is off by default and may be slower on small or moderately sized workloads. Vulkan/GPU compute, AVX-512 and ARM NEON are not implemented. Custom-component definitions, GUI definition authoring/editing and hierarchical flattening are implemented in the editor/model layer; self-contained dependency bundle export is not implemented. Phase 5 adds a separate editor layer and optional SDL3 frontend; it does not replace or modify the simulation model. Optimized IR supports dense fixed-point and direct event-driven execution; the separately selectable experimental HybridIR backend adds dirty-region scheduling but is not a default or automatic choice. The original interpreted backends remain available unchanged.

The cache uses a non-cryptographic FNV-1a fingerprint/checksum to detect accidental mismatches and corruption. It is intended for a user-owned local cache directory, not for loading native modules from untrusted locations.

## Requirements

- Linux (the current native loader uses `dlopen`, `fork`, and `exec`)
- C++20 compiler for the project
- C++20 compiler available at runtime for Compiled mode (default executable name: `c++`)
- CMake 3.20+

Tested in this build environment with GCC and Clang 17. The generated native module was also compiled and loaded with `clang++`. The project itself is portable C++20 except for the Linux-specific native-module loader/compiler process.

## Build

The project provides both a Makefile convenience interface and direct CMake commands.

```bash
make
```

Useful Make targets:

```bash
make test
make run
make gui
make bench
make bench-hardware
make clean
```

Select a build type or generator when needed:

```bash
make BUILD_TYPE=Debug
make GENERATOR=Ninja
```

Configure and build directly with CMake:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHCS_ENABLE_AVX2=ON
cmake --build build --parallel
```

## Build the graphical editor

The SDL3 dependency is optional for the core library and headless tools. On Arch Linux:

```bash
sudo pacman -S sdl3
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHCS_REQUIRE_SDL3=ON
cmake --build build --parallel
./build/hcs_gui
```

CMake creates `hcs_gui` when it finds SDL3 3.2+ and its CMake target (`SDL3::SDL3`). If SDL3 is absent and `HCS_REQUIRE_SDL3=OFF`, it warns and continues building the core/editor model/tests. Set `-DHCS_BUILD_GUI=OFF` for an explicitly headless build. SDL3_ttf is optional. When available, the centralized FontManager uses installed system fonts for proportional UI text and monospaced code text; without it, the editor falls back to an ASCII-only bitmap font. No font binaries are bundled. See the custom-component documentation for font candidates, licensing and Cyrillic fallback details.

The editor starts with two working examples: `Input -> NOT -> Output` and `A/B -> XOR -> Output`. Pick a component from the top palette and click the canvas to place it. Drag from an output pin to an input pin to wire components. Click an Input component to toggle it, drag component bodies to move them, use middle mouse to pan and the wheel to zoom. Delete removes the selected component/wire, Escape cancels an active placement/wire, and Space toggles run/pause.

The backend button cycles through Interpreted Event, Interpreted Dense, Optimized IR Dense, Optimized IR Event, Hybrid IR and Compiled Native. The rate button cycles through 1 Hz, 10 Hz, 60 Hz, 1 kHz and Unlimited. Unconnected gate/DFF inputs are tied to logic 0 until wired.

## Circuit files (Phase 5.3)

The editor saves circuits as versioned, human-readable `.hcs` text files. Use NEW, OPEN, SAVE and SAVE AS in the second toolbar row; this minimal SDL frontend uses an in-window path prompt rather than adding a file-dialog dependency. Unsaved changes trigger a Save / Discard / Cancel prompt, and Save As asks before replacing an existing different file. The window title and status bar mark modified documents with `*`.

Use TEXT or `Ctrl+T` to edit the canonical circuit text. APPLY TEXT validates the full document and replaces the graph/runtime only on success. Invalid text remains in the editor with a line-aware error; RELOAD TEXT explicitly discards the text buffer and regenerates it from the graph. `Ctrl+S` applies and saves in text mode, `Ctrl+R` reloads from the graph, and `Ctrl+T` returns to the graph while preserving unapplied text.

The format and CLI are documented in [`docs/CIRCUIT_FORMAT.md`](docs/CIRCUIT_FORMAT.md); the Phase 5.3 implementation and verification report is [`docs/PHASE5_3_REPORT.md`](docs/PHASE5_3_REPORT.md):

```bash
./build/hcs_circuit validate examples/xor.hcs
./build/hcs_circuit print examples/xor.hcs
./build/hcs_circuit format examples/xor.hcs formatted.hcs

# Validate a circuit that uses definitions from a specific library directory.
./build/hcs_circuit validate examples/half_adder_instance.hcs \
  --library-dir examples/components
```

## Custom components (Phase 5.4)

Custom definitions use `.hcscomponent` (`HCSC 1`) and can be composed from existing built-in components and nested custom definitions. Root `.hcs` files remain version 1 and refer to definitions by stable ID using `CUSTOM definition=<id>`; they do not duplicate implementation graphs. The default library directory is `$XDG_DATA_HOME/hcs/components` or `~/.local/share/hcs/components`. Copy referenced definition files alongside a circuit when moving it to another machine.

The runtime validates and flattens custom hierarchies into ordinary editor components before using the existing `CircuitBuilder` and `hcs::Simulator`. It does not add a separate custom-component evaluator or alter backend semantics. Default safety limits are 16 nested levels, 100,000 expanded built-in components and 4,096 dependency contexts.

The GUI supports multi-selection extraction through **MAKE CUSTOM**, empty definition creation through **NEW CUSTOM**, a separate definition-edit mode, add/remove/rename external ports, metadata editing, nested component placement from the library, and transactional **SAVE DEF** / **CANCEL EDIT**. The library panel supports search, category filtering and named pin previews. Selection-derived ports are generated from selected terminal components and cut wires; a dedicated arbitrary-signal port-mapping wizard and self-contained dependency bundle export are not included. See [`docs/CUSTOM_COMPONENTS.md`](docs/CUSTOM_COMPONENTS.md), [`docs/GUI_ARCHITECTURE.md`](docs/GUI_ARCHITECTURE.md) and the [Phase 5.4 implementation report](docs/PHASE5_4_REPORT.md).

Example definitions are in `examples/components/`: Half Adder, Full Adder (requires Half Adder), and DFF Cell. `examples/half_adder_instance.hcs` demonstrates a root circuit that references the Half Adder; use `hcs_circuit ... --library-dir examples/components` to validate, print or format it.

## Visual component symbols (Phase 5.5)

Custom definitions can store an editable vector appearance in an optional `symbol-begin` / `symbol-end` block. The SDL3 editor's **SYMBOL** view provides drawing tools for rectangles, rounded rectangles, circles, ellipses, lines, polygons, paths, arcs and text; ports can be moved to arbitrary symbol coordinates and oriented independently from their logical input/output direction. **SYMBOL TEXT** exposes the same underlying model as deterministic `HCSS 1` text for exact colors, coordinates, stroke widths, text alignment and other properties. Symbol-only changes do not alter the custom component's logic or rebuild the simulator. Old `.hcscomponent` files without a symbol block keep working with a generated default symbol. See [`docs/SYMBOL_DESIGNER.md`](docs/SYMBOL_DESIGNER.md) and the five standalone samples in `examples/symbols/`.

## Hardware acceleration (Phase 5.2)

The scalar implementation is always available. AVX2 is built in an isolated translation unit on x86 GCC/Clang builds and runtime-detected; the project is not globally compiled with `-mavx2`. AVX2 is available only for wide same-opcode levels inside acyclic HybridIR regions and is explicitly opt-in. `SimdMode::Auto` currently remains scalar because the controlled measurements did not demonstrate a repeatable speedup.

Optional multicore execution uses a persistent worker pool for sufficiently large batches of independent acyclic HybridIR regions. Feedback/cyclic regions, all other backends and DFF state transitions remain serial. It is off by default; thread scheduling was slower on most tested workloads.

```bash
./build/hcs_demo --hardware-info
./build/hcs_hardware_bench 20

# Build without the AVX2 object (portable scalar-only configuration):
cmake -S . -B build-scalar -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHCS_ENABLE_AVX2=OFF -DHCS_BUILD_GUI=OFF
```

The `AccelerationConfig` C++ API selects `SimdMode::{Off, Auto, AVX2}` and `CpuParallelMode::{Off, Auto, Custom}` independently of the backend. Custom worker count is 1–64 total threads including the caller. GPU compute is not implemented; the API rejects GPU enablement and the GUI marks it unavailable. See [`HARDWARE_ACCELERATION_AUDIT.md`](HARDWARE_ACCELERATION_AUDIT.md) and [`docs/HARDWARE_ACCELERATION.md`](docs/HARDWARE_ACCELERATION.md). Detailed SIMD, worker-pool and GPU status documents are [`docs/SIMD_IMPLEMENTATION.md`](docs/SIMD_IMPLEMENTATION.md), [`docs/MULTICORE_EXECUTION.md`](docs/MULTICORE_EXECUTION.md), and [`docs/GPU_COMPUTE.md`](docs/GPU_COMPUTE.md).

## Run the demo

```bash
./build/hcs_demo
```

## Run correctness tests

```bash
ctest --test-dir build --output-on-failure
```

The native-backend test compiles a generated shared library, verifies its outputs against the interpreted reference, and checks that the second simulator construction uses the cache. Randomized differential tests include combinational and sequential circuits; `HCS_RANDOM_CIRCUITS`, `HCS_RANDOM_SEQUENTIAL_CASES`, and `HCS_RANDOM_SEQUENTIAL_STEPS` can reduce their workload for CI.

## Run benchmarks

```bash
./build/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 20 --cold-runs 3 --warmup 4 --min-sample-ms 1 \
  --compile-mode both --cache .bench-cache-phase4.1 \
  --output-prefix benchmark-results/benchmark-results-phase4.1-summary
```

The 13 scenarios are: small/large combinational, binary counter, large sequential, sparse toggle, high switching, deep chain, wide fan-out, repeated expressions, redundant logic, feedback-stable, feedback-dynamic, and independent regions. Each scenario compares interpreted event, interpreted dense, optimized IR dense, optimized IR event, native cold, and native warm.

By default, benchmark CSV/JSON files are written under `benchmark-results/`. The default output prefix is `benchmark-results/benchmark-results`; `make bench-hardware` writes its three CSV files to the same directory. Set `BENCH_OUTPUT_DIR` to change the destination for Makefile targets, or pass `--output-dir` to `hcs_hardware_bench`.

The harness probes each backend independently, calibrates a common iteration count from the fastest backend, validates actual sample durations, and retries the entire scenario with fresh simulators if any sample is too short. It checks final checksums across backends and writes summary CSV, raw-sample CSV, and JSON. Use `--max-calibration-iterations` to cap calibration. Cold native samples use an isolated cache; warm samples must load a validated cache artifact. Interpreted modes do not touch the native cache.

Use `--scenario sparse-toggle` to run one named workload. `--profile` enables intrusive event/operation counters and micro-timings; keep it disabled for throughput measurements. `--native-compiler clang++` selects Clang for generated native modules, and `--optimization-flag -O3` changes the native optimization level.

A profile run can be generated with:

```bash
./build/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 3 --warmup 1 --min-sample-ms 0 --compile-mode warm \
  --profile --output-prefix benchmark-results/profiling-results-phase4.1
```

The Phase 3 and Phase 4 artifacts are preserved. Phase 4.1 adds `benchmark-results/benchmark-results-phase4.1-summary.csv`, `benchmark-results/benchmark-results-phase4.1-summary-raw.csv`, `benchmark-results/benchmark-results-phase4.1-summary.json`, and corresponding `benchmark-results/profiling-results-phase4.1.*` files. It also adds Phase 4-to-4.1 and controlled sequential comparisons. See [`BENCHMARKS.md`](BENCHMARKS.md), [`docs/PERFORMANCE_REPORT_PHASE4.md`](docs/PERFORMANCE_REPORT_PHASE4.md), and [`docs/PERFORMANCE_REPORT_PHASE4.1.md`](docs/PERFORMANCE_REPORT_PHASE4.1.md) for methodology, results, and limitations.

To keep generated native code out of the default cache location, use `--cache ./local-native-cache` or set `HCS_CACHE_DIR` / `XDG_CACHE_HOME`.

## API sketch

```cpp
hcs::CircuitBuilder builder;
auto a = builder.add_input();
auto b = builder.add_input();
auto y = builder.add_binary(hcs::LogicOp::Xor, a, b);
auto circuit = builder.build();

hcs::SimulatorConfig config;
config.backend = hcs::Backend::Interpreted;
config.strategy = hcs::InterpreterStrategy::EventDriven;
hcs::Simulator sim(circuit, config);

sim.set_input(a, true);
sim.set_input(b, false);
const auto step = sim.step();
bool output = sim.signal(y);
```

To run optimized IR without native compilation:

```cpp
hcs::SimulatorConfig config;
config.backend = hcs::Backend::OptimizedIR;
hcs::Simulator sim(circuit, config);
```

To experiment with region-dirty scheduling without native compilation:

```cpp
hcs::SimulatorConfig config;
config.backend = hcs::Backend::HybridIR;
hcs::Simulator sim(circuit, config);
```

`HybridIR` evaluates acyclic regions in topological order and uses a local FIFO operation queue for feedback regions. It is workload-dependent and remains experimental; compare it with `OptimizedIR` Dense/EventDriven and the optional compiled backend before choosing it.

To opt into native compilation from optimized IR:

```cpp
hcs::SimulatorConfig config;
config.backend = hcs::Backend::Compiled;
config.native_options.cache_directory = "./native-cache";
hcs::Simulator sim(circuit, config);
```

Compiler preparation can be inspected through `prepare_circuit(circuit)`. It returns original/optimized IR, dependency analysis, per-pass results, and phase timing statistics. `optimizer::dump(prepared.original_ir)` / `optimizer::dump(prepared.optimized_ir)` print IR; `analysis::dump(prepared.dependencies)` prints dependencies and regions. `NativeModule::compiler_statistics()` and `NativeModule::timings()` expose preparation counts and native codegen/cache/compiler/load timings. The generated C++ is kept next to the cached `.so`, and `NativeModule::source_map()` retains the IR-to-source component mapping.

`Simulator` keeps a reference to the immutable `Circuit`, so the circuit object must outlive the simulator.

## Development documents

- [`docs/GUI_ARCHITECTURE.md`](docs/GUI_ARCHITECTURE.md): SDL3 application and layer boundaries.
- [`docs/EDITOR_MODEL.md`](docs/EDITOR_MODEL.md): world-space editor data model and interaction rules.
- [`docs/HCS_GUI_INTEGRATION.md`](docs/HCS_GUI_INTEGRATION.md): editor-to-simulator mapping and scheduling.

- [`ARCHITECTURE.md`](ARCHITECTURE.md): verified implementation audit and compiler design.
- [`ROADMAP.md`](ROADMAP.md): completed and remaining milestones.
- [`BENCHMARKS.md`](BENCHMARKS.md): benchmark discrepancy audit and reproducible baseline.
- [`benchmark-results/benchmark-results-phase3.csv`](benchmark-results/benchmark-results-phase3.csv): preserved historical baseline.
- [`benchmark-results/benchmark-comparison-phase3-phase4.csv`](benchmark-results/benchmark-comparison-phase3-phase4.csv): directional comparison on unchanged workload builders.
- [`benchmark-results/benchmark-o2-o3-comparison.csv`](benchmark-results/benchmark-o2-o3-comparison.csv): native `-O2` versus `-O3` measurements.
- [`benchmark-results/benchmark-results-phase4.csv`](benchmark-results/benchmark-results-phase4.csv): historical Phase 4 12-scenario, 6-mode summary.
- [`benchmark-results/benchmark-results-phase4-raw.csv`](benchmark-results/benchmark-results-phase4-raw.csv): raw measured samples.
- [`benchmark-results/benchmark-results-phase4.json`](benchmark-results/benchmark-results-phase4.json): structured Phase 4 benchmark results.
- [`benchmark-results/benchmark-results-phase4.1-summary.csv`](benchmark-results/benchmark-results-phase4.1-summary.csv): Phase 4.1 summary (78 rows).
- [`benchmark-results/benchmark-results-phase4.1-summary-raw.csv`](benchmark-results/benchmark-results-phase4.1-summary-raw.csv): Phase 4.1 raw samples (1,339 rows).
- [`benchmark-results/benchmark-results-phase4.1-summary.json`](benchmark-results/benchmark-results-phase4.1-summary.json): structured Phase 4.1 results.
- [`benchmark-results/benchmark-comparison-phase4-phase4.1.csv`](benchmark-results/benchmark-comparison-phase4-phase4.1.csv): directional Phase 4 versus Phase 4.1 comparison.
- [`benchmark-results/benchmark-sequential-before-after-phase4.1.csv`](benchmark-results/benchmark-sequential-before-after-phase4.1.csv): controlled counter/large-sequential comparison.
- [`benchmark-results/benchmark-scratch-buffer-ab-phase4.1.csv`](benchmark-results/benchmark-scratch-buffer-ab-phase4.1.csv): scratch-buffer enabled/disabled A/B.
- [`benchmark-results/profiling-comparison-phase4-phase4.1.csv`](benchmark-results/profiling-comparison-phase4-phase4.1.csv): common profiler-counter comparison.
- [`benchmark-results/profiling-results-phase4.csv`](benchmark-results/profiling-results-phase4.csv): historical Phase 4 runtime profiling counters.
- [`benchmark-results/profiling-results-phase4.1.csv`](benchmark-results/profiling-results-phase4.1.csv): expanded Phase 4.1 profiler summary.
- [`benchmark-results/profiling-results-phase4.1-raw.csv`](benchmark-results/profiling-results-phase4.1-raw.csv): raw Phase 4.1 profiling samples.
- [`benchmark-results/profiling-results-phase4.1.json`](benchmark-results/profiling-results-phase4.1.json): structured Phase 4.1 profiling output.
- [`docs/PERFORMANCE_REPORT_PHASE4.md`](docs/PERFORMANCE_REPORT_PHASE4.md): historical Phase 4 performance analysis.
- [`docs/PERFORMANCE_REPORT_PHASE4.1.md`](docs/PERFORMANCE_REPORT_PHASE4.1.md): Phase 4.1 benchmark, profiling, and sequential optimization analysis.
- [`docs/PHASE4.3_RESEARCH.md`](docs/PHASE4.3_RESEARCH.md): Phase 4.3 executor audit, hybrid scheduling design and limitations.
- [`docs/PERFORMANCE_REPORT_PHASE4.3.md`](docs/PERFORMANCE_REPORT_PHASE4.3.md): full Phase 4.3 benchmark matrix, 20-run targeted experiments, and correctness/build results.
- [`docs/SIMD_FEASIBILITY.md`](docs/SIMD_FEASIBILITY.md): SIMD/bit-slicing feasibility.
- [`CHANGELOG.md`](CHANGELOG.md): changes in this development phase.
- [`docs/semantics.md`](docs/semantics.md): runtime semantics and backend behavior.

## Inspection tool

`hcs_inspect` exposes a small deterministic inspection fixture for development:

```bash
./build/hcs_inspect text
./build/hcs_inspect json
./build/hcs_inspect dot > graph.dot
```

The production library keeps diagnostics opt-in; normal simulation does not require these exports.


## Phase 4.2: profiling-guided runtime investigation

Phase 4.2 audited the complete Phase 4.1 runtime and ran controlled experiments without replacing the existing architecture. Profiling remains disabled by default. Three candidate changes were evaluated independently and reverted because results were mixed or within measurement variability; the final simulator runtime source is unchanged from Phase 4.1. This is intentional: no optimization was retained without repeatable evidence.

The full benchmark keeps all 13 scenarios and six backend configurations. In the supplied 3-vCPU KVM build environment it produced 78 summary rows and 364 accepted raw samples (5 measured runs for interpreted/warm backends, 3 cold-native runs). Separate profile results contain 78 rows and 208 samples. A separate 512-DFF sequential workload and raw A/B records are included. The Phase 4.1 comparison is directional/unpaired because calibration and repetition counts differ.

See [`docs/PHASE4.2_AUDIT.md`](docs/PHASE4.2_AUDIT.md) for source findings and [`docs/PERFORMANCE_REPORT_PHASE4.2.md`](docs/PERFORMANCE_REPORT_PHASE4.2.md) for measured results, rejected experiments, test/build matrix, profiler overhead, hardware-tool limitations and SIMD decision.

Phase 4.2 artifacts:

- [`benchmark-results/benchmark-results-phase4.2-summary.csv`](benchmark-results/benchmark-results-phase4.2-summary.csv), [`benchmark-results/benchmark-results-phase4.2-raw.csv`](benchmark-results/benchmark-results-phase4.2-raw.csv), [`benchmark-results/benchmark-results-phase4.2-summary.json`](benchmark-results/benchmark-results-phase4.2-summary.json).
- [`benchmark-results/profiling-results-phase4.2.csv`](benchmark-results/profiling-results-phase4.2.csv), [`benchmark-results/profiling-results-phase4.2-raw.csv`](benchmark-results/profiling-results-phase4.2-raw.csv), [`benchmark-results/profiling-results-phase4.2.json`](benchmark-results/profiling-results-phase4.2.json).
- [`benchmark-results/profiling-overhead-phase4.2.csv`](benchmark-results/profiling-overhead-phase4.2.csv) and raw samples.
- [`benchmark-results/optimization-experiments-phase4.2.csv`](benchmark-results/optimization-experiments-phase4.2.csv) and raw samples for all three rejected candidates.
- [`benchmark-results/benchmark-comparison-phase4.1-phase4.2.csv`](benchmark-results/benchmark-comparison-phase4.1-phase4.2.csv), a directional rather than paired comparison.
- `benchmark-results/benchmark-results-phase4.2-512dff.csv` and related raw/JSON results for an additional 512-DFF stress run.


## Phase 4.3: experimental region-dirty execution

Phase 4.3 adds a separate `Backend::HybridIR` without changing the existing interpreted, optimized-IR dense/event or compiled executors. Acyclic regions are executed once in deterministic topological order when dirty; cyclic regions use reusable per-region FIFO operation queues and duplicate suppression. Region IDs are recalculated from optimized IR after optimization. DFF sampling/commit remains in the common simulator core.

The full benchmark retains all 13 previous scenarios and six backend configurations, adds two region-focused scenarios, and adds `optimized-ir-hybrid` as a seventh backend: 105 summary rows and 465 accepted raw samples. A targeted size-512 experiment with 20 repetitions measured 34.8% higher throughput than optimized IR event on sparse independent chains; high-activity execution remains slower than optimized dense and native warm. Hybrid stays experimental and is never selected automatically.

See [`docs/PHASE4.3_RESEARCH.md`](docs/PHASE4.3_RESEARCH.md) and [`docs/PERFORMANCE_REPORT_PHASE4.3.md`](docs/PERFORMANCE_REPORT_PHASE4.3.md). Phase 4.2 and earlier historical files remain unchanged. Benchmark exports: `benchmark-results/benchmark-results-phase4.3-summary.csv`, `benchmark-results/benchmark-results-phase4.3-raw.csv`, `benchmark-results/benchmark-results-phase4.3-summary.json`, `benchmark-results/profiling-results-phase4.3.csv` and companions, plus `benchmark-results/benchmark-comparison-phase4.2-phase4.3.csv`.
