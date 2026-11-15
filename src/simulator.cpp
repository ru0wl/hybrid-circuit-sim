#include "hcs/simulator.hpp"

#include "hardware/simd_internal.hpp"
#include "hardware/worker_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <queue>
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

struct HybridRegionTaskContext {
    Simulator* simulator{};
    const std::vector<std::uint32_t>* regions{};
    std::vector<StepResult>* results{};
    std::vector<AccelerationStats>* acceleration_stats{};
};

Simulator::Simulator(const Circuit& circuit, SimulatorConfig config)
    : circuit_(circuit), config_(std::move(config)),
      signals_(circuit.signal_count(), 0), next_states_(circuit.flip_flops().size(), 0), fanout_(circuit.signal_count()),
      is_input_(circuit.signal_count(), false), queued_(circuit.gates().size(), false),
      previous_clock_(circuit.flip_flops().size(), false) {
    if (config_.acceleration.gpu_enabled)
        throw std::runtime_error("GPU compute is unavailable: this build has no Vulkan compute backend");
    if (config_.acceleration.parallel == CpuParallelMode::Custom && config_.acceleration.worker_threads == 0)
        throw std::invalid_argument("Custom CPU parallelism requires at least one worker thread");
    if (config_.acceleration.parallel == CpuParallelMode::Custom && config_.acceleration.worker_threads > 64)
        throw std::invalid_argument("Custom CPU parallelism is limited to 64 total worker threads");

    const auto capabilities = detect_hardware_capabilities();
    if (config_.acceleration.simd == SimdMode::AVX2 && !capabilities.avx2_usable) {
        if (!capabilities.avx2_cpu_supported)
            throw std::runtime_error("AVX2 was explicitly requested, but the CPU/OS does not support AVX2 state");
        throw std::runtime_error("AVX2 was explicitly requested, but this build has no AVX2 kernel; configure with HCS_ENABLE_AVX2=ON on x86 GCC/Clang");
    }
    if (config_.backend == Backend::HybridIR) {
        // Phase measurements did not establish a consistent AVX2 win over
        // scalar execution. Auto therefore remains scalar; AVX2 is explicitly opt-in.
        if (config_.acceleration.simd == SimdMode::AVX2 && !config_.enable_profiling)
            selected_simd_ = SimdImplementation::AVX2;
        switch (config_.acceleration.parallel) {
            case CpuParallelMode::Off: requested_worker_threads_ = 1; break;
            case CpuParallelMode::Auto: requested_worker_threads_ = std::min<std::uint32_t>(capabilities.logical_cpu_count, 32u); break;
            case CpuParallelMode::Custom: requested_worker_threads_ = config_.acceleration.worker_threads; break;
        }
        requested_worker_threads_ = std::max<std::uint32_t>(1, requested_worker_threads_);
        if (requested_worker_threads_ > 1 && !config_.enable_profiling)
            worker_pool_ = std::make_unique<detail::WorkerPool>(requested_worker_threads_);
    }
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
    } else if (config_.backend == Backend::OptimizedIR || config_.backend == Backend::HybridIR) {
        auto prepared = prepare_circuit(circuit_, config_.optimization_options);
        compiler_statistics_ = prepared.statistics;
        has_compiler_statistics_ = true;
        optimized_ir_ = std::move(prepared.optimized_ir);
        if (config_.backend == Backend::HybridIR) {
            // Analyze the optimized IR itself: optimization can change the operation list,
            // so region IDs from the pre-optimization dependency graph are not reusable.
            hybrid_dependencies_ = analysis::analyze(optimized_ir_);
            hybrid_signal_consumers_.resize(optimized_ir_.signal_count);
            ir_region_queued_.assign(hybrid_dependencies_.regions.size(), 0);
            hybrid_region_operations_.resize(hybrid_dependencies_.regions.size());
            hybrid_region_levels_.resize(hybrid_dependencies_.regions.size());
            hybrid_region_acyclic_.assign(hybrid_dependencies_.regions.size(), 0);
            hybrid_region_simd_eligible_.assign(hybrid_dependencies_.regions.size(), 0);
            hybrid_region_event_queues_.resize(hybrid_dependencies_.regions.size());
            hybrid_region_event_queue_heads_.assign(hybrid_dependencies_.regions.size(), 0);
            hybrid_operation_queued_.assign(optimized_ir_.operations.size(), 0);
            for (const auto& region : hybrid_dependencies_.regions) {
                auto& order = hybrid_region_operations_[region.id];
                order = region.operations;
                if (region.kind != analysis::RegionKind::AcyclicCombinational || order.empty()) continue;

                std::vector<std::uint8_t> member(optimized_ir_.operations.size(), 0);
                std::vector<std::uint32_t> indegree(optimized_ir_.operations.size(), 0);
                for (const auto operation : order) member[operation] = 1;
                std::priority_queue<ir::OperationId, std::vector<ir::OperationId>,
                                    std::greater<ir::OperationId>> ready;
                for (const auto operation : order) {
                    for (const auto predecessor : hybrid_dependencies_.predecessors[operation])
                        if (member[predecessor]) ++indegree[operation];
                    if (indegree[operation] == 0) ready.push(operation);
                }
                std::vector<ir::OperationId> topological;
                topological.reserve(order.size());
                while (!ready.empty()) {
                    const auto operation = ready.top();
                    ready.pop();
                    topological.push_back(operation);
                    for (const auto successor : hybrid_dependencies_.successors[operation]) {
                        if (member[successor] && indegree[successor] > 0 && --indegree[successor] == 0)
                            ready.push(successor);
                    }
                }
                if (topological.size() == order.size()) {
                    order = std::move(topological);
                    hybrid_region_acyclic_[region.id] = 1;

                    // Precompute deterministic dependency levels and opcode batches.
                    // Operations in one level are independent: none consumes another
                    // operation's output from that same level.
                    std::vector<std::uint32_t> level_of(optimized_ir_.operations.size(), 0);
                    std::uint32_t maximum_level = 0;
                    for (const auto operation : order) {
                        for (const auto predecessor : hybrid_dependencies_.predecessors[operation]) {
                            if (member[predecessor])
                                level_of[operation] = std::max(level_of[operation], level_of[predecessor] + 1);
                        }
                        maximum_level = std::max(maximum_level, level_of[operation]);
                    }
                    using OpcodeGroups = std::array<std::vector<ir::OperationId>, 10>;
                    std::vector<OpcodeGroups> groups(static_cast<std::size_t>(maximum_level) + 1);
                    for (const auto operation : order) {
                        const auto opcode = static_cast<std::size_t>(optimized_ir_.operations[operation].opcode);
                        if (opcode < groups[level_of[operation]].size())
                            groups[level_of[operation]][opcode].push_back(operation);
                    }
                    auto& levels = hybrid_region_levels_[region.id];
                    levels.resize(groups.size());
                    for (std::size_t level = 0; level < groups.size(); ++level) {
                        for (std::size_t opcode = 0; opcode < groups[level].size(); ++opcode) {
                            if (groups[level][opcode].empty()) continue;
                            if (groups[level][opcode].size() >= 32)
                                hybrid_region_simd_eligible_[region.id] = 1;
                            levels[level].push_back(HybridOperationBatch{
                                static_cast<LogicOp>(opcode), std::move(groups[level][opcode])});
                        }
                    }
                } else {
                    std::sort(order.begin(), order.end());
                }
            }
            for (std::size_t i = 0; i < optimized_ir_.operations.size(); ++i) {
                const auto region = hybrid_dependencies_.operation_region[i];
                optimized_ir_.operations[i].region = region;
                if (region == UINT32_MAX) continue;
                const auto& op = optimized_ir_.operations[i];
                if (op.a != kInvalidSignal && op.a < hybrid_signal_consumers_.size())
                    hybrid_signal_consumers_[op.a].push_back(static_cast<ir::OperationId>(i));
                if (op.b != kInvalidSignal && op.b != op.a && op.b < hybrid_signal_consumers_.size())
                    hybrid_signal_consumers_[op.b].push_back(static_cast<ir::OperationId>(i));
            }
            for (auto& consumers : hybrid_signal_consumers_) {
                std::sort(consumers.begin(), consumers.end());
                consumers.erase(std::unique(consumers.begin(), consumers.end()), consumers.end());
            }
            // Seed every region once. Acyclic regions are evaluated topologically;
            // feedback regions seed their own reusable operation queues.
            for (const auto& region : hybrid_dependencies_.regions) {
                if (region.operations.empty()) continue;
                schedule_ir_region(region.id);
                if (hybrid_region_acyclic_[region.id] == 0) {
                    auto& queue = hybrid_region_event_queues_[region.id];
                    queue.reserve(region.operations.size());
                    for (const auto operation : region.operations)
                        schedule_hybrid_region_operation(region.id, operation);
                }
            }
        } else {
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
        }
    } else if (config_.strategy == InterpreterStrategy::EventDriven) {
        for (std::size_t i = 0; i < circuit.gates().size(); ++i) {
            schedule_gate(static_cast<GateId>(i));
        }
    }
}

Simulator::~Simulator() = default;

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

void Simulator::schedule_ir_region(std::uint32_t region) {
    if (region >= ir_region_queued_.size() || ir_region_queued_[region]) {
        if (config_.enable_profiling) {
            ++profile_.region_schedule_attempts;
            if (region < ir_region_queued_.size()) ++profile_.duplicate_schedule_attempts;
        }
        return;
    }
    ir_region_queued_[region] = 1;
    ir_region_queue_.push_back(region);
    if (config_.enable_profiling) {
        ++profile_.region_schedule_attempts;
        ++profile_.regions_scheduled;
        ++profile_.events_enqueued;
        ++profile_.queue_enqueue_count;
        profile_.maximum_dirty_region_queue = std::max<std::uint64_t>(
            profile_.maximum_dirty_region_queue, ir_region_queue_.size() - ir_region_queue_head_);
    }
}

void Simulator::schedule_hybrid_region_operation(std::uint32_t region, ir::OperationId operation) {
    if (region >= hybrid_region_event_queues_.size() || operation >= hybrid_operation_queued_.size()) return;
    if (config_.enable_profiling) {
        ++profile_.schedule_attempts;
        ++profile_.dependent_schedule_attempts;
    }
    if (hybrid_operation_queued_[operation]) {
        if (config_.enable_profiling) ++profile_.duplicate_schedule_attempts;
        return;
    }
    hybrid_operation_queued_[operation] = 1;
    auto& queue = hybrid_region_event_queues_[region];
    auto& head = hybrid_region_event_queue_heads_[region];
    if (head == queue.size()) {
        queue.clear();
        head = 0;
    }
    queue.push_back(operation);
    if (config_.enable_profiling) {
        ++profile_.scheduled_operations;
        ++profile_.events_enqueued;
        ++profile_.queue_enqueue_count;
        const auto pending = queue.size() - head;
        profile_.maximum_queue_length = std::max<std::uint64_t>(profile_.maximum_queue_length, pending);
        profile_.queue_max_length = profile_.maximum_queue_length;
    }
}

void Simulator::schedule_ir_regions_for_signal(SignalId signal_id) {
    if (signal_id >= hybrid_signal_consumers_.size()) return;
    for (const auto operation : hybrid_signal_consumers_[signal_id]) {
        if (config_.enable_profiling) {
            ++profile_.fanout_edges_visited;
            ++profile_.dependent_operations_found;
        }
        const auto region = hybrid_dependencies_.operation_region[operation];
        if (region == UINT32_MAX) continue;
        schedule_ir_region(region);
        if (hybrid_region_acyclic_[region] == 0)
            schedule_hybrid_region_operation(region, operation);
    }
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
    } else if (config_.backend == Backend::HybridIR) {
        schedule_ir_regions_for_signal(input);
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

StepResult Simulator::settle_hybrid_acyclic_region(std::uint32_t region, AccelerationStats* local_stats) {
#if !defined(HCS_HAVE_AVX2_KERNEL)
    (void)local_stats;
#endif
    StepResult result;
    if (region >= hybrid_region_operations_.size()) return result;
    result.events = 1;
    result.dense_passes = 1;

    const auto& order = hybrid_region_operations_[region];
    const bool use_avx2 = selected_simd_ == SimdImplementation::AVX2 &&
                          hybrid_region_simd_eligible_[region] != 0 && !config_.enable_profiling;
    const auto& levels = hybrid_region_levels_[region];
    if (use_avx2 && !levels.empty()) {
#if defined(HCS_HAVE_AVX2_KERNEL)
        alignas(32) std::array<std::uint8_t, 32> lhs{};
        alignas(32) std::array<std::uint8_t, 32> rhs{};
        alignas(32) std::array<std::uint8_t, 32> output{};
        for (const auto& level : levels) {
            for (const auto& batch : level) {
                std::size_t offset = 0;
                for (; offset + lhs.size() <= batch.operations.size(); offset += lhs.size()) {
                    for (std::size_t lane = 0; lane < lhs.size(); ++lane) {
                        const auto& op = optimized_ir_.operations[batch.operations[offset + lane]];
                        lhs[lane] = op.a == kInvalidSignal ? 0u : signals_[op.a];
                        rhs[lane] = op.b == kInvalidSignal ? 0u : signals_[op.b];
                    }
                    detail::evaluate_avx2_batch(batch.opcode, lhs.data(), rhs.data(), output.data(), lhs.size());
                    auto& stats = local_stats != nullptr ? *local_stats : acceleration_stats_;
                    ++stats.simd_batches;
                    stats.simd_operations += lhs.size();
                    for (std::size_t lane = 0; lane < lhs.size(); ++lane) {
                        const auto& op = optimized_ir_.operations[batch.operations[offset + lane]];
                        signals_[op.output] = output[lane];
                    }
                }
                for (; offset < batch.operations.size(); ++offset) {
                    const auto& op = optimized_ir_.operations[batch.operations[offset]];
                    signals_[op.output] = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
                }
                result.operations += batch.operations.size();
                if (config_.enable_profiling) {
                    profile_.dense_operations += batch.operations.size();
                    profile_.region_operation_evaluations += batch.operations.size();
                    profile_.operation_evaluation_count += batch.operations.size();
                    profile_.signal_write_count += batch.operations.size();
                    profile_.signal_writes += batch.operations.size();
                }
            }
        }
        return result;
#else
        // A non-x86 or unsupported toolchain build cannot contain the AVX2 kernel.
#endif
    }

    for (const auto operation : order) {
        const auto& op = optimized_ir_.operations[operation];
        const auto next = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
        const bool changed = signals_[op.output] != next;
        if (changed) signals_[op.output] = next;
        ++result.operations;
        if (config_.enable_profiling) {
            ++profile_.dense_operations;
            ++profile_.region_operation_evaluations;
            ++profile_.operation_evaluation_count;
            ++profile_.signal_write_count;
            ++profile_.signal_writes;
            if (changed) {
                ++profile_.signal_change_count;
                ++profile_.signal_changes;
            } else {
                ++profile_.unchanged_signal_write_count;
                ++profile_.unchanged_writes;
            }
        }
    }
    return result;
}

StepResult Simulator::settle_hybrid_region(std::uint32_t region, std::uint64_t event_budget) {
    if (region >= hybrid_region_acyclic_.size() || event_budget == 0) return {};
    if (hybrid_region_acyclic_[region] != 0) return settle_hybrid_acyclic_region(region);

    StepResult result;
    auto& queue = hybrid_region_event_queues_[region];
    auto& head = hybrid_region_event_queue_heads_[region];
    while (head < queue.size() && result.events < event_budget) {
        const auto operation_id = queue[head++];
        hybrid_operation_queued_[operation_id] = 0;
        ++result.events;
        ++result.operations;
        if (config_.enable_profiling) {
            ++profile_.settle_iteration_count;
            ++profile_.events_processed;
            ++profile_.queue_dequeue_count;
            ++profile_.event_operations;
            ++profile_.region_operation_evaluations;
            ++profile_.operation_evaluation_count;
            ++profile_.signal_write_count;
            ++profile_.signal_writes;
        }
        const auto& op = optimized_ir_.operations[operation_id];
        const auto next = static_cast<std::uint8_t>(evaluate_ir_operation(op, signals_));
        if (signals_[op.output] == next) {
            if (config_.enable_profiling) {
                ++profile_.unchanged_signal_write_count;
                ++profile_.unchanged_writes;
            }
            continue;
        }
        signals_[op.output] = next;
        if (config_.enable_profiling) {
            ++profile_.signal_change_count;
            ++profile_.signal_changes;
        }
        for (const auto successor : hybrid_dependencies_.successors[operation_id])
            schedule_hybrid_region_operation(region, successor);
    }
    if (head == queue.size()) {
        queue.clear();
        head = 0;
    } else {
        result.converged = false;
    }
    return result;
}

StepResult Simulator::settle_ir_hybrid() {
    StepResult result;
    if (config_.enable_profiling) {
        ++profile_.settle_calls;
        ++profile_.settle_call_count;
    }

    // Regions are weakly connected components of the optimized operation
    // dependency graph. A batch containing only acyclic regions has no cross-region
    // signal dependencies or write conflicts and can be executed in parallel.
    // Feedback regions deliberately retain the serial FIFO path below.
    const auto queued_regions = ir_region_queue_.size() - ir_region_queue_head_;
    bool all_acyclic = worker_pool_ != nullptr &&
                       queued_regions >= static_cast<std::size_t>(worker_pool_->worker_count()) * 2u &&
                       !config_.enable_profiling && queued_regions <= config_.max_events_per_settle;
    std::uint64_t queued_operations = 0;
    if (all_acyclic) {
        for (std::size_t i = ir_region_queue_head_; i < ir_region_queue_.size(); ++i) {
            const auto region = ir_region_queue_[i];
            if (region >= hybrid_region_acyclic_.size() || hybrid_region_acyclic_[region] == 0) {
                all_acyclic = false;
                break;
            }
            queued_operations += hybrid_region_operations_[region].size();
        }
        // The pool wake-up/barrier cost dominates smaller workloads. This fixed
        // operation-count floor was selected from the phase benchmark experiment.
        if (queued_operations < 4096) all_acyclic = false;
    }
    if (all_acyclic) {
        const auto begin = ir_region_queue_head_;
        const auto count = queued_regions;
        hybrid_parallel_results_.resize(count);
        hybrid_parallel_acceleration_stats_.resize(count);
        std::fill(hybrid_parallel_acceleration_stats_.begin(), hybrid_parallel_acceleration_stats_.end(), AccelerationStats{});
        for (std::size_t i = begin; i < ir_region_queue_.size(); ++i)
            ir_region_queued_[ir_region_queue_[i]] = 0;
        HybridRegionTaskContext context{this, &ir_region_queue_, &hybrid_parallel_results_, &hybrid_parallel_acceleration_stats_};
        struct OffsetContext {
            HybridRegionTaskContext* base;
            std::size_t offset;
        } offset_context{&context, begin};
        // The pool callback indexes from zero; use a local callback adapter.
        const auto callback = +[](void* raw, std::size_t index) noexcept {
            auto* offset = static_cast<OffsetContext*>(raw);
            const auto region = (*offset->base->regions)[offset->offset + index];
            auto& local_stats = (*offset->base->acceleration_stats)[index];
            (*offset->base->results)[index] = offset->base->simulator->settle_hybrid_acyclic_region(region, &local_stats);
        };
        worker_pool_->parallel_for(count, &offset_context, callback);
        ++acceleration_stats_.parallel_region_batches;
        acceleration_stats_.parallel_regions += count;
        for (std::size_t i = 0; i < hybrid_parallel_results_.size(); ++i) {
            const auto& partial = hybrid_parallel_results_[i];
            const auto& local_stats = hybrid_parallel_acceleration_stats_[i];
            result.events += partial.events;
            result.operations += partial.operations;
            result.dense_passes += partial.dense_passes;
            result.converged = result.converged && partial.converged;
            acceleration_stats_.simd_batches += local_stats.simd_batches;
            acceleration_stats_.simd_operations += local_stats.simd_operations;
        }
        ir_region_queue_.clear();
        ir_region_queue_head_ = 0;
        return result;
    }

    while (ir_region_queue_head_ < ir_region_queue_.size() &&
           result.events < config_.max_events_per_settle) {
        const auto region_id = ir_region_queue_[ir_region_queue_head_++];
        ir_region_queued_[region_id] = 0;
        if (config_.enable_profiling) {
            ++profile_.regions_processed;
            ++profile_.queue_dequeue_count;
        }

        const auto remaining_budget = config_.max_events_per_settle > result.events
            ? config_.max_events_per_settle - result.events : 0;
        const auto partial = settle_hybrid_region(region_id, remaining_budget);
        result.events += partial.events;
        result.operations += partial.operations;
        result.dense_passes += partial.dense_passes;
        result.converged = result.converged && partial.converged;
        if (config_.enable_profiling && hybrid_region_acyclic_[region_id] != 0) {
            ++profile_.settle_iteration_count;
            ++profile_.events_processed;
            profile_.maximum_settle_iterations = std::max<std::uint64_t>(
                profile_.maximum_settle_iterations, result.events);
        }
        if (!partial.converged) {
            schedule_ir_region(region_id);
            if (config_.enable_profiling) ++profile_.oscillation_limit_hits;
            break;
        }
    }

    if (ir_region_queue_head_ == ir_region_queue_.size()) {
        ir_region_queue_.clear();
        ir_region_queue_head_ = 0;
    } else {
        result.converged = false;
        if (config_.enable_profiling) ++profile_.oscillation_limit_hits;
    }
    return result;
}

StepResult Simulator::settle() {
    if (config_.backend == Backend::Compiled) return settle_compiled();
    if (config_.backend == Backend::HybridIR) return settle_ir_hybrid();
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
            } else if (config_.backend == Backend::HybridIR) {
                schedule_ir_regions_for_signal(q);
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
SimdImplementation Simulator::simd_implementation() const noexcept { return selected_simd_; }
std::uint32_t Simulator::worker_thread_count() const noexcept {
    return worker_pool_ ? worker_pool_->worker_count() : 1u;
}
const AccelerationStats& Simulator::acceleration_stats() const noexcept { return acceleration_stats_; }

} // namespace hcs
