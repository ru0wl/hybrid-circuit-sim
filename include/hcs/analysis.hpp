#pragma once

#include "hcs/ir.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hcs::analysis {

enum class RegionKind : std::uint8_t { AcyclicCombinational, FeedbackCombinational, SequentialBoundary };

struct Region {
    std::uint32_t id{};
    RegionKind kind{RegionKind::AcyclicCombinational};
    std::vector<ir::OperationId> operations;
    std::vector<ir::StateId> states;
};

struct DependencyGraph {
    std::vector<std::vector<ir::OperationId>> predecessors;
    std::vector<std::vector<ir::OperationId>> successors;
    std::vector<std::vector<ir::OperationId>> state_consumers;
    std::vector<std::vector<ir::OperationId>> state_dependencies;
    std::vector<bool> reachable_operation;
    std::vector<SignalId> unused_outputs;
    std::vector<std::vector<ir::OperationId>> shared_expressions;
    std::vector<std::uint32_t> component_of;
    std::vector<std::uint32_t> operation_region;
    std::vector<std::vector<ir::OperationId>> strongly_connected_components;
    std::vector<Region> regions;
    std::vector<bool> cyclic_operation;
    std::uint32_t critical_path_length{};
};

[[nodiscard]] DependencyGraph analyze(const ir::CircuitIR& circuit);
[[nodiscard]] std::string dump(const DependencyGraph& graph);

} // namespace hcs::analysis
