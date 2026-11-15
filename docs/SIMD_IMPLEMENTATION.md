# SIMD Implementation — Phase 5.2

## Scope

The only explicit SIMD implementation is AVX2, isolated in `src/hardware/simd_avx2.cpp`. CMake compiles that source with `-mavx2` while all other translation units retain the baseline target. It is included only when `HCS_ENABLE_AVX2=ON`, the target processor is x86, and the compiler is GCC or Clang.

At runtime, `avx2_cpu_supported()` uses GCC/Clang's `__builtin_cpu_init()` and `__builtin_cpu_supports("avx2")`. The builtin checks CPU support and OS-enabled extended state. `avx2_kernel_compiled()` reports whether the isolated AVX2 source was included in the build, and `avx2_available()` is true only when both CPU support and the kernel are present. This distinction is also exposed in `HardwareCapabilities`, so a scalar-only build can report that the CPU supports AVX2 even though HCS did not include its kernel. A user-selected `SimdMode::AVX2` is rejected if either condition is absent.

## Data model and kernel

The HCS core currently stores each observable signal as `uint8_t` 0 or 1. The kernel operates on 32 byte lanes per 256-bit AVX2 register. It supports all current `LogicOp` values:

- Constant0 / Constant1
- Buffer / NOT
- AND / OR / XOR
- NAND / NOR / XNOR

HybridIR builds deterministic topological dependency levels for each acyclic weakly connected region, then groups operations by opcode within each level. A group is SIMD-eligible at 32 or more operations. For each 32-operation batch, HCS gathers the operands by their signal IDs into aligned arrays, executes the opcode with AVX2 byte-wise instructions, then scatters each lane back to its original signal ID. Remaining operations use scalar evaluation. This preserves intermediate signal observability and avoids assuming arbitrary signal IDs are contiguous.

No SIMD is used for cyclic/feedback regions, profiled runs, other backends, or groups smaller than 32. Enabling detailed profiling selects the instrumented scalar path so per-operation profiler counts remain exact. Auto currently selects scalar. AVX2 must be explicitly requested.

## Correctness

`tests/test_sim.cpp` compares scalar HybridIR with AVX2 over randomized inputs for every supported opcode, including a 35-operation group to cover a scalar tail. It compares every public signal after each step. A combined AVX2 + worker-pool test verifies the same signals and operation/event counts for multiple independent wide regions. The current binary semantics are preserved; no X/Z or multi-bit value model exists in this project.

The object disassembly of the AVX2 translation unit was checked and contains actual YMM instructions (`vmovdqu`, `vpxor`, `vpand`, `vpor`), rather than merely an AVX2 label or an unused dispatch path.

## Performance results

Twenty paired repetitions were collected per workload. Configuration order was rotated, and every configuration used the same circuit, tick count and deterministic input sequence. A negative paired speedup means the accelerated configuration was slower; `wins` counts paired runs faster than scalar.

| Workload | Configuration | Paired median throughput change | Wins | Median SIMD batches/run |
|---|---|---:|---:|---:|
| independent-regions | AVX2 | +0.56% | 11/20 | 0 |
| wide-independent-level | AVX2 | +8.06% | 12/20 | 1,020 |
| hybrid-wide-independent-regions | AVX2 | −16.48% | 4/20 | 1,920 |
| hybrid-large-independent-regions | AVX2 | −10.22% | 4/20 | 12,240 |

The wide single-region layer is the one workload where the paired result suggests a possible AVX2 gain. It is not considered confirmed: only 12 of 20 paired runs won, scalar sample variability was high, and other large region workloads were slower with AVX2. The independent chain workload did not execute the SIMD kernel at all (`simd_batches=0`); its small timing change is ordinary noise. All configurations executed identical operation counts and final checksums.

The benchmark counters confirm that AVX2 batches actually ran on the eligible wide workloads. However, no reliable cross-workload speedup was established. The likely cost on other topologies is the gather/scatter needed for arbitrary signal IDs plus a relatively small amount of Boolean work per lane; this explanation is an inference from the layout and measured timings, not a hardware-counter finding. Auto therefore remains scalar. AVX2 remains an explicit experimental choice for users who want to test their own wide acyclic circuits.

## Future work not included

No AVX-512, NEON, bit-sliced multi-instance engine, arbitrary-gather vectorized executor, or signal-layout rewrite was added. Those would require a suitable workload and new controlled measurements. The scalar implementation remains mandatory and available on every build.
