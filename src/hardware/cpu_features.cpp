#include "hcs/hardware.hpp"

#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define HCS_X86 1
#else
#define HCS_X86 0
#endif

namespace hcs {

bool avx2_cpu_supported() noexcept {
#if HCS_X86 && (defined(__GNUC__) || defined(__clang__))
    // GCC/Clang's builtin performs CPUID and OS-enabled extended-state checks.
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2");
#else
    return false;
#endif
}

bool avx2_kernel_compiled() noexcept {
#if defined(HCS_HAVE_AVX2_KERNEL)
    return true;
#else
    return false;
#endif
}

bool avx2_available() noexcept {
    return avx2_cpu_supported() && avx2_kernel_compiled();
}

HardwareCapabilities detect_hardware_capabilities() {
    HardwareCapabilities result;
#if defined(__x86_64__) || defined(_M_X64)
    result.architecture = "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    result.architecture = "x86";
#elif defined(__aarch64__)
    result.architecture = "AArch64";
#elif defined(__arm__)
    result.architecture = "ARM";
#else
    result.architecture = "unknown";
#endif
    const auto count = std::thread::hardware_concurrency();
    result.logical_cpu_count = count == 0 ? 1u : count;
    result.avx2_cpu_supported = avx2_cpu_supported();
    result.avx2_kernel_compiled = avx2_kernel_compiled();
    result.avx2_usable = result.avx2_cpu_supported && result.avx2_kernel_compiled;
    // The project does not currently contain a Vulkan compute executor.
    result.vulkan_compute_compiled = false;
    result.gpu_device_available = false;
    return result;
}

const char* simd_mode_name(SimdMode mode) noexcept {
    switch (mode) {
        case SimdMode::Off: return "Off";
        case SimdMode::Auto: return "Auto";
        case SimdMode::AVX2: return "AVX2";
    }
    return "Unknown";
}

const char* simd_implementation_name(SimdImplementation implementation) noexcept {
    switch (implementation) {
        case SimdImplementation::Scalar: return "Scalar";
        case SimdImplementation::AVX2: return "AVX2";
    }
    return "Unknown";
}

const char* cpu_parallel_mode_name(CpuParallelMode mode) noexcept {
    switch (mode) {
        case CpuParallelMode::Off: return "Off";
        case CpuParallelMode::Auto: return "Auto";
        case CpuParallelMode::Custom: return "Custom";
    }
    return "Unknown";
}

} // namespace hcs
