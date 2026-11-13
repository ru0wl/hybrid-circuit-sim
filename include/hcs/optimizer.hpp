#pragma once

#include "hcs/analysis.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hcs::optimizer {

enum class PassKind : std::uint8_t {
    ConstantFolding,
    ConstantPropagation,
    BooleanSimplification,
    CommonSubexpressionElimination,
    CopyPropagation,
    DeadLogicElimination,
    RedundantOperationElimination,
    OperationFusion
};

struct PassConfig {
    PassKind kind{};
    bool enabled{true};
};

struct PassResult {
    PassKind kind{};
    std::uint64_t changed_operations{};
    std::uint64_t removed_operations{};
    std::string skipped_reason;
};

struct Options {
    std::vector<PassConfig> passes;
    [[nodiscard]] static Options defaults();
};

struct Result {
    ir::CircuitIR circuit;
    std::vector<PassResult> pass_results;
    std::uint64_t constants_folded{};
    std::uint64_t constants_propagated{};
    std::uint64_t common_expressions_eliminated{};
    std::uint64_t redundant_operations_removed{};
    std::uint64_t dead_operations_removed{};
    std::uint64_t fused_operations{};
};

[[nodiscard]] Result run(const ir::CircuitIR& input,
                         const analysis::DependencyGraph& dependencies,
                         const Options& options = Options::defaults());
[[nodiscard]] const char* pass_name(PassKind kind) noexcept;
[[nodiscard]] std::string dump(const ir::CircuitIR& circuit);

} // namespace hcs::optimizer
