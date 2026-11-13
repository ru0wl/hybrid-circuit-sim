#include "hcs/circuit.hpp"

#include <algorithm>
#include <functional>
#include <queue>
#include <stdexcept>

namespace hcs {

std::uint32_t Circuit::signal_count() const noexcept { return signal_count_; }
const std::vector<SignalId>& Circuit::inputs() const noexcept { return inputs_; }
const std::vector<Gate>& Circuit::gates() const noexcept { return gates_; }
const std::vector<GateId>& Circuit::execution_order() const noexcept { return execution_order_; }
const std::vector<DFlipFlop>& Circuit::flip_flops() const noexcept { return flip_flops_; }

SignalId CircuitBuilder::add_signal() {
    if (circuit_.signal_count_ == UINT32_MAX) {
        throw std::overflow_error("Signal ID space exhausted");
    }
    const auto id = circuit_.signal_count_++;
    is_input_.push_back(false);
    has_driver_.push_back(false);
    return id;
}

SignalId CircuitBuilder::add_input() {
    const auto id = add_signal();
    is_input_[id] = true;
    circuit_.inputs_.push_back(id);
    return id;
}

SignalId CircuitBuilder::add_constant(bool value) {
    const auto output = add_signal();
    add_gate(value ? LogicOp::Constant1 : LogicOp::Constant0,
             kInvalidSignal, kInvalidSignal, output);
    return output;
}

void CircuitBuilder::validate_signal(SignalId id) const {
    if (id >= circuit_.signal_count_) {
        throw std::out_of_range("Signal ID is outside the circuit");
    }
}

void CircuitBuilder::claim_output(SignalId id) {
    validate_signal(id);
    if (is_input_[id]) {
        throw std::invalid_argument("An input signal cannot also have a driver");
    }
    if (has_driver_[id]) {
        throw std::invalid_argument("A signal cannot have multiple drivers");
    }
    has_driver_[id] = true;
}

void CircuitBuilder::add_gate(LogicOp op, SignalId a, SignalId b, SignalId out) {
    const bool no_inputs = op == LogicOp::Constant0 || op == LogicOp::Constant1;
    const bool unary = op == LogicOp::Buffer || op == LogicOp::Not;
    if (!no_inputs) validate_signal(a);
    if (!no_inputs && !unary) validate_signal(b);
    claim_output(out);
    circuit_.gates_.push_back(Gate{op, a, b, out});
}

SignalId CircuitBuilder::add_unary(LogicOp op, SignalId input) {
    const auto output = add_signal();
    add_gate(op, input, kInvalidSignal, output);
    return output;
}

SignalId CircuitBuilder::add_binary(LogicOp op, SignalId a, SignalId b) {
    const auto output = add_signal();
    add_gate(op, a, b, output);
    return output;
}

void CircuitBuilder::add_d_flip_flop(SignalId d, SignalId clock, SignalId q,
                                     bool initial_state) {
    validate_signal(d);
    validate_signal(clock);
    claim_output(q);
    circuit_.flip_flops_.push_back(DFlipFlop{d, clock, q, initial_state});
}

Circuit CircuitBuilder::build() const {
    Circuit result = circuit_;
    const auto gate_count = result.gates_.size();
    std::vector<GateId> driver(result.signal_count_, UINT32_MAX);
    for (std::size_t i = 0; i < gate_count; ++i) {
        driver[result.gates_[i].out] = static_cast<GateId>(i);
    }

    std::vector<std::vector<GateId>> dependents(gate_count);
    std::vector<std::uint32_t> indegree(gate_count, 0);
    for (std::size_t i = 0; i < gate_count; ++i) {
        const auto& gate = result.gates_[i];
        std::vector<GateId> dependencies;
        if (gate.a != kInvalidSignal && driver[gate.a] != UINT32_MAX) {
            dependencies.push_back(driver[gate.a]);
        }
        if (gate.b != kInvalidSignal && driver[gate.b] != UINT32_MAX) {
            dependencies.push_back(driver[gate.b]);
        }
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
        indegree[i] = static_cast<std::uint32_t>(dependencies.size());
        for (const auto dependency : dependencies) dependents[dependency].push_back(static_cast<GateId>(i));
    }

    std::priority_queue<GateId, std::vector<GateId>, std::greater<>> ready;
    for (std::size_t i = 0; i < gate_count; ++i) {
        if (indegree[i] == 0) ready.push(static_cast<GateId>(i));
    }
    std::vector<bool> emitted(gate_count, false);
    result.execution_order_.clear();
    result.execution_order_.reserve(gate_count);
    while (!ready.empty()) {
        const auto current = ready.top();
        ready.pop();
        emitted[current] = true;
        result.execution_order_.push_back(current);
        for (const auto dependent : dependents[current]) {
            if (--indegree[dependent] == 0) ready.push(dependent);
        }
    }

    // Cyclic components are appended in stable component-ID order and settled iteratively.
    for (std::size_t i = 0; i < gate_count; ++i) {
        if (!emitted[i]) result.execution_order_.push_back(static_cast<GateId>(i));
    }
    return result;
}

bool evaluate_gate(const Gate& gate, const std::vector<std::uint8_t>& s) noexcept {
    const auto a = gate.a == kInvalidSignal ? std::uint8_t{0} : s[gate.a];
    const auto b = gate.b == kInvalidSignal ? std::uint8_t{0} : s[gate.b];
    switch (gate.op) {
        case LogicOp::Constant0: return false;
        case LogicOp::Constant1: return true;
        case LogicOp::Buffer: return a != 0;
        case LogicOp::Not: return a == 0;
        case LogicOp::And: return a != 0 && b != 0;
        case LogicOp::Or: return a != 0 || b != 0;
        case LogicOp::Xor: return (a != 0) != (b != 0);
        case LogicOp::Nand: return !(a != 0 && b != 0);
        case LogicOp::Nor: return !(a != 0 || b != 0);
        case LogicOp::Xnor: return (a != 0) == (b != 0);
    }
    return false;
}

} // namespace hcs
