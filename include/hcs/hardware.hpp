#pragma once

#include <cstdint>
#include <string>

namespace hcs {

enum class SimdMode : std::uint8_t {
    Off,
    Auto,
    AVX2,
};

enum class CpuParallelMode : std::uint8_t {
    Off,
    Auto,
    Custom,
};

struct AccelerationConfig {
    SimdMode simd{SimdMode::Off};
    CpuParallelMode parallel{CpuParallelMode::Off};
    // Auto uses hardware_concurrency(); Custom requires a value >= 1.
    std::uint32_t worker_threads{0};
    // Reserved for a future Vulkan compute backend. Enabling it is rejected
    // until a real device execution path is compiled into HCS.
    bool gpu_enabled{false};
    std::int32_t gpu_device_index{-1};
};

enum class SimdImplementation : std::uint8_t {
    Scalar,
    AVX2,
};

struct HardwareCapabilities {
    std::string architecture;
    std::uint32_t logical_cpu_count{1};
    bool avx2_cpu_supported{false};
    bool avx2_kernel_compiled{false};
    bool avx2_usable{false};
    bool vulkan_compute_compiled{false};
    bool gpu_device_available{false};
};

[[nodiscard]] HardwareCapabilities detect_hardware_capabilities();
[[nodiscard]] bool avx2_cpu_supported() noexcept;
[[nodiscard]] bool avx2_kernel_compiled() noexcept;
[[nodiscard]] bool avx2_available() noexcept;
[[nodiscard]] const char* simd_mode_name(SimdMode mode) noexcept;
[[nodiscard]] const char* simd_implementation_name(SimdImplementation implementation) noexcept;
[[nodiscard]] const char* cpu_parallel_mode_name(CpuParallelMode mode) noexcept;

} // namespace hcs
