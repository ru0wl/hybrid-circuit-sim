#pragma once

#include "hcs/circuit.hpp"
#include "hcs/native_compiler.hpp"
#include "hcs/compiler_pipeline.hpp"
#include "hcs/hardware.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace hcs {

namespace detail { class WorkerPool; }

enum class Backend : std::uint8_t {
    Interpreted,
    Compiled,
    OptimizedIR,
    HybridIR, // Experimental region-dirty optimized IR executor.
};

enum class InterpreterStrategy : std::uint8_t {
    Dense,
    EventDriven,
};

struct SimulatorConfig {
    Backend backend{Backend::Interpreted};
    InterpreterStrategy strategy{InterpreterStrategy::EventDriven};
    std::uint32_t max_dense_passes{64};
    std::uint64_t max_events_per_settle{1'000'000};
    NativeCompileOptions native_options{};
    optimizer::Options optimization_options{optimizer::Options::defaults()};
    bool enable_profiling{false};
    AccelerationConfig acceleration{};
};

struct ExecutionProfile {
    std::uint64_t dense_operations{};
    std::uint64_t event_operations{};
    std::uint64_t events_enqueued{};
    std::uint64_t events_processed{};
    std::uint64_t duplicate_schedule_attempts{};
    std::uint64_t dependent_schedule_attempts{};
    std::uint64_t signal_writes{};
    std::uint64_t signal_changes{};
    std::uint64_t unchanged_writes{};
    std::uint64_t settle_calls{};
    std::uint64_t settle_call_count{};
    std::uint64_t settle_iteration_count{};
    std::uint64_t maximum_settle_iterations{};
    std::uint64_t oscillation_limit_hits{};
    std::uint64_t queue_enqueue_time_ns{};
    std::uint64_t queue_dequeue_time_ns{};
    std::uint64_t queue_enqueue_count{};
    std::uint64_t queue_dequeue_count{};
    std::uint64_t fanout_traversal_time_ns{};
    std::uint64_t fanout_edges_visited{};
    std::uint64_t dependent_operations_found{};
    std::uint64_t scheduling_time_ns{};
    std::uint64_t schedule_attempts{};
    std::uint64_t scheduled_operations{};
    std::uint64_t signal_update_time_ns{};
    std::uint64_t signal_write_count{};
    std::uint64_t signal_change_count{};
    std::uint64_t unchanged_signal_write_count{};
    std::uint64_t operation_evaluation_time_ns{};
    std::uint64_t operation_evaluation_count{};
    std::uint64_t maximum_queue_length{};
    std::uint64_t queue_max_length{};
    // Legacy fields retained for source compatibility.
    std::uint64_t queue_time_ns{};
    std::uint64_t evaluation_time_ns{};
    std::uint64_t region_schedule_attempts{};
    std::uint64_t regions_scheduled{};
    std::uint64_t regions_processed{};
    std::uint64_t region_operation_evaluations{};
    std::uint64_t maximum_dirty_region_queue{};
};

struct AccelerationStats {
    std::uint64_t simd_batches{};
    std::uint64_t simd_operations{};
    std::uint64_t parallel_region_batches{};
    std::uint64_t parallel_regions{};
};

struct StepResult {
    bool converged{true};
    std::uint64_t operations{0};
    // Work-unit count is backend-specific. HybridIR counts acyclic region
    // activations and individual operation events in cyclic-region queues.
    std::uint64_t events{0};
    std::uint32_t dense_passes{0};
};

class Simulator final {
public:
    explicit Simulator(const Circuit& circuit, SimulatorConfig config = {});
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void set_input(SignalId input, bool value);
    [[nodiscard]] bool signal(SignalId id) const;
    [[nodiscard]] StepResult step();

    [[nodiscard]] std::uint64_t tick_count() const noexcept;
    [[nodiscard]] Backend backend() const noexcept;
    [[nodiscard]] InterpreterStrategy strategy() const noexcept;
    [[nodiscard]] bool compiled_cache_hit() const noexcept;
    [[nodiscard]] const CompilerStatistics* compiler_statistics() const noexcept;
    [[nodiscard]] const NativeCompileTimings* native_timings() const noexcept;
    [[nodiscard]] std::uint32_t signal_count() const noexcept;
    [[nodiscard]] const ExecutionProfile& execution_profile() const noexcept;
    [[nodiscard]] SimdImplementation simd_implementation() const noexcept;
    [[nodiscard]] std::uint32_t worker_thread_count() const noexcept;
    [[nodiscard]] const AccelerationStats& acceleration_stats() const noexcept;

private:
    void schedule_gate(GateId id);
    void schedule_fanout(SignalId signal);
    [[nodiscard]] StepResult settle_dense();
    [[nodiscard]] StepResult settle_events();
    [[nodiscard]] StepResult settle_compiled();
    [[nodiscard]] StepResult settle_ir();
    [[nodiscard]] StepResult settle_ir_events();
    [[nodiscard]] StepResult settle_ir_hybrid();
    void schedule_ir_regions_for_signal(SignalId signal);
    void schedule_ir_region(std::uint32_t region);
    void schedule_hybrid_region_operation(std::uint32_t region, ir::OperationId operation);
    [[nodiscard]] StepResult settle_hybrid_acyclic_region(std::uint32_t region, AccelerationStats* local_stats = nullptr);
    [[nodiscard]] StepResult settle_hybrid_region(std::uint32_t region, std::uint64_t event_budget);
    void schedule_ir_operation(ir::OperationId id);
    void schedule_ir_fanout(SignalId signal);
    [[nodiscard]] StepResult settle();

    const Circuit& circuit_;
    SimulatorConfig config_;
    std::vector<std::uint8_t> signals_;
    std::vector<std::uint8_t> next_states_;
    std::vector<std::vector<GateId>> fanout_;
    std::vector<bool> is_input_;
    std::vector<bool> queued_;
    std::vector<GateId> event_queue_;
    std::size_t queue_head_{0};
    std::vector<bool> previous_clock_;
    std::shared_ptr<NativeModule> native_module_;
    ir::CircuitIR optimized_ir_;
    analysis::DependencyGraph ir_dependencies_;
    std::vector<std::vector<ir::OperationId>> ir_fanout_;
    std::vector<std::uint8_t> ir_queued_;
    analysis::DependencyGraph hybrid_dependencies_;
    std::vector<std::vector<ir::OperationId>> hybrid_signal_consumers_;
    struct HybridOperationBatch {
        LogicOp opcode{};
        std::vector<ir::OperationId> operations;
    };
    std::vector<std::vector<ir::OperationId>> hybrid_region_operations_;
    std::vector<std::vector<std::vector<HybridOperationBatch>>> hybrid_region_levels_;
    std::vector<std::uint8_t> hybrid_region_acyclic_;
    std::vector<std::uint8_t> hybrid_region_simd_eligible_;
    std::vector<std::vector<ir::OperationId>> hybrid_region_event_queues_;
    std::vector<std::size_t> hybrid_region_event_queue_heads_;
    std::vector<std::uint8_t> hybrid_operation_queued_;
    std::vector<std::uint8_t> ir_region_queued_;
    std::vector<std::uint32_t> ir_region_queue_;
    std::size_t ir_region_queue_head_{0};
    std::vector<ir::OperationId> ir_event_queue_;
    SimdImplementation selected_simd_{SimdImplementation::Scalar};
    std::uint32_t requested_worker_threads_{1};
    std::unique_ptr<detail::WorkerPool> worker_pool_;
    std::vector<StepResult> hybrid_parallel_results_;
    std::vector<AccelerationStats> hybrid_parallel_acceleration_stats_;
    AccelerationStats acceleration_stats_{};
    std::size_t ir_queue_head_{0};
    CompilerStatistics compiler_statistics_{};
    bool has_compiler_statistics_{false};
    std::uint64_t ticks_{0};
    ExecutionProfile profile_{};
};

} // namespace hcs
