#pragma once

#include "hcs/circuit.hpp"

#include <cstdint>
#include <vector>

namespace hcs::ir {

using OperationId = std::uint32_t;
using StateId = std::uint32_t;

struct Operation {
    LogicOp opcode{};
    SignalId a{kInvalidSignal};
    SignalId b{kInvalidSignal};
    SignalId output{kInvalidSignal};
    GateId source_component{UINT32_MAX};
    std::uint32_t region{UINT32_MAX};
};

struct StateElement {
    StateId id{};
    SignalId data{kInvalidSignal};
    SignalId clock{kInvalidSignal};
    SignalId output{kInvalidSignal};
    bool initial_value{};
    GateId source_component{UINT32_MAX};
};

struct CircuitIR {
    std::uint32_t signal_count{};
    std::vector<SignalId> inputs;
    std::vector<SignalId> observable_signals;
    std::vector<Operation> operations;
    std::vector<StateElement> states;
};

[[nodiscard]] CircuitIR lower(const Circuit& circuit);

} // namespace hcs::ir
