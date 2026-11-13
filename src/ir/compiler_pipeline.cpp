#include "hcs/compiler_pipeline.hpp"

#include <algorithm>

namespace hcs {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
}

PreparedCircuit prepare_circuit(const Circuit& circuit, const optimizer::Options& options) {
    PreparedCircuit prepared;
    const auto ir_start = Clock::now();
    prepared.original_ir = ir::lower(circuit);
    const auto ir_end = Clock::now();
    const auto analysis_start = Clock::now();
    prepared.dependencies = analysis::analyze(prepared.original_ir);
    const auto analysis_end = Clock::now();
    const auto optimize_start = Clock::now();
    auto optimized = optimizer::run(prepared.original_ir, prepared.dependencies, options);
    const auto optimize_end = Clock::now();

    prepared.optimized_ir = std::move(optimized.circuit);
    for (std::size_t i = 0; i < prepared.optimized_ir.operations.size(); ++i) {
        prepared.optimized_ir.operations[i].region = prepared.dependencies.operation_region[i];
    }
    prepared.passes = std::move(optimized.pass_results);
    auto& stats = prepared.statistics;
    stats.original_components = circuit.gates().size() + circuit.flip_flops().size();
    stats.original_operations = circuit.gates().size();
    stats.initial_ir_operations = prepared.original_ir.operations.size();
    stats.final_ir_operations = prepared.optimized_ir.operations.size();
    stats.constants_folded = optimized.constants_folded;
    stats.constants_propagated = optimized.constants_propagated;
    stats.dead_operations_removed = optimized.dead_operations_removed;
    stats.common_expressions_eliminated = optimized.common_expressions_eliminated;
    stats.redundant_operations_removed = optimized.redundant_operations_removed;
    stats.fused_operations = optimized.fused_operations;
    stats.regions = prepared.dependencies.regions.size();
    stats.signals = prepared.original_ir.signal_count;
    stats.observable_signals = prepared.original_ir.observable_signals.size();
    stats.sequential_operations = prepared.original_ir.states.size();
    stats.strongly_connected_components = prepared.dependencies.strongly_connected_components.size();
    std::vector<std::uint8_t> observable_mask(prepared.original_ir.signal_count, 0);
    std::vector<LogicOp> original_opcode_by_output(prepared.original_ir.signal_count, LogicOp::Buffer);
    std::vector<std::uint8_t> has_original_driver(prepared.original_ir.signal_count, 0);
    for (const auto signal : prepared.original_ir.observable_signals) {
        if (signal < observable_mask.size()) observable_mask[signal] = 1;
    }
    for (std::size_t i = 0; i < prepared.original_ir.operations.size(); ++i) {
        const auto opcode = static_cast<std::size_t>(prepared.original_ir.operations[i].opcode);
        if (opcode < stats.opcode_counts.size()) ++stats.opcode_counts[opcode];
        const auto output_signal = prepared.original_ir.operations[i].output;
        if (output_signal < observable_mask.size() && observable_mask[output_signal]) {
            ++stats.observable_operation_outputs;
        }
        if (output_signal < original_opcode_by_output.size()) {
            original_opcode_by_output[output_signal] = prepared.original_ir.operations[i].opcode;
            has_original_driver[output_signal] = 1;
        }
        if (i < prepared.dependencies.successors.size()) {
            stats.dependency_edges += prepared.dependencies.successors[i].size();
            stats.maximum_fanout = std::max<std::uint64_t>(stats.maximum_fanout,
                                                            prepared.dependencies.successors[i].size());
        }
    }
    for (const auto& operation : prepared.optimized_ir.operations) {
        const auto output_signal = operation.output;
        if (operation.opcode == LogicOp::Buffer && output_signal < has_original_driver.size() &&
            has_original_driver[output_signal] && original_opcode_by_output[output_signal] != LogicOp::Buffer) {
            ++stats.forwarding_operations_introduced;
        }
    }
    for (const auto& component : prepared.dependencies.strongly_connected_components) {
        const bool cyclic = std::any_of(component.begin(), component.end(), [&](ir::OperationId op) {
            return op < prepared.dependencies.cyclic_operation.size() && prepared.dependencies.cyclic_operation[op];
        });
        if (cyclic) ++stats.cyclic_components;
    }
    stats.critical_path_length = prepared.dependencies.critical_path_length;
    stats.ir_generation_ms = elapsed_ms(ir_start, ir_end);
    stats.dependency_analysis_ms = elapsed_ms(analysis_start, analysis_end);
    stats.optimization_ms = elapsed_ms(optimize_start, optimize_end);
    return prepared;
}

} // namespace hcs
