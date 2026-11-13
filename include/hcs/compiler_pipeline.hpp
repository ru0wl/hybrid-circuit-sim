#pragma once

#include "hcs/optimizer.hpp"

#include <chrono>
#include <array>
#include <cstdint>

namespace hcs {

struct CompilerStatistics {
    std::uint64_t original_components{};
    std::uint64_t original_operations{};
    std::uint64_t initial_ir_operations{};
    std::uint64_t final_ir_operations{};
    std::uint64_t constants_folded{};
    std::uint64_t constants_propagated{};
    std::uint64_t dead_operations_removed{};
    std::uint64_t common_expressions_eliminated{};
    std::uint64_t redundant_operations_removed{};
    std::uint64_t fused_operations{};
    std::uint64_t regions{};
    std::uint64_t signals{};
    std::uint64_t observable_signals{};
    std::uint64_t observable_operation_outputs{};
    std::uint64_t forwarding_operations_introduced{};
    std::uint64_t dependency_edges{};
    std::uint64_t strongly_connected_components{};
    std::uint64_t cyclic_components{};
    std::uint64_t sequential_operations{};
    std::uint64_t maximum_fanout{};
    std::array<std::uint64_t, 10> opcode_counts{};
    std::uint32_t critical_path_length{};
    double ir_generation_ms{};
    double dependency_analysis_ms{};
    double optimization_ms{};
};

struct PreparedCircuit {
    ir::CircuitIR original_ir;
    ir::CircuitIR optimized_ir;
    analysis::DependencyGraph dependencies;
    std::vector<optimizer::PassResult> passes;
    CompilerStatistics statistics;
};

[[nodiscard]] PreparedCircuit prepare_circuit(
    const Circuit& circuit,
    const optimizer::Options& options = optimizer::Options::defaults());

} // namespace hcs
