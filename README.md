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
- Compiled backend now lowers and optimizes IR before generating C++ and retains the existing `.so` / `dlopen` architecture.
- Preparation statistics and IR dump APIs.
- Headless correctness tests and benchmark comparisons for event, dense, optimized IR, and compiled backends.

Interpreted execution never invokes native compilation. `Backend::Compiled` remains an explicit choice. `Backend::OptimizedIR` is also interpreted execution; it does not create a shared library.

## Deliberate constraints

The current API exposes every signal through `Simulator::signal(id)`. Consequently, ordinary circuits mark every signal observable and dead-logic elimination keeps every driver. The DCE pass itself is implemented and can remove logic when an IR producer supplies a narrower `observable_signals` set. Operation fusion is disabled by default and reports why it was skipped; discarding intermediate writes would change current inspection behavior. CSE preserves duplicate output signals by turning duplicate computations into forwarding operations.

The simulator currently supports one-bit signals and gates with at most two inputs. Multi-input gates, buses, delays, latches, X/Z values, incremental compilation, SIMD, multithreading, nested custom components, and editor integration are not implemented. Optimized IR supports both dense fixed-point and direct event-driven execution; the original interpreted backends remain available unchanged.

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
make bench
make clean
```

Select a build type or generator when needed:

```bash
make BUILD_TYPE=Debug
make GENERATOR=Ninja
```

Configure and build directly with CMake:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

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
  --output-prefix benchmark-results-phase4.1-summary
```

The 13 scenarios are: small/large combinational, binary counter, large sequential, sparse toggle, high switching, deep chain, wide fan-out, repeated expressions, redundant logic, feedback-stable, feedback-dynamic, and independent regions. Each scenario compares interpreted event, interpreted dense, optimized IR dense, optimized IR event, native cold, and native warm.

The harness probes each backend independently, calibrates a common iteration count from the fastest backend, validates actual sample durations, and retries the entire scenario with fresh simulators if any sample is too short. It checks final checksums across backends and writes summary CSV, raw-sample CSV, and JSON. Use `--max-calibration-iterations` to cap calibration. Cold native samples use an isolated cache; warm samples must load a validated cache artifact. Interpreted modes do not touch the native cache.

Use `--scenario sparse-toggle` to run one named workload. `--profile` enables intrusive event/operation counters and micro-timings; keep it disabled for throughput measurements. `--native-compiler clang++` selects Clang for generated native modules, and `--optimization-flag -O3` changes the native optimization level.

A profile run can be generated with:

```bash
./build/hcs_bench --scenario all --size 256 --iterations 100 \
  --runs 3 --warmup 1 --min-sample-ms 0 --compile-mode warm \
  --profile --output-prefix profiling-results-phase4.1
```

The Phase 3 and Phase 4 artifacts are preserved. Phase 4.1 adds `benchmark-results-phase4.1-summary.csv`, `benchmark-results-phase4.1-summary-raw.csv`, `benchmark-results-phase4.1-summary.json`, and corresponding `profiling-results-phase4.1.*` files. It also adds Phase 4-to-4.1 and controlled sequential comparisons. See [`BENCHMARKS.md`](BENCHMARKS.md), [`docs/PERFORMANCE_REPORT_PHASE4.md`](docs/PERFORMANCE_REPORT_PHASE4.md), and [`docs/PERFORMANCE_REPORT_PHASE4.1.md`](docs/PERFORMANCE_REPORT_PHASE4.1.md) for methodology, results, and limitations.

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

- [`ARCHITECTURE.md`](ARCHITECTURE.md): verified implementation audit and compiler design.
- [`ROADMAP.md`](ROADMAP.md): completed and remaining milestones.
- [`BENCHMARKS.md`](BENCHMARKS.md): benchmark discrepancy audit and reproducible baseline.
- [`benchmark-results-phase3.csv`](benchmark-results-phase3.csv): preserved historical baseline.
- [`benchmark-comparison-phase3-phase4.csv`](benchmark-comparison-phase3-phase4.csv): directional comparison on unchanged workload builders.
- [`benchmark-o2-o3-comparison.csv`](benchmark-o2-o3-comparison.csv): native `-O2` versus `-O3` measurements.
- [`benchmark-results-phase4.csv`](benchmark-results-phase4.csv): historical Phase 4 12-scenario, 6-mode summary.
- [`benchmark-results-phase4-raw.csv`](benchmark-results-phase4-raw.csv): raw measured samples.
- [`benchmark-results-phase4.json`](benchmark-results-phase4.json): structured Phase 4 benchmark results.
- [`benchmark-results-phase4.1-summary.csv`](benchmark-results-phase4.1-summary.csv): Phase 4.1 summary (78 rows).
- [`benchmark-results-phase4.1-summary-raw.csv`](benchmark-results-phase4.1-summary-raw.csv): Phase 4.1 raw samples (1,339 rows).
- [`benchmark-results-phase4.1-summary.json`](benchmark-results-phase4.1-summary.json): structured Phase 4.1 results.
- [`benchmark-comparison-phase4-phase4.1.csv`](benchmark-comparison-phase4-phase4.1.csv): directional Phase 4 versus Phase 4.1 comparison.
- [`benchmark-sequential-before-after-phase4.1.csv`](benchmark-sequential-before-after-phase4.1.csv): controlled counter/large-sequential comparison.
- [`benchmark-scratch-buffer-ab-phase4.1.csv`](benchmark-scratch-buffer-ab-phase4.1.csv): scratch-buffer enabled/disabled A/B.
- [`profiling-comparison-phase4-phase4.1.csv`](profiling-comparison-phase4-phase4.1.csv): common profiler-counter comparison.
- [`profiling-results-phase4.csv`](profiling-results-phase4.csv): historical Phase 4 runtime profiling counters.
- [`profiling-results-phase4.1.csv`](profiling-results-phase4.1.csv): expanded Phase 4.1 profiler summary.
- [`profiling-results-phase4.1-raw.csv`](profiling-results-phase4.1-raw.csv): raw Phase 4.1 profiling samples.
- [`profiling-results-phase4.1.json`](profiling-results-phase4.1.json): structured Phase 4.1 profiling output.
- [`docs/PERFORMANCE_REPORT_PHASE4.md`](docs/PERFORMANCE_REPORT_PHASE4.md): historical Phase 4 performance analysis.
- [`docs/PERFORMANCE_REPORT_PHASE4.1.md`](docs/PERFORMANCE_REPORT_PHASE4.1.md): Phase 4.1 benchmark, profiling, and sequential optimization analysis.
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
