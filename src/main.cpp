#include "hcs/simulator.hpp"

#include <iostream>
#include <string_view>

namespace {
int print_hardware_info() {
    const auto hardware = hcs::detect_hardware_capabilities();
    std::cout << "HCS hardware capabilities\n"
              << "Architecture: " << hardware.architecture << '\n'
              << "Logical CPUs: " << hardware.logical_cpu_count << '\n'
              << "AVX2 CPU support: " << (hardware.avx2_cpu_supported ? "yes" : "no") << '\n'
              << "AVX2 kernel compiled: " << (hardware.avx2_kernel_compiled ? "yes" : "no") << '\n'
              << "AVX2 usable by HCS: " << (hardware.avx2_usable ? "yes" : "no") << '\n'
              << "SIMD Auto policy: scalar (AVX2 is opt-in; no consistent speedup was measured)\n"
              << "Vulkan compute compiled: " << (hardware.vulkan_compute_compiled ? "yes" : "no") << '\n'
              << "GPU device available to HCS: " << (hardware.gpu_device_available ? "yes" : "no") << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--hardware-info")
        return print_hardware_info();
    if (argc > 1) {
        std::cerr << "Usage: hcs_demo [--hardware-info]\n";
        return 2;
    }

    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto out = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto circuit = builder.build();

    hcs::Simulator simulator(circuit);
    for (int av = 0; av <= 1; ++av) {
        for (int bv = 0; bv <= 1; ++bv) {
            simulator.set_input(a, av != 0);
            simulator.set_input(b, bv != 0);
            const auto result = simulator.step();
            std::cout << av << " XOR " << bv << " = " << simulator.signal(out)
                      << " (settled=" << (result.converged ? "yes" : "no") << ")\n";
        }
    }
    return 0;
}
