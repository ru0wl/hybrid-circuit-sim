# Hardware Acceleration Audit — HCS Phase 5.2

## Scope and method

The Phase 5.2 audit was performed against the Phase 5 source tree before adding hardware-specific code. It covered the CMake/Makefile build, `Simulator` and its backends, optimized IR, dependency/SCC/region analysis, the HybridIR scheduler, DFF transitions, GUI settings, tests, and benchmark infrastructure.

The host used for implementation and measurements was a virtualized Linux x86-64 environment:

- CPU model reported by the guest: Intel Xeon Platinum 8573C.
- `std::thread::hardware_concurrency()` / `lscpu`: 3 logical CPUs exposed.
- cgroup CPU quota: `200000 100000` (2 CPU quota).
- Runtime AVX2 CPU feature detection: available; the AVX2 kernel was built for the AVX-enabled configuration.
- GCC 14.2.0, Clang 17.0.0, CMake 3.31.6.
- No `/dev/dri`, Vulkan headers/development package, `glslc`, `glslangValidator`, `perf`, or Valgrind/Callgrind was available. A Vulkan runtime loader (`libvulkan.so.1`) alone was present.

The host is virtualized and CPU quota constrained. Results are not a prediction for a desktop CPU or a machine with a different core topology.

## Findings before implementation

### Explicitly implemented acceleration

None of the pre-Phase-5.2 sources contained explicit AVX/SSE/NEON intrinsics, a hardware-specific executor, a thread pool, or a GPU compute backend.

The existing `Compiled` backend emits ordinary C++ and loads a native shared object. It is a CPU native-code backend, not SIMD-specific code, multithreading, CUDA, or Vulkan.

### Compiler-generated acceleration

The original simulator used scalar `uint8_t` signals and a per-operation opcode switch. The Phase 4.2 compiler-vectorization review did not establish that the irregular signal-ID gather/scatter execution loops were usefully auto-vectorized. No existing code forced AVX2 globally. GCC/Clang may still apply ordinary target-independent optimizations, but there was no explicit SIMD dispatch or guarantee of AVX2 execution.

### Existing but unused infrastructure

- `analysis::DependencyGraph` already represented operation predecessors/successors, SCCs, and weakly connected operation regions.
- `Backend::HybridIR` already executed acyclic regions in topological order and cyclic regions with local FIFO scheduling.
- Weakly connected regions provided a potential unit of parallel work: operation outputs are within a region, and separate regions have no producer-to-consumer operation edges between them. Shared primary/state inputs are read-only during a combinational settle.
- `Simulator::step()` already captured every DFF next state before committing the Q signals. This boundary was suitable to keep wholly sequential and unchanged.
- The SDL3 GUI already exposed backend/runtime controls and had room for an execution-settings row.

### Missing capabilities

Before Phase 5.2 the repository had no runtime CPU feature detection for AVX2, no SIMD kernels, no persistent worker pool, no independent-region parallel execution, no Vulkan instance/device initialization, no compute shaders, no GPU synchronization/transfer path, and no user-facing hardware acceleration configuration.

### Current signal semantics

The current core exposes Boolean values through `signal(id)` and stores signal values as `uint8_t` values 0 or 1. It does not implement X/unknown or Z/high-impedance states. The SIMD implementation preserves this existing binary truth table; it does not narrow a richer four-state model.

## Implemented in Phase 5.2

1. Added an isolated AVX2 implementation in `src/hardware/simd_avx2.cpp`. CMake applies `-mavx2` to that translation unit only. Runtime feature detection uses GCC/Clang CPU builtins, which include CPU and OS extended-state checks. The rest of HCS is not compiled with AVX2 flags.
2. Added AVX2 evaluation for batches of at least 32 same-opcode operations in the same dependency level of an acyclic HybridIR region. Inputs are gathered into 32-byte arrays, the existing Boolean operation is evaluated with AVX2 byte-wise instructions, and results are scattered back to their original observable signal IDs. Tails and unsupported paths remain scalar.
3. Added a persistent per-simulator worker pool and static region-range partitioning. Only batches of independent acyclic HybridIR regions can execute concurrently. Feedback/cyclic regions and all DFF state transitions remain serial.
4. Added `AccelerationConfig`, capability reporting, execution statistics, CLI `--hardware-info`, GUI execution settings, and `hcs_hardware_bench`.
5. Added differential tests for all current LogicOp values, randomized input sequences, independent regions, AVX2 plus multicore, feedback fallback, and worker-count validation.

## Features intentionally not implemented

No Vulkan backend was added. The environment lacked headers/SDK, shader compiler, and GPU device access. No device scan or shader execution could be truthfully tested. `vulkan_compute_compiled` and `gpu_device_available` therefore remain false; requesting `gpu_enabled` throws an explicit runtime error; GUI labels GPU as unavailable. The second field describes HCS usability, not a hardware inventory of physical GPUs. There is no CMake option that pretends to enable a backend that does not exist.

No AVX-512 or ARM NEON kernel was added. The project retains scalar support on all supported targets and compiles the AVX2 file only for x86 GNU/Clang configurations when `HCS_ENABLE_AVX2=ON`.

## Parallelization safety boundary

Parallel execution is limited to all-acyclic dirty-region batches in HybridIR. Acyclic regions are independent weakly connected components of the optimized operation dependency graph. Workers only write signals whose producer operations belong to their own region; cross-region operation dependencies do not exist, while shared external inputs are read-only during settle. If a queued batch contains any cyclic region, the entire batch follows the original serial region scheduler. Event limits are checked before dispatch. DFF capture/commit and post-commit propagation remain in the existing serial `Simulator::step()` sequence.

## Decision

Both AVX2 and multicore execution are real, opt-in implementations, but the measurements in this environment do not justify enabling either automatically. `SimdMode::Auto` currently selects Scalar. `CpuParallelMode::Off` remains the default; Auto/Custom create workers only for HybridIR, subject to the scheduler's safe-workload checks. Vulkan remains unavailable rather than represented by a non-functional switch.

See `docs/SIMD_IMPLEMENTATION.md`, `docs/MULTICORE_EXECUTION.md`, `docs/GPU_COMPUTE.md`, and the Phase 5.2 benchmark CSV files for detailed implementation and measurements.
