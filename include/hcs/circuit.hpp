#pragma once

#include <cstdint>
#include <vector>

namespace hcs {

using SignalId = std::uint32_t;
using GateId = std::uint32_t;

constexpr SignalId kInvalidSignal = UINT32_MAX;

enum class LogicOp : std::uint8_t {
    Constant0,
    Constant1,
    Buffer,
    Not,
    And,
    Or,
    Xor,
    Nand,
    Nor,
    Xnor,
};

struct Gate {
    LogicOp op{};
    SignalId a{kInvalidSignal};
    SignalId b{kInvalidSignal};
    SignalId out{kInvalidSignal};
};

struct DFlipFlop {
    SignalId d{kInvalidSignal};
    SignalId clock{kInvalidSignal};
    SignalId q{kInvalidSignal};
    bool initial_state{false};
};

class Circuit final {
public:
    [[nodiscard]] std::uint32_t signal_count() const noexcept;
    [[nodiscard]] const std::vector<SignalId>& inputs() const noexcept;
    [[nodiscard]] const std::vector<Gate>& gates() const noexcept;
    [[nodiscard]] const std::vector<GateId>& execution_order() const noexcept;
    [[nodiscard]] const std::vector<DFlipFlop>& flip_flops() const noexcept;

private:
    friend class CircuitBuilder;
    std::uint32_t signal_count_{0};
    std::vector<SignalId> inputs_;
    std::vector<Gate> gates_;
    std::vector<GateId> execution_order_;
    std::vector<DFlipFlop> flip_flops_;
};

class CircuitBuilder final {
public:
    [[nodiscard]] SignalId add_signal();
    [[nodiscard]] SignalId add_input();
    [[nodiscard]] SignalId add_constant(bool value);

    // Allocate output signals explicitly when constructing feedback loops.
    void add_gate(LogicOp op, SignalId a, SignalId b, SignalId out);
    [[nodiscard]] SignalId add_unary(LogicOp op, SignalId input);
    [[nodiscard]] SignalId add_binary(LogicOp op, SignalId a, SignalId b);
    void add_d_flip_flop(SignalId d, SignalId clock, SignalId q,
                         bool initial_state = false);

    [[nodiscard]] Circuit build() const;

private:
    void validate_signal(SignalId id) const;
    void claim_output(SignalId id);

    Circuit circuit_;
    std::vector<bool> is_input_;
    std::vector<bool> has_driver_;
};

[[nodiscard]] bool evaluate_gate(const Gate& gate,
                                 const std::vector<std::uint8_t>& signals) noexcept;

} // namespace hcs
