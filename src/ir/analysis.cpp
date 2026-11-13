#include "hcs/analysis.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <queue>
#include <sstream>
#include <map>
#include <tuple>

namespace hcs::analysis {

DependencyGraph analyze(const ir::CircuitIR& circuit) {
    DependencyGraph result;
    const auto count = circuit.operations.size();
    result.predecessors.resize(count);
    result.successors.resize(count);
    result.component_of.resize(count, UINT32_MAX);
    result.operation_region.resize(count, UINT32_MAX);
    result.cyclic_operation.resize(count, false);
    result.reachable_operation.resize(count, false);
    result.state_consumers.resize(circuit.states.size());
    result.state_dependencies.resize(circuit.states.size());

    std::vector<ir::StateId> state_driver(circuit.signal_count, UINT32_MAX);
    for (const auto& state : circuit.states) {
        if (state.output < state_driver.size()) state_driver[state.output] = state.id;
    }
    std::vector<ir::OperationId> driver(circuit.signal_count, UINT32_MAX);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& op = circuit.operations[i];
        if (op.output < driver.size()) driver[op.output] = static_cast<ir::OperationId>(i);
    }
    std::vector<std::uint32_t> consumer_count(circuit.signal_count, 0);
    std::vector<bool> observable(circuit.signal_count, false);
    for (const auto signal : circuit.observable_signals) {
        if (signal < observable.size()) observable[signal] = true;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& op = circuit.operations[i];
        if (op.a != kInvalidSignal && op.a < consumer_count.size()) ++consumer_count[op.a];
        if (op.b != kInvalidSignal && op.b < consumer_count.size()) ++consumer_count[op.b];
    }
    for (const auto& state : circuit.states) {
        for (const auto signal : {state.data, state.clock, state.output}) {
            if (signal < observable.size()) observable[signal] = true;
            if (signal < consumer_count.size() && signal != state.output) ++consumer_count[signal];
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& op = circuit.operations[i];
        auto add_dependency = [&](SignalId signal) {
            if (signal == kInvalidSignal || signal >= driver.size()) return;
            if (state_driver[signal] != UINT32_MAX) {
                result.state_consumers[state_driver[signal]].push_back(static_cast<ir::OperationId>(i));
            }
            const auto source = driver[signal];
            if (source == UINT32_MAX || source == i) {
                if (source == i) result.predecessors[i].push_back(source);
                return;
            }
            result.predecessors[i].push_back(source);
        };
        add_dependency(op.a);
        add_dependency(op.b);
        auto& predecessors = result.predecessors[i];
        std::sort(predecessors.begin(), predecessors.end());
        predecessors.erase(std::unique(predecessors.begin(), predecessors.end()), predecessors.end());
        for (const auto source : predecessors) result.successors[source].push_back(static_cast<ir::OperationId>(i));
    }

    std::vector<int> index(count, -1), low(count, -1);
    std::vector<ir::OperationId> stack;
    std::vector<bool> on_stack(count, false);
    int next_index = 0;
    std::function<void(ir::OperationId)> visit = [&](ir::OperationId node) {
        index[node] = low[node] = next_index++;
        stack.push_back(node);
        on_stack[node] = true;
        for (const auto next : result.successors[node]) {
            if (index[next] == -1) {
                visit(next);
                low[node] = std::min(low[node], low[next]);
            } else if (on_stack[next]) {
                low[node] = std::min(low[node], index[next]);
            }
        }
        if (low[node] != index[node]) return;
        std::vector<ir::OperationId> component;
        for (;;) {
            const auto member = stack.back();
            stack.pop_back();
            on_stack[member] = false;
            result.component_of[member] = static_cast<std::uint32_t>(result.strongly_connected_components.size());
            component.push_back(member);
            if (member == node) break;
        }
        std::sort(component.begin(), component.end());
        result.strongly_connected_components.push_back(std::move(component));
    };
    for (std::size_t i = 0; i < count; ++i) {
        if (index[i] == -1) visit(static_cast<ir::OperationId>(i));
    }

    for (const auto& state : circuit.states) {
        for (const auto signal : {state.data, state.clock}) {
            if (signal == kInvalidSignal || signal >= driver.size()) continue;
            if (driver[signal] != UINT32_MAX) result.state_dependencies[state.id].push_back(driver[signal]);
        }
        auto& deps = result.state_dependencies[state.id];
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }

    std::vector<SignalId> live_signals;
    for (SignalId signal = 0; signal < observable.size(); ++signal) {
        if (observable[signal]) live_signals.push_back(signal);
    }
    while (!live_signals.empty()) {
        const auto signal = live_signals.back();
        live_signals.pop_back();
        if (signal >= driver.size()) continue;
        const auto operation = driver[signal];
        if (operation == UINT32_MAX || result.reachable_operation[operation]) continue;
        result.reachable_operation[operation] = true;
        live_signals.push_back(circuit.operations[operation].a);
        live_signals.push_back(circuit.operations[operation].b);
    }
    for (const auto& op : circuit.operations) {
        if (op.output < consumer_count.size() && consumer_count[op.output] == 0 && !observable[op.output]) {
            result.unused_outputs.push_back(op.output);
        }
    }

    using ExpressionKey = std::tuple<unsigned, SignalId, SignalId>;
    std::map<ExpressionKey, std::vector<ir::OperationId>> expression_groups;
    for (std::size_t i = 0; i < count; ++i) {
        if (result.cyclic_operation[i]) continue;
        const auto& op = circuit.operations[i];
        if (op.opcode == LogicOp::Constant0 || op.opcode == LogicOp::Constant1) continue;
        auto a = op.a, b = op.b;
        if (op.opcode == LogicOp::And || op.opcode == LogicOp::Or || op.opcode == LogicOp::Xor ||
            op.opcode == LogicOp::Nand || op.opcode == LogicOp::Nor || op.opcode == LogicOp::Xnor) {
            if (b < a) std::swap(a, b);
        }
        expression_groups[{static_cast<unsigned>(op.opcode), a, b}].push_back(static_cast<ir::OperationId>(i));
    }
    for (auto& [key, group] : expression_groups) {
        (void)key;
        if (group.size() > 1) result.shared_expressions.push_back(std::move(group));
    }

    for (const auto& members : result.strongly_connected_components) {
        const bool cyclic = members.size() > 1 ||
            (!members.empty() && std::find(result.predecessors[members.front()].begin(),
                                           result.predecessors[members.front()].end(),
                                           members.front()) != result.predecessors[members.front()].end());
        for (const auto op : members) result.cyclic_operation[op] = cyclic;
    }

    std::vector<bool> region_visited(count, false);
    for (std::size_t root = 0; root < count; ++root) {
        if (region_visited[root]) continue;
        Region region;
        region.id = static_cast<std::uint32_t>(result.regions.size());
        bool contains_feedback = false;
        std::vector<ir::OperationId> pending{static_cast<ir::OperationId>(root)};
        region_visited[root] = true;
        while (!pending.empty()) {
            const auto node = pending.back();
            pending.pop_back();
            region.operations.push_back(node);
            contains_feedback = contains_feedback || result.cyclic_operation[node];
            auto visit_neighbor = [&](ir::OperationId neighbor) {
                if (!region_visited[neighbor]) {
                    region_visited[neighbor] = true;
                    pending.push_back(neighbor);
                }
            };
            for (const auto neighbor : result.predecessors[node]) visit_neighbor(neighbor);
            for (const auto neighbor : result.successors[node]) visit_neighbor(neighbor);
        }
        std::sort(region.operations.begin(), region.operations.end());
        region.kind = contains_feedback ? RegionKind::FeedbackCombinational
                                        : RegionKind::AcyclicCombinational;
        for (const auto op : region.operations) result.operation_region[op] = region.id;
        result.regions.push_back(std::move(region));
    }
    for (const auto& state : circuit.states) {
        Region region;
        region.id = static_cast<std::uint32_t>(result.regions.size());
        region.kind = RegionKind::SequentialBoundary;
        region.states.push_back(state.id);
        result.regions.push_back(std::move(region));
    }

    std::vector<std::uint32_t> indegree(count, 0), depth(count, 1);
    std::queue<ir::OperationId> ready;
    for (std::size_t i = 0; i < count; ++i) {
        for (const auto pred : result.predecessors[i]) {
            if (result.component_of[pred] != result.component_of[i]) ++indegree[i];
        }
        if (indegree[i] == 0) ready.push(static_cast<ir::OperationId>(i));
    }
    std::uint32_t visited = 0;
    while (!ready.empty()) {
        const auto node = ready.front(); ready.pop(); ++visited;
        result.critical_path_length = std::max(result.critical_path_length, depth[node]);
        for (const auto next : result.successors[node]) {
            if (result.component_of[next] == result.component_of[node]) continue;
            depth[next] = std::max(depth[next], depth[node] + 1);
            if (indegree[next] > 0 && --indegree[next] == 0) ready.push(next);
        }
    }
    if (visited != count) result.critical_path_length = 0;
    return result;
}

std::string dump(const DependencyGraph& graph) {
    std::ostringstream out;
    out << "DependencyGraph operations=" << graph.successors.size()
        << " scc=" << graph.strongly_connected_components.size()
        << " regions=" << graph.regions.size()
        << " critical_path=" << graph.critical_path_length
        << " shared_expression_groups=" << graph.shared_expressions.size()
        << " unused_outputs=" << graph.unused_outputs.size() << '\n';
    for (std::size_t i = 0; i < graph.successors.size(); ++i) {
        out << "op " << i << " <- [";
        for (std::size_t j = 0; j < graph.predecessors[i].size(); ++j) {
            if (j) out << ',';
            out << graph.predecessors[i][j];
        }
        out << "] -> [";
        for (std::size_t j = 0; j < graph.successors[i].size(); ++j) {
            if (j) out << ',';
            out << graph.successors[i][j];
        }
        out << "] cyclic=" << (graph.cyclic_operation[i] ? "yes" : "no") << '\n';
    }
    for (const auto signal : graph.unused_outputs) out << "unused-output signal=" << signal << '\n';
    for (const auto& group : graph.shared_expressions) {
        out << "shared-expression operations=[";
        for (std::size_t i = 0; i < group.size(); ++i) {
            if (i) out << ',';
            out << group[i];
        }
        out << "]\n";
    }
    for (std::size_t state = 0; state < graph.state_consumers.size(); ++state) {
        out << "state " << state << " depends-on=[";
        for (std::size_t j = 0; j < graph.state_dependencies[state].size(); ++j) {
            if (j) out << ',';
            out << graph.state_dependencies[state][j];
        }
        out << "] consumed-by=[";
        for (std::size_t j = 0; j < graph.state_consumers[state].size(); ++j) {
            if (j) out << ',';
            out << graph.state_consumers[state][j];
        }
        out << "]\n";
    }
    for (const auto& region : graph.regions) {
        out << "region " << region.id << " kind=";
        switch (region.kind) {
            case RegionKind::AcyclicCombinational: out << "combinational"; break;
            case RegionKind::FeedbackCombinational: out << "feedback"; break;
            case RegionKind::SequentialBoundary: out << "sequential"; break;
        }
        out << " operations=" << region.operations.size() << " states=" << region.states.size() << '\n';
    }
    return out.str();
}

} // namespace hcs::analysis
