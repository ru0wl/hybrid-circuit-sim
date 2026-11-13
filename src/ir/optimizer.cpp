#include "hcs/optimizer.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <tuple>

namespace hcs::optimizer {
namespace {
constexpr std::int8_t Unknown = -1;

bool eval(LogicOp op, bool a, bool b) {
    switch (op) {
        case LogicOp::Constant0: return false;
        case LogicOp::Constant1: return true;
        case LogicOp::Buffer: return a;
        case LogicOp::Not: return !a;
        case LogicOp::And: return a && b;
        case LogicOp::Or: return a || b;
        case LogicOp::Xor: return a != b;
        case LogicOp::Nand: return !(a && b);
        case LogicOp::Nor: return !(a || b);
        case LogicOp::Xnor: return a == b;
    }
    return false;
}

void make_constant(ir::Operation& op, bool value) {
    op.opcode = value ? LogicOp::Constant1 : LogicOp::Constant0;
    op.a = kInvalidSignal;
    op.b = kInvalidSignal;
}

void make_buffer(ir::Operation& op, SignalId source) {
    op.opcode = LogicOp::Buffer;
    op.a = source;
    op.b = kInvalidSignal;
}

std::string op_name(LogicOp op) {
    switch (op) {
        case LogicOp::Constant0: return "CONST0";
        case LogicOp::Constant1: return "CONST1";
        case LogicOp::Buffer: return "BUFFER";
        case LogicOp::Not: return "NOT";
        case LogicOp::And: return "AND";
        case LogicOp::Or: return "OR";
        case LogicOp::Xor: return "XOR";
        case LogicOp::Nand: return "NAND";
        case LogicOp::Nor: return "NOR";
        case LogicOp::Xnor: return "XNOR";
    }
    return "UNKNOWN";
}
}

Options Options::defaults() {
    return Options{{
        {PassKind::BooleanSimplification, true},
        {PassKind::ConstantFolding, true},
        {PassKind::ConstantPropagation, true},
        {PassKind::CommonSubexpressionElimination, true},
        {PassKind::CopyPropagation, true},
        {PassKind::DeadLogicElimination, true},
        {PassKind::RedundantOperationElimination, true},
        {PassKind::OperationFusion, false}
    }};
}

const char* pass_name(PassKind kind) noexcept {
    switch (kind) {
        case PassKind::ConstantFolding: return "constant-folding";
        case PassKind::ConstantPropagation: return "constant-propagation";
        case PassKind::BooleanSimplification: return "boolean-simplification";
        case PassKind::CommonSubexpressionElimination: return "common-subexpression-elimination";
        case PassKind::CopyPropagation: return "copy-propagation";
        case PassKind::DeadLogicElimination: return "dead-logic-elimination";
        case PassKind::RedundantOperationElimination: return "redundant-operation-elimination";
        case PassKind::OperationFusion: return "operation-fusion";
    }
    return "unknown";
}

Result run(const ir::CircuitIR& input, const analysis::DependencyGraph& dependencies,
           const Options& options) {
    Result result;
    result.circuit = input;
    auto& ops = result.circuit.operations;
    auto current_dependencies = dependencies;
    for (const auto& config : options.passes) {
        PassResult pass{config.kind, 0, 0, {}};
        if (!config.enabled) { result.pass_results.push_back(pass); continue; }

        std::vector<std::int8_t> constants(result.circuit.signal_count, Unknown);
        std::vector<SignalId> aliases(result.circuit.signal_count);
        for (SignalId i = 0; i < aliases.size(); ++i) aliases[i] = i;
        auto resolve = [&](SignalId id) {
            while (id != kInvalidSignal && id < aliases.size() && aliases[id] != id) id = aliases[id];
            return id;
        };

        if (config.kind == PassKind::ConstantFolding ||
            config.kind == PassKind::ConstantPropagation ||
            config.kind == PassKind::BooleanSimplification ||
            config.kind == PassKind::RedundantOperationElimination) {
            for (std::size_t i = 0; i < ops.size(); ++i) {
                auto& op = ops[i];
                if (i < current_dependencies.cyclic_operation.size() && current_dependencies.cyclic_operation[i]) continue;
                if (op.a != kInvalidSignal) op.a = resolve(op.a);
                if (op.b != kInvalidSignal) op.b = resolve(op.b);
                const auto a = op.a == kInvalidSignal ? Unknown : constants[op.a];
                const auto b = op.b == kInvalidSignal ? Unknown : constants[op.b];
                const auto old = op.opcode;

                if (config.kind == PassKind::ConstantFolding || config.kind == PassKind::ConstantPropagation) {
                    if (op.opcode != LogicOp::Constant0 && op.opcode != LogicOp::Constant1) {
                        if (config.kind == PassKind::ConstantPropagation) {
                            result.constants_propagated += static_cast<std::uint64_t>(a != Unknown) +
                                                           static_cast<std::uint64_t>(b != Unknown);
                        }
                    }
                    if (op.opcode == LogicOp::Constant0 || op.opcode == LogicOp::Constant1) {
                        constants[op.output] = op.opcode == LogicOp::Constant1 ? 1 : 0;
                    } else if (a != Unknown && (op.opcode == LogicOp::Buffer || op.opcode == LogicOp::Not)) {
                        make_constant(op, eval(op.opcode, a != 0, false));
                        constants[op.output] = op.opcode == LogicOp::Constant1 ? 1 : 0;
                    } else if (a != Unknown && b != Unknown) {
                        make_constant(op, eval(op.opcode, a != 0, b != 0));
                        constants[op.output] = op.opcode == LogicOp::Constant1 ? 1 : 0;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::And && (a == 0 || b == 0))) {
                        make_constant(op, false); constants[op.output] = 0;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Or && (a == 1 || b == 1))) {
                        make_constant(op, true); constants[op.output] = 1;
                    } else if (config.kind == PassKind::ConstantPropagation && ((op.opcode == LogicOp::Xor || op.opcode == LogicOp::Xnor) &&
                               (a == 0 || b == 0))) {
                        const auto source = a == 0 ? op.b : op.a;
                        if (op.opcode == LogicOp::Xor) make_buffer(op, source);
                        else { op.opcode = LogicOp::Not; op.a = source; op.b = kInvalidSignal; }
                        if (constants[source] != Unknown) {
                            const bool value = constants[source] != 0;
                            constants[op.output] = static_cast<std::int8_t>(op.opcode == LogicOp::Not ? !value : value);
                        }
                    } else if (config.kind == PassKind::ConstantPropagation && ((op.opcode == LogicOp::Xor || op.opcode == LogicOp::Xnor) &&
                               (a == 1 || b == 1))) {
                        const auto source = a == 1 ? op.b : op.a;
                        if (op.opcode == LogicOp::Xor) { op.opcode = LogicOp::Not; op.a = source; op.b = kInvalidSignal; }
                        else make_buffer(op, source);
                        if (constants[source] != Unknown) {
                            const bool value = constants[source] != 0;
                            constants[op.output] = static_cast<std::int8_t>(op.opcode == LogicOp::Not ? !value : value);
                        }
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Nand && (a == 0 || b == 0))) {
                        make_constant(op, true); constants[op.output] = 1;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Nor && (a == 1 || b == 1))) {
                        make_constant(op, false); constants[op.output] = 0;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Nand && (a == 1 || b == 1))) {
                        const auto source = a == 1 ? op.b : op.a;
                        op.opcode = LogicOp::Not; op.a = source; op.b = kInvalidSignal;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Nor && (a == 0 || b == 0))) {
                        const auto source = a == 0 ? op.b : op.a;
                        op.opcode = LogicOp::Not; op.a = source; op.b = kInvalidSignal;
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::And && (a == 1 || b == 1))) {
                        const auto source = a == 1 ? op.b : op.a;
                        make_buffer(op, source);
                        if (constants[source] != Unknown) constants[op.output] = constants[source];
                    } else if (config.kind == PassKind::ConstantPropagation && (op.opcode == LogicOp::Or && (a == 0 || b == 0))) {
                        const auto source = a == 0 ? op.b : op.a;
                        make_buffer(op, source);
                        if (constants[source] != Unknown) constants[op.output] = constants[source];
                    }
                } else if (config.kind == PassKind::BooleanSimplification) {
                    if ((op.opcode == LogicOp::And || op.opcode == LogicOp::Or) && op.a == op.b) {
                        make_buffer(op, op.a);
                    } else if (op.opcode == LogicOp::Xor && op.a == op.b) {
                        make_constant(op, false);
                    } else if (op.opcode == LogicOp::Xnor && op.a == op.b) {
                        make_constant(op, true);
                    } else if ((op.opcode == LogicOp::Nand || op.opcode == LogicOp::Nor) && op.a == op.b) {
                        op.opcode = LogicOp::Not; op.b = kInvalidSignal;
                    }
                } else if (config.kind == PassKind::RedundantOperationElimination) {
                    if (op.opcode == LogicOp::Not && op.a != kInvalidSignal) {
                        const auto source_id = op.a;
                        const auto producer = std::find_if(ops.begin(), ops.begin() + static_cast<std::ptrdiff_t>(i),
                            [&](const ir::Operation& candidate) { return candidate.output == source_id; });
                        if (producer != ops.begin() + static_cast<std::ptrdiff_t>(i) &&
                            producer->opcode == LogicOp::Not) {
                            make_buffer(op, producer->a);
                        }
                    }
                }
                if (op.opcode != old) {
                    ++pass.changed_operations;
                    if (op.opcode == LogicOp::Constant0 || op.opcode == LogicOp::Constant1) {
                        ++result.constants_folded;
                    } else if (op.opcode == LogicOp::Buffer) {
                        ++result.redundant_operations_removed;
                    }
                }
            }
        } else if (config.kind == PassKind::CommonSubexpressionElimination) {
            using Key = std::tuple<unsigned, SignalId, SignalId>;
            std::map<Key, SignalId> expressions;
            for (std::size_t i = 0; i < ops.size(); ++i) {
                auto& op = ops[i];
                if (i < current_dependencies.cyclic_operation.size() && current_dependencies.cyclic_operation[i]) continue;
                if (op.a != kInvalidSignal) op.a = resolve(op.a);
                if (op.b != kInvalidSignal) op.b = resolve(op.b);
                if (op.opcode == LogicOp::Constant0 || op.opcode == LogicOp::Constant1) continue;
                auto a = op.a, b = op.b;
                if (op.opcode == LogicOp::And || op.opcode == LogicOp::Or || op.opcode == LogicOp::Xor ||
                    op.opcode == LogicOp::Nand || op.opcode == LogicOp::Nor || op.opcode == LogicOp::Xnor) {
                    if (b < a) std::swap(a, b);
                }
                const Key key{static_cast<unsigned>(op.opcode), a, b};
                const auto found = expressions.find(key);
                if (found == expressions.end()) expressions.emplace(key, op.output);
                else {
                    make_buffer(op, found->second);
                    aliases[op.output] = found->second;
                    ++pass.changed_operations;
                    ++result.common_expressions_eliminated;
                }
            }
        } else if (config.kind == PassKind::CopyPropagation) {
            for (std::size_t i = 0; i < ops.size(); ++i) {
                auto& op = ops[i];
                if (i < current_dependencies.cyclic_operation.size() && current_dependencies.cyclic_operation[i]) continue;
                if (op.a != kInvalidSignal) op.a = resolve(op.a);
                if (op.b != kInvalidSignal) op.b = resolve(op.b);
                if (op.opcode == LogicOp::Buffer && op.a != kInvalidSignal) aliases[op.output] = resolve(op.a);
            }
        } else if (config.kind == PassKind::DeadLogicElimination) {
            std::vector<ir::OperationId> driver(result.circuit.signal_count, UINT32_MAX);
            for (std::size_t i = 0; i < ops.size(); ++i) {
                if (ops[i].output < driver.size()) driver[ops[i].output] = static_cast<ir::OperationId>(i);
            }
            std::vector<bool> live(ops.size(), false);
            std::vector<SignalId> pending = result.circuit.observable_signals;
            for (const auto& state : result.circuit.states) {
                pending.push_back(state.data);
                pending.push_back(state.clock);
                pending.push_back(state.output);
            }
            while (!pending.empty()) {
                const auto signal = pending.back();
                pending.pop_back();
                if (signal == kInvalidSignal || signal >= driver.size()) continue;
                const auto operation = driver[signal];
                if (operation == UINT32_MAX || live[operation]) continue;
                live[operation] = true;
                pending.push_back(ops[operation].a);
                pending.push_back(ops[operation].b);
            }
            std::vector<ir::Operation> kept;
            kept.reserve(ops.size());
            for (std::size_t i = 0; i < ops.size(); ++i) {
                if (live[i]) kept.push_back(ops[i]);
                else ++pass.removed_operations;
            }
            if (pass.removed_operations != 0) {
                ops = std::move(kept);
                pass.changed_operations = pass.removed_operations;
                result.dead_operations_removed += pass.removed_operations;
                current_dependencies = analysis::analyze(result.circuit);
                for (std::size_t i = 0; i < ops.size(); ++i) {
                    ops[i].region = current_dependencies.operation_region[i];
                }
            }
        } else if (config.kind == PassKind::OperationFusion) {
            pass.skipped_reason = "Fusion is deferred until intermediate-signal observability can be represented without preserving every individual write.";
        }
        result.pass_results.push_back(pass);
    }
    return result;
}

std::string dump(const ir::CircuitIR& circuit) {
    std::ostringstream out;
    out << "IR signals=" << circuit.signal_count << " operations=" << circuit.operations.size()
        << " states=" << circuit.states.size() << '\n';
    for (std::size_t i = 0; i < circuit.operations.size(); ++i) {
        const auto& op = circuit.operations[i];
        out << '%' << i << " [component=" << op.source_component << "] " << op_name(op.opcode)
            << '(';
        if (op.a == kInvalidSignal) out << '-'; else out << '%' << op.a;
        if (op.opcode != LogicOp::Buffer && op.opcode != LogicOp::Not &&
            op.opcode != LogicOp::Constant0 && op.opcode != LogicOp::Constant1) {
            out << ", "; if (op.b == kInvalidSignal) out << '-'; else out << '%' << op.b;
        }
        out << ") -> %" << op.output << '\n';
    }
    for (const auto& state : circuit.states) {
        out << "state[" << state.id << "] D=%" << state.data << " CLK=%" << state.clock
            << " Q=%" << state.output << " initial=" << state.initial_value << '\n';
    }
    return out.str();
}

} // namespace hcs::optimizer
