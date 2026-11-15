# GPU Compute — Phase 5.2 Status

## Status: not implemented

HCS does not contain a Vulkan compute backend in Phase 5.2. There are no Vulkan shaders, command buffers, descriptor sets, device buffers, submission/synchronization path, CPU/GPU transfer path, or GPU circuit executor. No CUDA or vendor-specific GPU dependency is present.

The current environment exposed only the Vulkan runtime loader `libvulkan.so.1`. It did not provide Vulkan headers/development files, `glslc`, `glslangValidator`, or `/dev/dri`. As a result, an actual compute backend could not be compiled, initialized, run or differentially tested here. A runtime loader without the headers, shader compiler and device access is insufficient evidence of Vulkan compute availability.

## Current behavior

- `HardwareCapabilities::vulkan_compute_compiled` is false.
- `HardwareCapabilities::gpu_device_available` is false. This means no GPU is usable by HCS; it is not a claim that the physical host has no GPU.
- `AccelerationConfig::gpu_enabled=true` is rejected with a clear runtime error.
- The SDL3 GUI displays `GPU: N/A` and does not offer a fake device selector.
- There is no `HCS_ENABLE_VULKAN` CMake option because it would imply a buildable backend that does not currently exist.

## Required future work

A later GPU phase must add optional Vulkan discovery and isolated GPU sources, device enumeration, capability checks, shader compilation/loading, a documented supported IR subset, upload/download buffers, synchronization, device-loss/error fallback, and scalar-vs-GPU differential tests. It must measure both kernel time and end-to-end time including transfer/synchronization costs. It must not dispatch arbitrary circuits to a GPU without proving that dependencies, feedback and sequential semantics are supported.

GPU execution should remain a user-selected backend/acceleration mode. CPU simulation must remain available even when Vulkan is absent or initialization fails.
