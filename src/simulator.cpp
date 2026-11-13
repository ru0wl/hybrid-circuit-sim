#include "hcs/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace hcs {
namespace {
using ProfileClock = std::chrono::steady_clock;
std::uint64_t elapsed_ns(ProfileClock::time_point begin, ProfileClock::time_point end) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}
inline bool evaluate_ir_operation(const ir::Operation& op, const std::vector<std::uint8_t>& signals) noexcept {
    const auto a = op.a == kInvalidSignal ? std::uint8_t{0} : signals[op.a];
    const auto b = op.b == kInvalidSignal ? std::uint8_t{0} : signals[op.b];
    switch (op.opcode) {
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
}

Simulator::Simulator(const Circuit& circuit, SimulatorConfig config)
    : circuit_(circuit), config_(std::move(config)),
      signals_(circuit.signal_count(), 0), next_states_(circuit.flip_flops().size(), 0), fanout_(circuit.signal_count()),
      is_input_(circuit.signal_count(), false), queued_(circuit.gates().size(), false),
      previous_clock_(circuit.flip_flops().size(), false) {
    for (const auto input : circuit.inputs()) is_input_[input] = true;

    for (std::size_t gate_index = 0; gate_index < circuit.gates().size(); ++gate_index) {
        const auto& gate = circuit.gates()[gate_index];
        if (gate.a != kInvalidSignal) fanout_[gate.a].push_back(static_cast<GateId>(gate_index));
        if (gate.b != kInvalidSignal) fanout_[gate.b].push_back(static_cast<GateId>(gate_index));
    }

    for (std::size_t i = 0; i < circuit.flip_flops().size(); ++i) {
        const auto& ff = circuit.flip_flops()[i];
        signals_[ff.q] = static_cast<std::uint8_t>(ff.initial_state);
        previous_clock_[i] = signals_[ff.clock] != 0;
    }

    if (config_.backend == Backend::Compiled) {
        config_.native_options.max_dense_passes = config_.max_dense_passes;
        native_module_ = NativeCompiler::load_or_compile(circuit_, config_.native_options);
        compiler_statistics_ = native_module_->compiler_statistics();
        has_compiler_statistics_ = true;
    } else if (config_.backend == Backend::OptimizedIR) {
        auto prepared = prepare_circuit(circuit_, config_.optimization_options);
        compiler_statistics_ = prepared.statistics;
        has_compiler_statistics_ = true;
        optimized_ir_ = std::move(prepared.optimized_ir);
        ir_dependencies_ = std::move(prepared.dependencies);
        ir_fanout_.resize(optimized_ir_.signal_count);
        ir_queued_.assign(optimized_ir_.operations.size(), 0);
        for (std::size_t i = 0; i < optimized_ir_.operations.size(); ++i) {
            const auto& op = optimized_ir_.operations[i];
            if (op.a != kInvalidSignal) ir_fanout_[op.a].push_back(static_cast<ir::OperationId>(i));
            if (op.b != kInvalidSignal && op.b != op.a) ir_fanout_[op.b].push_back(static_cast<ir::OperationId>(i));
        }
        if (config_.strategy == InterpreterStrategy::EventDriven) {
            for (std::size_t i = 0; i < optimized_ir_.operations.size(); ++i)
                schedule_ir_operation(static_cast<ir::OperationId>(i));
        }
    } else if (config_.strategy == InterpreterStrategy::EventDriven) {
        for (std::size_t i = 0; i < circuit.gates().size(); ++i) {
            schedule_gate(static_cast<GateId>(i));
        }
    }
}

void Simulator::schedule_gate(GateId id) {
    if (!config_.enable_profiling) {
        if (id >= queued_.size() || queued_[id]) return;
        queued_[id] = true;
        event_queue_.push_back(id);
        return;
    }
    const auto scheduling_start = ProfileClock::now();
    ++profile_.schedule_attempts;
    ++profile_.dependent_schedule_attempts;
    if (id >= queued_.size() || queued_[id]) {
        if (id < queued_.size()) ++profile_.duplicate_schedule_attempts;
        profile_.scheduling_time_ns += elapsed_ns(scheduling_start, ProfileClock::now());
        return;
    }
    queued_[id] = true;
    profile_.scheduling_time_ns += elapsed_ns(scheduling_start, ProfileClock::now());
    const auto enqueue_start = ProfileClock::now();
    event_queue_.push_back(id);
    profile_.queue_enqueue_time_ns += elapsed_ns(enqueue_start, ProfileClock::now());
    ++profile_.queue_enqueue_count;
    ++profile_.events_enqueued;
    ++profile_.scheduled_operations;
    profile_.maximum_queue_length = std::max<std::uint64_t>(profile_.maximum_queue_length, event_queue_.size() - queue_head_);
    profile_.queue_max_length = profile_.maximum_queue_length;
}

void Simulator::schedule_fanout(SignalId signal_id) {
    if (!config_.enable_profiling) {
        for (const auto gate : fanout_[signal_id]) schedule_gate(gate);
        return;
    }
    const auto start = ProfileClock::now();
    const auto scheduling_before = profile_.scheduling_time_ns;
    const auto enqueue_before = profile_.queue_enqueue_time_ns;
    for (const auto gate : fanout_[signal_id]) {
        ++profile_.fanout_edges_visited;
        ++profile_.dependent_operations_found;
        schedule_gate(gate);
    }
    const auto inclusive = elapsed_ns(start, ProfileClock::now());
    const auto nested = (profile_.scheduling_time_ns - scheduling_before) +
                        (profile_.queue_enqueue_time_ns - enqueue_before);
    profile_.fanout_traversal_time_ns += inclusive > nested ? inclusive - nested : 0;
}

void Simulator::schedule_ir_operation(ir::OperationId id) {
    if (!config_.enable_profiling) {
        if (id >= ir_queued_.size() || ir_queued_[id]) return;
        ir_queued_[id] = 1;
        ir_event_queue_.push_back(id);
        return;
    }
    const auto scheduling_start = ProfileClock::now();
    ++profile_.schedule_attempts;
    ++profile_.dependent_schedule_attempts;
    if (id >= ir_queued_.size() || ir_queued_[id]) {
        if (id < ir_queued_.size()) ++profile_.duplicate_schedule_attempts;
        profile_.scheduling_time_ns += elapsed_ns(scheduling_start, ProfileClock::now());
        return;
    }
    ir_queued_[id] = 1;
    profile_.scheduling_time_ns += elapsed_ns(scheduling_start, ProfileClock::now());
    const auto enqueue_start = ProfileClock::now();
    ir_event_queue_.push_back(id);
    profile_.queue_enqueue_time_ns += elapsed_ns(enqueue_start, ProfileClock::now());
    ++profile_.queue_enqueue_count;
    ++profile_.events_enqueued;
    ++profile_.scheduled_operations;
    profile_.maximum_queue_length = std::max<std::uint64_t>(profile_.maximum_queue_length, ir_event_queue_.size() - ir_queue_head_);
    profile_.queue_max_length = profile_.maximum_queue_length;
}

void Simulator::schedule_ir_fanout(SignalId signal_id) {
    if (signal_id >= ir_fanout_.size()) return;
    if (!config_.enable_profiling) {
        for (const auto op : ir_fanout_[signal_id]) schedule_ir_operation(op);
        return;
    }
    const auto start = ProfileClock::now();
    const auto scheduling_before = profile_.scheduling_time_ns;
    const auto enqueue_before = profile_.queue_enqueue_time_ns;
    for (const auto op : ir_fanout_[signal_id]) {
        ++profile_.fanout_edges_visited;
        ++profile_.dependent_operations_found;
        schedule_ir_operation(op);
    }
    const auto inclusive = elapsed_ns(start, ProfileClock::now());
    const auto nested = (profile_.scheduling_time_ns - scheduling_before) +
                        (profile_.queue_enqueue_time_ns - enqueue_before);
    profile_.fanout_traversal_time_ns += inclusive > nested ? inclusive - nested : 0;
}

void Simulator::set_input(SignalId input, bool value) {
    if (input >= signals_.size() || !is_input_[input]) {
        throw std::invalid_argument("set_input requires a valid primary input signal");
    }
    const auto next = static_cast<std::uint8_t>(value);
    if (signals_[input] == next) return;
    const auto update_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
    signals_[input] = next;
    if (config_.enable_profiling) {
        ++profile_.signal_write_count; ++profile_.signal_change_count;
        ++profile_.signal_writes; ++profile_.signal_changes;
        profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
    }
    if (config_.backend == Backend::Interpreted &&
        config_.strategy == InterpreterStrategy::EventDriven) {
        schedule_fanout(input);
    } else if (config_.backend == Backend::OptimizedIR &&
               config_.strategy == InterpreterStrategy::EventDriven) {
        schedule_ir_fanout(input);
    }
}

bool Simulator::signal(SignalId id) const {
    if (id >= signals_.size()) throw std::out_of_range("Signal ID is outside the simulator");
    return signals_[id] != 0;
}

StepResult Simulator::settle_dense() {
    StepResult result;
    if (config_.enable_profiling) { ++profile_.settle_calls; ++profile_.settle_call_count; }
    for (std::uint32_t pass = 0; pass < config_.max_dense_passes; ++pass) {
        bool changed = false;
        ++result.dense_passes;
        if (config_.enable_profiling) { ++profile_.settle_iteration_count; profile_.maximum_settle_iterations = std::max<std::uint64_t>(profile_.maximum_settle_iterations, result.dense_passes); }
        for (const auto gate_id : circuit_.execution_order()) {
            const auto& gate = circuit_.gates()[gate_id];
            ++result.operations;
            if (config_.enable_profiling) ++profile_.dense_operations;
            const auto eval_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
            const auto next = static_cast<std::uint8_t>(evaluate_gate(gate, signals_));
            if (config_.enable_profiling) {
                const auto eval_ns = elapsed_ns(eval_start, ProfileClock::now());
                profile_.operation_evaluation_time_ns += eval_ns;
                profile_.evaluation_time_ns += eval_ns;
                ++profile_.operation_evaluation_count;
                const auto update_start = ProfileClock::now();
                ++profile_.signal_write_count; ++profile_.signal_writes;
                if (signals_[gate.out] != next) { signals_[gate.out] = next; ++profile_.signal_change_count; ++profile_.signal_changes; changed = true; }
                else { ++profile_.unchanged_signal_write_count; ++profile_.unchanged_writes; }
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
            } else if (signals_[gate.out] != next) { signals_[gate.out] = next; changed = true; }
        }
        if (!changed) return result;
    }
    result.converged = false;
    if (config_.enable_profiling) ++profile_.oscillation_limit_hits;
    return result;
}

StepResult Simulator::settle_events() {
    StepResult result;
    if (config_.enable_profiling) { ++profile_.settle_calls; ++profile_.settle_call_count; }
    while (queue_head_ < event_queue_.size() && result.events < config_.max_events_per_settle) {
        const auto queue_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
        const auto id = event_queue_[queue_head_++];
        queued_[id] = false;
        if (config_.enable_profiling) {
            const auto dequeue_ns = elapsed_ns(queue_start, ProfileClock::now());
            profile_.queue_dequeue_time_ns += dequeue_ns;
            profile_.queue_time_ns += dequeue_ns;
            ++profile_.queue_dequeue_count;
        }
        const auto& gate = circuit_.gates()[id];
        ++result.events;
        if (config_.enable_profiling) { ++profile_.settle_iteration_count; profile_.maximum_settle_iterations = std::max(profile_.maximum_settle_iterations, result.events); }
        ++result.operations;
        if (config_.enable_profiling) { ++profile_.events_processed; ++profile_.event_operations; }
        const auto eval_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
        const auto next = static_cast<std::uint8_t>(evaluate_gate(gate, signals_));
        if (config_.enable_profiling) {
            const auto eval_ns = elapsed_ns(eval_start, ProfileClock::now());
            profile_.operation_evaluation_time_ns += eval_ns;
            profile_.evaluation_time_ns += eval_ns;
            ++profile_.operation_evaluation_count;
            const auto update_start = ProfileClock::now();
            ++profile_.signal_write_count; ++profile_.signal_writes;
            if (signals_[gate.out] != next) {
                signals_[gate.out] = next;
                ++profile_.signal_change_count; ++profile_.signal_changes;
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
                schedule_fanout(gate.out);
            } else {
                ++profile_.unchanged_signal_write_count; ++profile_.unchanged_writes;
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
            }
        } else if (signals_[gate.out] != next) {
            signals_[gate.out] = next;
            schedule_fanout(gate.out);
        }
    }
    if (queue_head_ == event_queue_.size()) {
        event_queue_.clear();
        queue_head_ = 0;
    } else {
        result.converged = false;
        if (config_.enable_profiling) ++profile_.oscillation_limit_hits;
    }
    return result;
}

StepResult Simulator::settle_compiled() {
    const auto native = native_module_->evaluate(signals_);
    return StepResult{native.converged != 0, native.operations, 0, native.passes};
}

StepResult Simulator::settle_ir() {
    StepResult result;
    if (!config_.enable_profiling) {
        for (std::uint32_t pass = 0; pass < config_.max_dense_passes; ++pass) {
            bool changed = false;
            ++result.dense_passes;
            for (const auto& op : optimized_ir_.operations) {
                ++result.operations;
                const auto next = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
                if (signals_[op.output] != next) {
                    signals_[op.output] = next;
                    changed = true;
                }
            }
            if (!changed) return result;
        }
        result.converged = false;
        return result;
    }

    ++profile_.settle_calls;
    ++profile_.settle_call_count;
    for (std::uint32_t pass = 0; pass < config_.max_dense_passes; ++pass) {
        bool changed = false;
        ++result.dense_passes;
        ++profile_.settle_iteration_count;
        profile_.maximum_settle_iterations = std::max<std::uint64_t>(profile_.maximum_settle_iterations, result.dense_passes);
        for (const auto& op : optimized_ir_.operations) {
            ++result.operations;
            ++profile_.dense_operations;
            const auto eval_start = ProfileClock::now();
            const auto next = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
            profile_.operation_evaluation_time_ns += elapsed_ns(eval_start, ProfileClock::now());
            profile_.evaluation_time_ns = profile_.operation_evaluation_time_ns;
            ++profile_.operation_evaluation_count;
            const auto update_start = ProfileClock::now();
            ++profile_.signal_write_count;
            ++profile_.signal_writes;
            if (signals_[op.output] != next) {
                signals_[op.output] = next;
                ++profile_.signal_change_count;
                ++profile_.signal_changes;
                changed = true;
            } else {
                ++profile_.unchanged_signal_write_count;
                ++profile_.unchanged_writes;
            }
            profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
        }
        if (!changed) return result;
    }
    result.converged = false;
    ++profile_.oscillation_limit_hits;
    return result;
}

StepResult Simulator::settle_ir_events() {
    StepResult result;
    if (config_.enable_profiling) { ++profile_.settle_calls; ++profile_.settle_call_count; }
    while (ir_queue_head_ < ir_event_queue_.size() && result.events < config_.max_events_per_settle) {
        const auto queue_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
        const auto id = ir_event_queue_[ir_queue_head_++];
        ir_queued_[id] = 0;
        if (config_.enable_profiling) {
            const auto dequeue_ns = elapsed_ns(queue_start, ProfileClock::now());
            profile_.queue_dequeue_time_ns += dequeue_ns;
            profile_.queue_time_ns += dequeue_ns;
            ++profile_.queue_dequeue_count;
        }
        const auto& op = optimized_ir_.operations[id];
        ++result.events;
        if (config_.enable_profiling) { ++profile_.settle_iteration_count; profile_.maximum_settle_iterations = std::max(profile_.maximum_settle_iterations, result.events); }
        ++result.operations;
        if (config_.enable_profiling) { ++profile_.events_processed; ++profile_.event_operations; }
        const auto eval_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
        const auto next = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
        if (config_.enable_profiling) {
            const auto eval_ns = elapsed_ns(eval_start, ProfileClock::now());
            profile_.operation_evaluation_time_ns += eval_ns;
            profile_.evaluation_time_ns += eval_ns;
            ++profile_.operation_evaluation_count;
            const auto update_start = ProfileClock::now();
            ++profile_.signal_write_count; ++profile_.signal_writes;
            if (signals_[op.output] != next) {
                signals_[op.output] = next;
                ++profile_.signal_change_count; ++profile_.signal_changes;
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
                schedule_ir_fanout(op.output);
            } else {
                ++profile_.unchanged_signal_write_count; ++profile_.unchanged_writes;
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
            }
        } else if (signals_[op.output] != next) {
            signals_[op.output] = next;
            schedule_ir_fanout(op.output);
        }
    }
    if (ir_queue_head_ == ir_event_queue_.size()) {
        ir_event_queue_.clear();
        ir_queue_head_ = 0;
    } else {
        result.converged = false;
        if (config_.enable_profiling) ++profile_.oscillation_limit_hits;
    }
    return result;
}

StepResult Simulator::settle() {
    if (config_.backend == Backend::Compiled) return settle_compiled();
    if (config_.backend == Backend::OptimizedIR) return config_.strategy == InterpreterStrategy::EventDriven ? settle_ir_events() : settle_ir();
    if (config_.strategy == InterpreterStrategy::Dense) return settle_dense();
    return settle_events();
}

StepResult Simulator::step() {
    StepResult total;
    const auto before_clock = settle();
    total.converged = before_clock.converged;
    total.operations += before_clock.operations;
    total.events += before_clock.events;
    total.dense_passes += before_clock.dense_passes;

    if (next_states_.size() != circuit_.flip_flops().size())
        next_states_.resize(circuit_.flip_flops().size());
    for (std::size_t i = 0; i < circuit_.flip_flops().size(); ++i) {
        const auto& ff = circuit_.flip_flops()[i];
        const bool clock_now = signals_[ff.clock] != 0;
        const bool rising_edge = clock_now && !previous_clock_[i];
        next_states_[i] = static_cast<std::uint8_t>(rising_edge ? signals_[ff.d] : signals_[ff.q]);
        previous_clock_[i] = clock_now;
    }

    bool state_changed = false;
    for (std::size_t i = 0; i < circuit_.flip_flops().size(); ++i) {
        const auto q = circuit_.flip_flops()[i].q;
        if (signals_[q] != next_states_[i]) {
            const auto update_start = config_.enable_profiling ? ProfileClock::now() : ProfileClock::time_point{};
            signals_[q] = next_states_[i];
            if (config_.enable_profiling) {
                ++profile_.signal_write_count; ++profile_.signal_change_count;
                ++profile_.signal_writes; ++profile_.signal_changes;
                profile_.signal_update_time_ns += elapsed_ns(update_start, ProfileClock::now());
            }
            state_changed = true;
            if (config_.backend == Backend::Interpreted &&
                config_.strategy == InterpreterStrategy::EventDriven) {
                schedule_fanout(q);
            } else if (config_.backend == Backend::OptimizedIR &&
                       config_.strategy == InterpreterStrategy::EventDriven) {
                schedule_ir_fanout(q);
            }
        }
    }

    if (state_changed) {
        const auto after_clock = settle();
        total.converged = total.converged && after_clock.converged;
        total.operations += after_clock.operations;
        total.events += after_clock.events;
        total.dense_passes += after_clock.dense_passes;
    }

    ++ticks_;
    return total;
}

std::uint64_t Simulator::tick_count() const noexcept { return ticks_; }
Backend Simulator::backend() const noexcept { return config_.backend; }
InterpreterStrategy Simulator::strategy() const noexcept { return config_.strategy; }
bool Simulator::compiled_cache_hit() const noexcept {
    return native_module_ != nullptr && native_module_->cache_hit();
}
const CompilerStatistics* Simulator::compiler_statistics() const noexcept {
    return has_compiler_statistics_ ? &compiler_statistics_ : nullptr;
}
const NativeCompileTimings* Simulator::native_timings() const noexcept {
    return native_module_ != nullptr ? &native_module_->timings() : nullptr;
}
std::uint32_t Simulator::signal_count() const noexcept {
    return static_cast<std::uint32_t>(signals_.size());
}
const ExecutionProfile& Simulator::execution_profile() const noexcept { return profile_; }

} // namespace hcs
