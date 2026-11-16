# Hardware Acceleration

## Availability

Phase 5.2 adds experimental AVX2 and multicore execution for the opt-in `Backend::HybridIR` only. Every existing interpreted, optimized-IR, HybridIR scalar, and compiled-native route remains available. Other backends ignore the acceleration configuration for execution and continue using their existing implementation.

| Capability | Status | Scope |
|---|---|---|
| Scalar CPU | Available | Mandatory fallback for every build |
| AVX2 | Available when built and detected | Eligible acyclic HybridIR operation batches only |
| CPU worker pool | Available | Independent acyclic HybridIR regions only |
| AVX2 + workers | Available | Same safe region-level constraints; experimental |
| AVX-512 | Not implemented | Scalar fallback |
| ARM NEON | Not implemented | Scalar fallback |
| Vulkan compute | Not implemented | Explicitly unavailable; no GPU fallback is implied |

The current signal domain is binary (`uint8_t` 0/1). X/Z logic is not part of the current HCS core and is not implemented by these acceleration kernels.

## Configure and build

AVX2 CPU support is detected independently from whether an AVX2 kernel was compiled. The kernel is built in a separate translation unit by default on x86 GNU/Clang builds. HCS reports CPU support, kernel build availability, and whether AVX2 is usable as separate capability fields. The whole application is not compiled with `-mavx2`.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHCS_ENABLE_AVX2=ON -DHCS_BUILD_GUI=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build without any AVX2 object:

```bash
cmake -S . -B build-scalar -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DHCS_ENABLE_AVX2=OFF -DHCS_BUILD_GUI=OFF
cmake --build build-scalar --parallel
ctest --test-dir build-scalar --output-on-failure
```

Makefile equivalents:

```bash
make HCS_ENABLE_AVX2=ON HCS_BUILD_GUI=OFF
make test HCS_ENABLE_AVX2=OFF HCS_BUILD_GUI=OFF BUILD_DIR=build-scalar
make bench-hardware HCS_BUILD_GUI=OFF BENCH_RUNS=20
```

## Runtime configuration

```cpp
hcs::SimulatorConfig config;
config.backend = hcs::Backend::HybridIR;
config.acceleration.simd = hcs::SimdMode::Off;      // Off, Auto, AVX2
config.acceleration.parallel = hcs::CpuParallelMode::Off; // Off, Auto, Custom
config.acceleration.worker_threads = 4;             // Custom: total threads, including caller
hcs::Simulator simulator(circuit, config);
```

- `SimdMode::Off`: scalar evaluation.
- `SimdMode::Auto`: currently scalar. Phase 5.2 did not demonstrate a repeatable AVX2 win, so Auto does not choose AVX2.
- `SimdMode::AVX2`: explicitly requests the AVX2 kernel. Construction fails clearly if the AVX2 kernel was not built or the CPU/OS does not support it. Detailed profiling deliberately uses scalar execution for exact operation counters.
- `CpuParallelMode::Off`: serial HybridIR execution (default).
- `CpuParallelMode::Auto`: uses `hardware_concurrency()` capped at 32 total threads.
- `CpuParallelMode::Custom`: uses `worker_threads` total threads, including the calling thread; valid range is 1–64.
- `gpu_enabled=true`: rejected because no GPU executor is implemented.

Acceleration is independent from backend selection. In practice, only HybridIR uses the new SIMD and worker-pool paths. The GUI exposes the selected mode, actual SIMD implementation, worker count, and an explicit GPU unavailable state. The CLI supports `hcs_demo --hardware-info` for detected architecture/AVX2/build capability; per-simulation acceleration is configured through the C++ API.

## Correctness boundary

The accelerated path is not allowed to change backend selection or simulation semantics. AVX2 evaluates the same binary operation truth tables. Parallel workers only evaluate independent acyclic weakly connected regions; cyclic/feedback regions execute serially. DFF next-state calculation and simultaneous commit are unchanged and always remain outside the worker pool. See the dedicated implementation documents for the exact restrictions.

## Benchmark summary

The benchmark command below writes raw and summary CSV files in the current directory:

```bash
./build/hcs_hardware_bench 20
```

It uses deterministic workloads, a separate warm-up simulator, a separate measured simulator, rotated configuration order, equal tick counts, checksum checks, and operation-count checks. It reports median, mean, min/max, standard deviation, P95, tick throughput, operation throughput, simulator setup time, process CPU time/approximate process CPU percentage, and actual SIMD/parallel path counters.

The Phase 5.2 host was a virtualized Intel Xeon Platinum 8573C x86-64 guest with 3 logical CPUs visible and a 2-CPU cgroup quota. It supported AVX2. Results are in `../benchmark-results/hardware-bench-results-phase5.2-{raw,summary}.csv`. Neither AVX2 nor multicore won consistently. Auto therefore remains scalar/off. Detailed results and caveats are in `docs/SIMD_IMPLEMENTATION.md` and `docs/MULTICORE_EXECUTION.md`.

## Disabling acceleration

Use the default `Off` settings, or explicitly select `SimdMode::Off` and `CpuParallelMode::Off`. Build scalar-only with `-DHCS_ENABLE_AVX2=OFF`. Vulkan is not compiled or available and cannot be enabled in this phase.
