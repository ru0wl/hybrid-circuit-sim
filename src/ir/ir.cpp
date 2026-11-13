#include "hcs/ir.hpp"

#include <limits>

namespace hcs::ir {

CircuitIR lower(const Circuit& circuit) {
    CircuitIR result;
    result.signal_count = circuit.signal_count();
    result.inputs = circuit.inputs();
    result.observable_signals.reserve(result.signal_count);
    for (SignalId signal = 0; signal < result.signal_count; ++signal) {
        result.observable_signals.push_back(signal);
    }
    result.operations.reserve(circuit.gates().size());
    for (const auto gate_id : circuit.execution_order()) {
        const auto& gate = circuit.gates()[gate_id];
        result.operations.push_back(Operation{gate.op, gate.a, gate.b, gate.out, gate_id});
    }
    result.states.reserve(circuit.flip_flops().size());
    for (std::size_t i = 0; i < circuit.flip_flops().size(); ++i) {
        const auto& ff = circuit.flip_flops()[i];
        result.states.push_back(StateElement{static_cast<StateId>(i), ff.d, ff.clock,
                                             ff.q, ff.initial_state,
                                             static_cast<GateId>(0x80000000u | static_cast<std::uint32_t>(i))});
    }
    return result;
}

} // namespace hcs::ir
