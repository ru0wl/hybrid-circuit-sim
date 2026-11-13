#include "hcs/simulator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct BenchCircuit {
    hcs::Circuit circuit;
    hcs::SignalId active_input{hcs::kInvalidSignal};
    hcs::SignalId clock_input{hcs::kInvalidSignal};
    hcs::SignalId output{hcs::kInvalidSignal};
    bool sequential{false};
    double construction_ms{0};
    hcs::SignalId secondary_input{hcs::kInvalidSignal};
    hcs::SignalId feedback_state{hcs::kInvalidSignal};
    bool feedback_dynamic{false};
};

struct Options {
    std::uint32_t size{4000};
    std::uint64_t iterations{10000};
    std::uint32_t runs{20};
    std::uint32_t cold_runs{3};
    std::uint32_t warmup{4};
    std::string scenario{"all"};
    std::filesystem::path cache{std::filesystem::temp_directory_path() / "hcs-bench-phase4-cache"};
    std::filesystem::path output_prefix{"benchmark-results-phase4"};
    std::string compile_mode{"both"};
    bool profile{false};
    double min_sample_ms{20.0};
    std::uint64_t max_calibration_iterations{100'000'000ull};
    std::string native_compiler{"c++"};
    std::string optimization_flag{"-O2"};
};

BenchCircuit make_sparse_circuit(std::uint32_t size) {
    hcs::CircuitBuilder builder;
    const auto active = builder.add_input();
    const auto inactive = builder.add_input();
    const auto one = builder.add_constant(true);
    auto active_path = active;
    auto inactive_path = inactive;
    const auto active_gates = std::max<std::uint32_t>(1, size / 2);
    const auto inactive_gates = std::max<std::uint32_t>(1, size - active_gates);
    for (std::uint32_t i = 0; i < active_gates; ++i) active_path = builder.add_binary(hcs::LogicOp::And, active_path, one);
    for (std::uint32_t i = 0; i < inactive_gates; ++i) inactive_path = builder.add_binary(hcs::LogicOp::And, inactive_path, one);
    return {builder.build(), active, hcs::kInvalidSignal, active_path, false, 0};
}

BenchCircuit make_counter(std::uint32_t bits) {
    hcs::CircuitBuilder builder;
    const auto clock = builder.add_input();
    const auto one = builder.add_constant(true);
    std::vector<hcs::SignalId> q(bits);
    for (auto& signal : q) signal = builder.add_signal();
    auto carry = one;
    for (std::uint32_t bit = 0; bit < bits; ++bit) {
        const auto d = builder.add_binary(hcs::LogicOp::Xor, q[bit], carry);
        builder.add_d_flip_flop(d, clock, q[bit]);
        carry = builder.add_binary(hcs::LogicOp::And, q[bit], carry);
    }
    return {builder.build(), hcs::kInvalidSignal, clock, q.front(), true, 0};
}

BenchCircuit make_large_combinational(std::uint32_t size) {
    hcs::CircuitBuilder b;
    const auto a = b.add_input();
    const auto c = b.add_input();
    const hcs::LogicOp ops[] = {hcs::LogicOp::And, hcs::LogicOp::Or, hcs::LogicOp::Xor,
                                hcs::LogicOp::Nand, hcs::LogicOp::Nor, hcs::LogicOp::Xnor};
    auto out = a;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) {
        out = b.add_binary(ops[i % (sizeof(ops) / sizeof(ops[0]))], a, c);
    }
    BenchCircuit result{b.build(), a, hcs::kInvalidSignal, out, false, 0};
    result.secondary_input = c;
    return result;
}

BenchCircuit make_wide_fanout(std::uint32_t size) {
    hcs::CircuitBuilder b;
    const auto in = b.add_input();
    auto out = in;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) out = b.add_unary(hcs::LogicOp::Buffer, in);
    return {b.build(), in, hcs::kInvalidSignal, out, false, 0};
}

BenchCircuit make_chain(std::uint32_t size) {
    hcs::CircuitBuilder b; const auto in = b.add_input(); auto x = in;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) x = b.add_unary((i & 1u) ? hcs::LogicOp::Not : hcs::LogicOp::Buffer, x);
    return {b.build(), in, hcs::kInvalidSignal, x, false, 0};
}
BenchCircuit make_fanout(std::uint32_t size) {
    hcs::CircuitBuilder b;
    const auto in = b.add_input();
    const auto one = b.add_constant(true);
    auto out = in;
    for (std::uint32_t i = 0; i < std::max(1u, size / 3); ++i) {
        (void)b.add_unary(hcs::LogicOp::Not, in);
        (void)b.add_unary(hcs::LogicOp::Buffer, in);
        out = b.add_binary(hcs::LogicOp::Nand, b.add_unary(hcs::LogicOp::Not, in), one);
    }
    return {b.build(), in, hcs::kInvalidSignal, out, false, 0};
}
BenchCircuit make_repeated(std::uint32_t size) {
    hcs::CircuitBuilder b; const auto a = b.add_input(); const auto c = b.add_input(); auto out = a;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) { const auto x = b.add_binary(hcs::LogicOp::And, a, c); out = b.add_binary(hcs::LogicOp::Xor, out, x); }
    BenchCircuit result{b.build(), a, hcs::kInvalidSignal, out, false, 0};
    result.secondary_input = c;
    return result;
}
BenchCircuit make_redundant(std::uint32_t size) {
    hcs::CircuitBuilder b; const auto in = b.add_input(); const auto zero = b.add_constant(false); auto out = in;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) { (void)b.add_binary(hcs::LogicOp::And, in, zero); out = b.add_unary(hcs::LogicOp::Buffer, out); }
    return {b.build(), in, hcs::kInvalidSignal, out, false, 0};
}
BenchCircuit make_many_sequential(std::uint32_t size) { return make_counter(std::min<std::uint32_t>(std::max(2u, size), 512)); }
BenchCircuit make_independent(std::uint32_t size) {
    hcs::CircuitBuilder b; const auto in = b.add_input(); auto out = in;
    for (std::uint32_t i = 0; i < std::max(1u, size); ++i) out = b.add_unary((i & 1u) ? hcs::LogicOp::Not : hcs::LogicOp::Buffer, in);
    return {b.build(), in, hcs::kInvalidSignal, out, false, 0};
}
BenchCircuit make_feedback_stable(std::uint32_t size) {
    hcs::CircuitBuilder b;
    const auto set = b.add_input();
    const auto x = b.add_signal();
    b.add_gate(hcs::LogicOp::Or, x, set, x);
    auto out = x;
    for (std::uint32_t i = 1; i < std::max(1u, size / 8); ++i) out = b.add_unary(hcs::LogicOp::Buffer, out);
    BenchCircuit result{b.build(), set, hcs::kInvalidSignal, out, false, 0};
    result.feedback_state = x;
    return result;
}

BenchCircuit make_feedback_dynamic(std::uint32_t size) {
    hcs::CircuitBuilder b;
    const auto set = b.add_input();
    const auto hold = b.add_input();
    const auto x = b.add_signal();
    const auto held = b.add_binary(hcs::LogicOp::And, x, hold);
    b.add_gate(hcs::LogicOp::Or, held, set, x);
    auto out = x;
    for (std::uint32_t i = 1; i < std::max(1u, size / 8); ++i) out = b.add_unary(hcs::LogicOp::Buffer, out);
    BenchCircuit result{b.build(), set, hcs::kInvalidSignal, out, false, 0};
    result.secondary_input = hold;
    result.feedback_state = x;
    result.feedback_dynamic = true;
    return result;
}

template<class F>
BenchCircuit timed_circuit(F&& factory) {
    const auto begin = Clock::now();
    auto result = factory();
    result.construction_ms = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
    return result;
}

struct Sample {
    double elapsed_ms{};
    double requested_target_ms{};
    std::uint64_t calibration_iterations{};
    bool duration_target_reached{true};
    bool accepted{true};
    std::uint64_t feedback_initial_state{};
    std::uint64_t feedback_final_state{};
    std::uint64_t feedback_state_changes{};
    bool converged{true};
    double setup_ms{};
    double ir_ms{};
    double analysis_ms{};
    double optimization_ms{};
    double codegen_ms{};
    double cache_lookup_ms{};
    double compile_ms{};
    double load_ms{};
    double preparation_ms{};
    std::uint64_t steps{};
    std::uint64_t operations{};
    std::uint64_t events{};
    std::uint64_t checksum{};
    bool cache_hit{};
    hcs::ExecutionProfile profile{};
    std::uint64_t rss_after_setup_kb{};
    std::uint64_t rss_after_run_kb{};
    std::uint64_t generated_source_bytes{};
    std::uint64_t shared_library_bytes{};
    std::uint64_t ir_operations{};
    std::uint64_t optimized_ir_operations{};
    std::uint64_t constants_folded{};
    std::uint64_t constants_propagated{};
    std::uint64_t dead_operations_removed{};
    std::uint64_t common_expressions_eliminated{};
    std::uint64_t redundant_operations_removed{};
    std::uint64_t fused_operations{};
    std::uint64_t signals{};
    std::uint64_t observable_signals{};
    std::uint64_t observable_operation_outputs{};
    std::uint64_t forwarding_operations_introduced{};
    std::uint64_t dependency_edges{};
    std::uint64_t scc_count{};
    std::uint64_t cyclic_scc_count{};
    std::uint64_t sequential_operations{};
    std::uint64_t maximum_fanout{};
    std::uint64_t regions{};
    std::uint32_t critical_path_length{};
    std::array<std::uint64_t, 10> opcode_counts{};
};

struct Summary {
    std::string scenario;
    std::string backend;
    std::uint32_t warmup{};
    std::uint32_t runs{};
    std::uint64_t iterations{};
    std::uint64_t steps{};
    std::uint64_t operations{};
    std::uint64_t events{};
    std::uint64_t checksum{};
    double median_ms{}, min_ms{}, max_ms{}, mean_ms{}, stddev_ms{}, p95_ms{};
    double requested_target_ms{};
    std::uint64_t calibration_iterations{};
    bool duration_target_reached{true};
    bool accepted{true};
    std::uint64_t feedback_initial_state{};
    std::uint64_t feedback_final_state{};
    std::uint64_t feedback_state_changes{};
    bool converged{true};
    double steps_per_second{}, operations_per_second{}, events_per_second{};
    double setup_ms{}, ir_ms{}, analysis_ms{}, optimization_ms{}, codegen_ms{}, cache_lookup_ms{}, compile_ms{}, load_ms{}, preparation_ms{};
    bool cache_hit{};
    hcs::ExecutionProfile profile{};
    double break_even_vs_event{-1.0};
    double break_even_vs_ir_event{-1.0};
    std::uint64_t rss_after_setup_kb{};
    std::uint64_t rss_after_run_kb{};
    std::uint64_t generated_source_bytes{};
    std::uint64_t shared_library_bytes{};
    double circuit_construction_ms{};
    std::uint32_t requested_size{};
    std::uint64_t ir_operations{};
    std::uint64_t optimized_ir_operations{};
    std::uint64_t constants_folded{};
    std::uint64_t constants_propagated{};
    std::uint64_t dead_operations_removed{};
    std::uint64_t common_expressions_eliminated{};
    std::uint64_t redundant_operations_removed{};
    std::uint64_t fused_operations{};
    std::uint64_t signals{};
    std::uint64_t observable_signals{};
    std::uint64_t observable_operation_outputs{};
    std::uint64_t forwarding_operations_introduced{};
    std::uint64_t dependency_edges{};
    std::uint64_t scc_count{};
    std::uint64_t cyclic_scc_count{};
    std::uint64_t sequential_operations{};
    std::uint64_t maximum_fanout{};
    std::uint64_t regions{};
    std::uint32_t critical_path_length{};
    std::array<std::uint64_t, 10> opcode_counts{};
};

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    if (values.empty()) return 0;
    const auto m = values.size() / 2;
    return values.size() % 2 ? values[m] : (values[m - 1] + values[m]) * 0.5;
}
double percentile95(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(0.95 * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}
double mean(const std::vector<double>& values) {
    if (values.empty()) return 0;
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}
double stddev(const std::vector<double>& values, double avg) {
    if (values.size() < 2) return 0;
    double sum = 0;
    for (const auto value : values) sum += (value - avg) * (value - avg);
    return std::sqrt(sum / static_cast<double>(values.size() - 1));
}

hcs::SimulatorConfig config_for(const std::string& backend, const std::filesystem::path& cache, const Options& options) {
    hcs::SimulatorConfig config;
    config.native_options.cache_directory = cache;
    config.enable_profiling = options.profile;
    config.native_options.compiler = options.native_compiler;
    config.native_options.optimization_flag = options.optimization_flag;
    if (backend == "interpreted-event") { config.backend = hcs::Backend::Interpreted; config.strategy = hcs::InterpreterStrategy::EventDriven; }
    else if (backend == "interpreted-dense") { config.backend = hcs::Backend::Interpreted; config.strategy = hcs::InterpreterStrategy::Dense; }
    else if (backend == "optimized-ir-dense") { config.backend = hcs::Backend::OptimizedIR; config.strategy = hcs::InterpreterStrategy::Dense; }
    else if (backend == "optimized-ir-event") { config.backend = hcs::Backend::OptimizedIR; config.strategy = hcs::InterpreterStrategy::EventDriven; }
    else { config.backend = hcs::Backend::Compiled; }
    return config;
}

std::uint64_t current_rss_kb() {
    std::ifstream status("/proc/self/status");
    std::string key;
    while (status >> key) {
        if (key == "VmRSS:") {
            std::uint64_t value = 0;
            std::string unit;
            status >> value >> unit;
            return value;
        }
        std::string rest;
        std::getline(status, rest);
    }
    return 0;
}

void execute_iterations(hcs::Simulator& simulator, const BenchCircuit& bench, std::uint64_t iterations,
                        Sample& sample, bool count) {
    bool last_feedback = bench.feedback_state != hcs::kInvalidSignal
        ? simulator.signal(bench.feedback_state) : false;
    if (count && bench.feedback_state != hcs::kInvalidSignal)
        sample.feedback_initial_state = last_feedback ? 1u : 0u;
    for (std::uint64_t i = 0; i < iterations; ++i) {
        if (bench.sequential) {
            simulator.set_input(bench.clock_input, true);
            const auto high = simulator.step();
            simulator.set_input(bench.clock_input, false);
            const auto low = simulator.step();
            if (count) {
                sample.operations += high.operations + low.operations;
                sample.events += high.events + low.events;
                sample.steps += 2;
                sample.converged = sample.converged && high.converged && low.converged;
            }
        } else {
            simulator.set_input(bench.active_input, (i & 1u) == 0);
            if (bench.secondary_input != hcs::kInvalidSignal)
                simulator.set_input(bench.secondary_input, bench.feedback_dynamic ? false : ((i & 2u) == 0));
            const auto result = simulator.step();
            if (count) {
                sample.operations += result.operations;
                sample.events += result.events;
                ++sample.steps;
                sample.converged = sample.converged && result.converged;
            }
        }
        if (count && bench.feedback_state != hcs::kInvalidSignal) {
            const bool current = simulator.signal(bench.feedback_state);
            if (current != last_feedback) ++sample.feedback_state_changes;
            last_feedback = current;
        }
    }
    if (count && bench.feedback_state != hcs::kInvalidSignal)
        sample.feedback_final_state = simulator.signal(bench.feedback_state) ? 1u : 0u;
}

Sample run_sample(const BenchCircuit& bench, const std::string& backend, const Options& options,
                  const std::filesystem::path& cache, bool cold_compile) {
    Sample sample;
    sample.requested_target_ms = options.min_sample_ms;
    sample.calibration_iterations = options.iterations;
    // Warmup and measured execution use separate fresh simulators to preserve identical initial state.
    {
        const auto warm_cache = cold_compile ? cache / "warmup-only" : cache;
        auto warm_config = config_for(backend, warm_cache, options);
        hcs::Simulator warm(bench.circuit, warm_config);
        execute_iterations(warm, bench, options.warmup, sample, false);
    }
    if (cold_compile) std::filesystem::remove_all(cache);
    auto config = config_for(backend, cache, options);
    const auto setup_start = Clock::now();
    hcs::Simulator simulator(bench.circuit, config);
    const auto setup_end = Clock::now();
    sample.setup_ms = std::chrono::duration<double, std::milli>(setup_end - setup_start).count();
    sample.cache_hit = simulator.compiled_cache_hit();
    sample.rss_after_setup_kb = current_rss_kb();
    if (const auto* stats = simulator.compiler_statistics()) {
        sample.ir_operations = stats->initial_ir_operations;
        sample.optimized_ir_operations = stats->final_ir_operations;
        sample.constants_folded = stats->constants_folded;
        sample.constants_propagated = stats->constants_propagated;
        sample.dead_operations_removed = stats->dead_operations_removed;
        sample.common_expressions_eliminated = stats->common_expressions_eliminated;
        sample.redundant_operations_removed = stats->redundant_operations_removed;
        sample.fused_operations = stats->fused_operations;
        sample.signals = stats->signals;
        sample.observable_signals = stats->observable_signals;
        sample.observable_operation_outputs = stats->observable_operation_outputs;
        sample.forwarding_operations_introduced = stats->forwarding_operations_introduced;
        sample.dependency_edges = stats->dependency_edges;
        sample.scc_count = stats->strongly_connected_components;
        sample.cyclic_scc_count = stats->cyclic_components;
        sample.sequential_operations = stats->sequential_operations;
        sample.maximum_fanout = stats->maximum_fanout;
        sample.regions = stats->regions;
        sample.critical_path_length = stats->critical_path_length;
        sample.opcode_counts = stats->opcode_counts;
        sample.ir_ms = stats->ir_generation_ms;
        sample.analysis_ms = stats->dependency_analysis_ms;
        sample.optimization_ms = stats->optimization_ms;
    }
    if (const auto* native = simulator.native_timings()) {
        sample.codegen_ms = native->code_generation_ms;
        sample.cache_lookup_ms = native->cache_lookup_ms;
        sample.compile_ms = native->compiler_invocation_ms;
        sample.load_ms = native->library_load_ms;
        sample.preparation_ms = native->total_preparation_ms;
        sample.generated_source_bytes = native->generated_source_bytes;
        sample.shared_library_bytes = native->shared_library_bytes;
    }
    const auto start = Clock::now();
    execute_iterations(simulator, bench, options.iterations, sample, true);
    const auto end = Clock::now();
    sample.elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    sample.duration_target_reached = options.min_sample_ms <= 0.0 || sample.elapsed_ms >= options.min_sample_ms;
    sample.accepted = sample.duration_target_reached;
    // Hash all public signals once, outside the timed region.
    std::uint64_t hash = 14695981039346656037ull;
    for (hcs::SignalId id = 0; id < simulator.signal_count(); ++id) {
        hash ^= simulator.signal(id) ? 1u : 0u;
        hash *= 1099511628211ull;
    }
    sample.checksum = hash;
    sample.profile = simulator.execution_profile();
    sample.rss_after_run_kb = current_rss_kb();
    return sample;
}

std::uint64_t calibrate_iterations(const BenchCircuit& bench, const Options& options,
                                   const std::filesystem::path& cache,
                                   const std::vector<std::string>& backends) {
    auto iterations = std::min(options.iterations, options.max_calibration_iterations);
    if (options.min_sample_ms <= 0.0) return iterations;
    for (const auto& backend : backends) {
        if (backend == "compiled-native-warm" || backend == "compiled-native-cold") {
            const auto backend_cache = cache / "compiled-native-calibration";
            std::filesystem::create_directories(backend_cache);
            auto config = config_for("compiled-native", backend_cache, options);
            hcs::Simulator prime(bench.circuit, config);
            break;
        }
    }
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        double fastest_elapsed = std::numeric_limits<double>::max();
        for (const auto& backend_name : backends) {
            const std::string backend = (backend_name == "compiled-native-cold" || backend_name == "compiled-native-warm")
                ? "compiled-native" : backend_name;
            const auto backend_cache = cache / (backend + "-calibration");
            auto config = config_for(backend, backend_cache, options);
            {
                hcs::Simulator warm(bench.circuit, config);
                Sample ignored;
                execute_iterations(warm, bench, options.warmup, ignored, false);
            }
            hcs::Simulator probe(bench.circuit, config);
            Sample ignored;
            const auto begin = Clock::now();
            execute_iterations(probe, bench, iterations, ignored, false);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
            fastest_elapsed = std::min(fastest_elapsed, elapsed);
        }
        if (fastest_elapsed >= options.min_sample_ms || fastest_elapsed <= 0.0) break;
        const auto scaled = static_cast<long double>(iterations) *
            (static_cast<long double>(options.min_sample_ms) / fastest_elapsed) * 1.10L;
        const auto next = static_cast<std::uint64_t>(std::ceil(scaled));
        if (next <= iterations || iterations >= options.max_calibration_iterations) break;
        iterations = std::min(next, options.max_calibration_iterations);
    }
    return iterations;
}

Summary summarize(const std::string& scenario, const std::string& backend, const Options& options,
                  const std::vector<Sample>& samples) {
    Summary out;
    out.scenario = scenario; out.backend = backend; out.warmup = options.warmup; out.runs = options.runs;
    out.iterations = options.iterations;
    std::vector<double> times, setups, ir, analysis, opt, codegen, cache, compile, load, prep;
    std::vector<double> rates_steps, rates_ops, rates_events;
    bool all_cache_hits = true;
    for (const auto& s : samples) {
        times.push_back(s.elapsed_ms); setups.push_back(s.setup_ms); ir.push_back(s.ir_ms); analysis.push_back(s.analysis_ms);
        opt.push_back(s.optimization_ms); codegen.push_back(s.codegen_ms); cache.push_back(s.cache_lookup_ms);
        compile.push_back(s.compile_ms); load.push_back(s.load_ms); prep.push_back(s.preparation_ms);
        rates_steps.push_back(s.elapsed_ms > 0 ? static_cast<double>(s.steps) / (s.elapsed_ms / 1000.0) : 0);
        rates_ops.push_back(s.elapsed_ms > 0 ? static_cast<double>(s.operations) / (s.elapsed_ms / 1000.0) : 0);
        rates_events.push_back(s.elapsed_ms > 0 ? static_cast<double>(s.events) / (s.elapsed_ms / 1000.0) : 0);
        out.steps = s.steps; out.operations = s.operations; out.events = s.events; out.checksum = s.checksum;
        out.requested_target_ms = s.requested_target_ms;
        out.calibration_iterations = s.calibration_iterations;
        out.duration_target_reached = out.duration_target_reached && s.duration_target_reached;
        out.accepted = out.accepted && s.accepted;
        out.feedback_initial_state = s.feedback_initial_state;
        out.feedback_final_state = s.feedback_final_state;
        out.feedback_state_changes += s.feedback_state_changes;
        out.converged = out.converged && s.converged;
        all_cache_hits = all_cache_hits && s.cache_hit;
        auto& profile = out.profile;
        const auto& sample_profile = s.profile;
        profile.dense_operations += sample_profile.dense_operations;
        profile.event_operations += sample_profile.event_operations;
        profile.events_enqueued += sample_profile.events_enqueued;
        profile.events_processed += sample_profile.events_processed;
        profile.duplicate_schedule_attempts += sample_profile.duplicate_schedule_attempts;
        profile.dependent_schedule_attempts += sample_profile.dependent_schedule_attempts;
        profile.signal_writes += sample_profile.signal_writes;
        profile.signal_changes += sample_profile.signal_changes;
        profile.unchanged_writes += sample_profile.unchanged_writes;
        profile.settle_calls += sample_profile.settle_calls;
        profile.settle_call_count += sample_profile.settle_call_count;
        profile.settle_iteration_count += sample_profile.settle_iteration_count;
        profile.maximum_settle_iterations = std::max(profile.maximum_settle_iterations, sample_profile.maximum_settle_iterations);
        profile.oscillation_limit_hits += sample_profile.oscillation_limit_hits;
        profile.queue_enqueue_time_ns += sample_profile.queue_enqueue_time_ns;
        profile.queue_dequeue_time_ns += sample_profile.queue_dequeue_time_ns;
        profile.queue_enqueue_count += sample_profile.queue_enqueue_count;
        profile.queue_dequeue_count += sample_profile.queue_dequeue_count;
        profile.fanout_traversal_time_ns += sample_profile.fanout_traversal_time_ns;
        profile.fanout_edges_visited += sample_profile.fanout_edges_visited;
        profile.dependent_operations_found += sample_profile.dependent_operations_found;
        profile.scheduling_time_ns += sample_profile.scheduling_time_ns;
        profile.schedule_attempts += sample_profile.schedule_attempts;
        profile.scheduled_operations += sample_profile.scheduled_operations;
        profile.signal_update_time_ns += sample_profile.signal_update_time_ns;
        profile.signal_write_count += sample_profile.signal_write_count;
        profile.signal_change_count += sample_profile.signal_change_count;
        profile.unchanged_signal_write_count += sample_profile.unchanged_signal_write_count;
        profile.operation_evaluation_time_ns += sample_profile.operation_evaluation_time_ns;
        profile.operation_evaluation_count += sample_profile.operation_evaluation_count;
        profile.maximum_queue_length = std::max(profile.maximum_queue_length, sample_profile.maximum_queue_length);
        profile.queue_max_length = std::max(profile.queue_max_length, sample_profile.queue_max_length);
        profile.queue_time_ns += sample_profile.queue_time_ns;
        profile.evaluation_time_ns += sample_profile.evaluation_time_ns;
        out.rss_after_setup_kb = s.rss_after_setup_kb;
        out.rss_after_run_kb = s.rss_after_run_kb;
        out.generated_source_bytes = s.generated_source_bytes;
        out.shared_library_bytes = s.shared_library_bytes;
        out.ir_operations = s.ir_operations;
        out.optimized_ir_operations = s.optimized_ir_operations;
        out.constants_folded = s.constants_folded;
        out.constants_propagated = s.constants_propagated;
        out.dead_operations_removed = s.dead_operations_removed;
        out.common_expressions_eliminated = s.common_expressions_eliminated;
        out.redundant_operations_removed = s.redundant_operations_removed;
        out.fused_operations = s.fused_operations;
        out.signals = s.signals;
        out.observable_signals = s.observable_signals;
        out.observable_operation_outputs = s.observable_operation_outputs;
        out.forwarding_operations_introduced = s.forwarding_operations_introduced;
        out.dependency_edges = s.dependency_edges;
        out.scc_count = s.scc_count;
        out.cyclic_scc_count = s.cyclic_scc_count;
        out.sequential_operations = s.sequential_operations;
        out.maximum_fanout = s.maximum_fanout;
        out.regions = s.regions;
        out.critical_path_length = s.critical_path_length;
        out.opcode_counts = s.opcode_counts;
    }
    out.cache_hit = all_cache_hits;
    out.median_ms = median(times); out.min_ms = *std::min_element(times.begin(), times.end()); out.max_ms = *std::max_element(times.begin(), times.end());
    out.mean_ms = mean(times); out.stddev_ms = stddev(times, out.mean_ms); out.p95_ms = percentile95(times);
    out.steps_per_second = median(rates_steps); out.operations_per_second = median(rates_ops); out.events_per_second = median(rates_events);
    out.setup_ms = median(setups); out.ir_ms = median(ir); out.analysis_ms = median(analysis); out.optimization_ms = median(opt);
    out.codegen_ms = median(codegen); out.cache_lookup_ms = median(cache); out.compile_ms = median(compile); out.load_ms = median(load); out.preparation_ms = median(prep);
    return out;
}

void write_summary_csv(const std::filesystem::path& path, const std::vector<Summary>& rows) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write summary CSV: " + path.string());
    out << "scenario,backend,warmup,runs,iterations,steps,operations,events,checksum,cache_hit,requested_target_ms,calibration_iterations,duration_target_reached,accepted,feedback_initial_state,feedback_final_state,feedback_state_changes,converged,median_ms,min_ms,max_ms,mean_ms,stddev_ms,p95_ms,steps_per_second,operations_per_second,events_per_second,setup_ms,ir_generation_ms,dependency_analysis_ms,optimization_ms,code_generation_ms,cache_lookup_ms,native_compile_ms,library_load_ms,total_preparation_ms,dense_operations,event_operations,events_enqueued,events_processed,duplicate_schedule_attempts,dependent_schedule_attempts,signal_writes,signal_changes,unchanged_writes,settle_calls,settle_call_count,settle_iteration_count,maximum_settle_iterations,oscillation_limit_hits,queue_enqueue_time_ns,queue_dequeue_time_ns,queue_enqueue_count,queue_dequeue_count,queue_max_length,fanout_traversal_time_ns,fanout_edges_visited,dependent_operations_found,scheduling_time_ns,schedule_attempts,scheduled_operations,signal_update_time_ns,signal_write_count,signal_change_count,unchanged_signal_write_count,operation_evaluation_time_ns,operation_evaluation_count,queue_time_ns,evaluation_time_ns,maximum_queue_length,break_even_steps_vs_interpreted_event,break_even_steps_vs_optimized_ir_event,rss_after_setup_kb,rss_after_run_kb,generated_source_bytes,shared_library_bytes,circuit_construction_ms,requested_size,ir_operations,signals,dependency_edges,scc_count,cyclic_scc_count,sequential_operations,maximum_fanout,regions,critical_path_length,opcode_counts,observable_signals,observable_operation_outputs,forwarding_operations_introduced,optimized_ir_operations,constants_folded,constants_propagated,dead_operations_removed,common_expressions_eliminated,redundant_operations_removed,fused_operations,operation_reduction_percent\n";
    out << std::setprecision(12);
    for (const auto& r : rows) {
        out << r.scenario << ',' << r.backend << ',' << r.warmup << ',' << r.runs << ',' << r.iterations << ','
            << r.steps << ',' << r.operations << ',' << r.events << ',' << r.checksum << ','
            << (r.cache_hit ? "true" : "false") << ',' << r.requested_target_ms << ',' << r.calibration_iterations << ','
            << (r.duration_target_reached ? "true" : "false") << ',' << (r.accepted ? "true" : "false") << ','
            << r.feedback_initial_state << ',' << r.feedback_final_state << ',' << r.feedback_state_changes << ','
            << (r.converged ? "true" : "false") << ',' << r.median_ms << ',' << r.min_ms << ',' << r.max_ms << ','
            << r.mean_ms << ',' << r.stddev_ms << ',' << r.p95_ms << ',' << r.steps_per_second << ','
            << r.operations_per_second << ',' << r.events_per_second << ',' << r.setup_ms << ',' << r.ir_ms << ','
            << r.analysis_ms << ',' << r.optimization_ms << ',' << r.codegen_ms << ',' << r.cache_lookup_ms << ','
            << r.compile_ms << ',' << r.load_ms << ',' << r.preparation_ms << ',' << r.profile.dense_operations << ','
            << r.profile.event_operations << ',' << r.profile.events_enqueued << ',' << r.profile.events_processed << ','
            << r.profile.duplicate_schedule_attempts << ',' << r.profile.dependent_schedule_attempts << ','
            << r.profile.signal_writes << ',' << r.profile.signal_changes << ',' << r.profile.unchanged_writes << ','
            << r.profile.settle_calls << ',' << r.profile.settle_call_count << ',' << r.profile.settle_iteration_count << ','
            << r.profile.maximum_settle_iterations << ',' << r.profile.oscillation_limit_hits << ','
            << r.profile.queue_enqueue_time_ns << ',' << r.profile.queue_dequeue_time_ns << ','
            << r.profile.queue_enqueue_count << ',' << r.profile.queue_dequeue_count << ',' << r.profile.queue_max_length << ','
            << r.profile.fanout_traversal_time_ns << ',' << r.profile.fanout_edges_visited << ','
            << r.profile.dependent_operations_found << ',' << r.profile.scheduling_time_ns << ','
            << r.profile.schedule_attempts << ',' << r.profile.scheduled_operations << ','
            << r.profile.signal_update_time_ns << ',' << r.profile.signal_write_count << ','
            << r.profile.signal_change_count << ',' << r.profile.unchanged_signal_write_count << ','
            << r.profile.operation_evaluation_time_ns << ',' << r.profile.operation_evaluation_count << ','
            << r.profile.queue_time_ns << ',' << r.profile.evaluation_time_ns << ','
            << r.profile.maximum_queue_length << ','
            << (r.break_even_vs_event >= 0.0 ? std::to_string(r.break_even_vs_event) : std::string{}) << ','
            << (r.break_even_vs_ir_event >= 0.0 ? std::to_string(r.break_even_vs_ir_event) : std::string{}) << ','
            << r.rss_after_setup_kb << ',' << r.rss_after_run_kb << ',' << r.generated_source_bytes << ','
            << r.shared_library_bytes << ',' << r.circuit_construction_ms << ',' << r.requested_size << ','
            << r.ir_operations << ',' << r.signals << ',' << r.dependency_edges << ',' << r.scc_count << ','
            << r.cyclic_scc_count << ',' << r.sequential_operations << ',' << r.maximum_fanout << ',' << r.regions << ',' << r.critical_path_length << ",\"";
        for (std::size_t i = 0; i < r.opcode_counts.size(); ++i) {
            if (i) out << ';';
            out << r.opcode_counts[i];
        }
        const auto reduction = r.ir_operations > 0
            ? 100.0 * (static_cast<double>(r.ir_operations) - static_cast<double>(r.optimized_ir_operations)) / static_cast<double>(r.ir_operations)
            : 0.0;
        out << "\"," << r.observable_signals << ',' << r.observable_operation_outputs << ',' << r.forwarding_operations_introduced << ',' << r.optimized_ir_operations << ',' << r.constants_folded << ',' << r.constants_propagated << ','
            << r.dead_operations_removed << ',' << r.common_expressions_eliminated << ','
            << r.redundant_operations_removed << ',' << r.fused_operations << ',' << reduction << '\n';
    }
}

void write_raw_csv(const std::filesystem::path& path, const std::vector<std::pair<Summary, std::vector<Sample>>>& data) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write raw CSV: " + path.string());
    out << "scenario,backend,sample,elapsed_ms,setup_ms,steps,operations,events,checksum,cache_hit,requested_target_ms,calibration_iterations,duration_target_reached,accepted,feedback_initial_state,feedback_final_state,feedback_state_changes,converged,ir_generation_ms,dependency_analysis_ms,optimization_ms,code_generation_ms,cache_lookup_ms,native_compile_ms,library_load_ms,total_preparation_ms,dense_operations,event_operations,events_enqueued,events_processed,duplicate_schedule_attempts,dependent_schedule_attempts,signal_writes,signal_changes,unchanged_writes,settle_calls,settle_call_count,settle_iteration_count,maximum_settle_iterations,oscillation_limit_hits,queue_enqueue_time_ns,queue_dequeue_time_ns,queue_enqueue_count,queue_dequeue_count,queue_max_length,fanout_traversal_time_ns,fanout_edges_visited,dependent_operations_found,scheduling_time_ns,schedule_attempts,scheduled_operations,signal_update_time_ns,signal_write_count,signal_change_count,unchanged_signal_write_count,operation_evaluation_time_ns,operation_evaluation_count,queue_time_ns,evaluation_time_ns,maximum_queue_length,break_even_steps_vs_interpreted_event,break_even_steps_vs_optimized_ir_event,rss_after_setup_kb,rss_after_run_kb,generated_source_bytes,shared_library_bytes,circuit_construction_ms,requested_size,ir_operations,signals,dependency_edges,scc_count,cyclic_scc_count,sequential_operations,maximum_fanout,regions,critical_path_length,opcode_counts,observable_signals,observable_operation_outputs,forwarding_operations_introduced,optimized_ir_operations,constants_folded,constants_propagated,dead_operations_removed,common_expressions_eliminated,redundant_operations_removed,fused_operations,operation_reduction_percent\n";
    out << std::setprecision(12);
    for (const auto& [summary, samples] : data) {
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto& s = samples[i];
            out << summary.scenario << ',' << summary.backend << ',' << i << ',' << s.elapsed_ms << ',' << s.setup_ms << ','
                << s.steps << ',' << s.operations << ',' << s.events << ',' << s.checksum << ','
                << (s.cache_hit ? "true" : "false") << ',' << s.requested_target_ms << ',' << s.calibration_iterations << ','
                << (s.duration_target_reached ? "true" : "false") << ',' << (s.accepted ? "true" : "false") << ','
                << s.feedback_initial_state << ',' << s.feedback_final_state << ',' << s.feedback_state_changes << ','
                << (s.converged ? "true" : "false") << ',' << s.ir_ms << ',' << s.analysis_ms << ','
                << s.optimization_ms << ',' << s.codegen_ms << ',' << s.cache_lookup_ms << ',' << s.compile_ms << ','
                << s.load_ms << ',' << s.preparation_ms << ',' << s.profile.dense_operations << ','
                << s.profile.event_operations << ',' << s.profile.events_enqueued << ',' << s.profile.events_processed << ','
                << s.profile.duplicate_schedule_attempts << ',' << s.profile.dependent_schedule_attempts << ','
                << s.profile.signal_writes << ',' << s.profile.signal_changes << ',' << s.profile.unchanged_writes << ','
                << s.profile.settle_calls << ',' << s.profile.settle_call_count << ',' << s.profile.settle_iteration_count << ','
                << s.profile.maximum_settle_iterations << ',' << s.profile.oscillation_limit_hits << ','
                << s.profile.queue_enqueue_time_ns << ',' << s.profile.queue_dequeue_time_ns << ','
                << s.profile.queue_enqueue_count << ',' << s.profile.queue_dequeue_count << ',' << s.profile.queue_max_length << ','
                << s.profile.fanout_traversal_time_ns << ',' << s.profile.fanout_edges_visited << ','
                << s.profile.dependent_operations_found << ',' << s.profile.scheduling_time_ns << ','
                << s.profile.schedule_attempts << ',' << s.profile.scheduled_operations << ','
                << s.profile.signal_update_time_ns << ',' << s.profile.signal_write_count << ','
                << s.profile.signal_change_count << ',' << s.profile.unchanged_signal_write_count << ','
                << s.profile.operation_evaluation_time_ns << ',' << s.profile.operation_evaluation_count << ','
                << s.profile.queue_time_ns << ',' << s.profile.evaluation_time_ns << ','
                << s.profile.maximum_queue_length << ','
                << (summary.break_even_vs_event >= 0.0 ? std::to_string(summary.break_even_vs_event) : std::string{}) << ','
                << (summary.break_even_vs_ir_event >= 0.0 ? std::to_string(summary.break_even_vs_ir_event) : std::string{}) << ','
                << s.rss_after_setup_kb << ',' << s.rss_after_run_kb << ',' << s.generated_source_bytes << ','
                << s.shared_library_bytes << ',' << summary.circuit_construction_ms << ',' << summary.requested_size << ','
                << s.ir_operations << ',' << s.signals << ',' << s.dependency_edges << ',' << s.scc_count << ','
                << s.cyclic_scc_count << ',' << s.sequential_operations << ',' << s.maximum_fanout << ',' << s.regions << ',' << s.critical_path_length << ",\"";
            for (std::size_t op = 0; op < s.opcode_counts.size(); ++op) {
                if (op) out << ';';
                out << s.opcode_counts[op];
            }
            const auto reduction = s.ir_operations > 0
                ? 100.0 * (static_cast<double>(s.ir_operations) - static_cast<double>(s.optimized_ir_operations)) / static_cast<double>(s.ir_operations)
                : 0.0;
            out << "\"," << s.observable_signals << ',' << s.observable_operation_outputs << ',' << s.forwarding_operations_introduced << ',' << s.optimized_ir_operations << ',' << s.constants_folded << ',' << s.constants_propagated << ','
                << s.dead_operations_removed << ',' << s.common_expressions_eliminated << ','
                << s.redundant_operations_removed << ',' << s.fused_operations << ',' << reduction << '\n';
        }
    }
}

void write_json(const std::filesystem::path& path, const std::vector<Summary>& rows) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("Cannot write JSON: " + path.string());
    out << std::setprecision(12) << "[\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        out << "  {\"scenario\":\"" << r.scenario << "\",\"backend\":\"" << r.backend << "\",\"warmup\":" << r.warmup << ",\"runs\":" << r.runs << ",\"iterations\":" << r.iterations << ",\"steps\":" << r.steps << ",\"operations\":" << r.operations << ",\"events\":" << r.events << ",\"checksum\":" << r.checksum << ",\"cache_hit\":" << (r.cache_hit ? "true" : "false") << ",\"requested_target_ms\":" << r.requested_target_ms << ",\"calibration_iterations\":" << r.calibration_iterations << ",\"duration_target_reached\":" << (r.duration_target_reached ? "true" : "false") << ",\"accepted\":" << (r.accepted ? "true" : "false") << ",\"feedback_initial_state\":" << r.feedback_initial_state << ",\"feedback_final_state\":" << r.feedback_final_state << ",\"feedback_state_changes\":" << r.feedback_state_changes << ",\"converged\":" << (r.converged ? "true" : "false") << ",\"median_ms\":" << r.median_ms << ",\"min_ms\":" << r.min_ms << ",\"max_ms\":" << r.max_ms << ",\"mean_ms\":" << r.mean_ms << ",\"stddev_ms\":" << r.stddev_ms << ",\"p95_ms\":" << r.p95_ms << ",\"steps_per_second\":" << r.steps_per_second << ",\"operations_per_second\":" << r.operations_per_second << ",\"events_per_second\":" << r.events_per_second << ",\"setup_ms\":" << r.setup_ms << ",\"ir_generation_ms\":" << r.ir_ms << ",\"dependency_analysis_ms\":" << r.analysis_ms << ",\"optimization_ms\":" << r.optimization_ms << ",\"code_generation_ms\":" << r.codegen_ms << ",\"cache_lookup_ms\":" << r.cache_lookup_ms << ",\"native_compile_ms\":" << r.compile_ms << ",\"library_load_ms\":" << r.load_ms << ",\"total_preparation_ms\":" << r.preparation_ms << ",\"profile\":{\"dense_operations\":" << r.profile.dense_operations << ",\"event_operations\":" << r.profile.event_operations << ",\"events_enqueued\":" << r.profile.events_enqueued << ",\"events_processed\":" << r.profile.events_processed << ",\"duplicate_schedule_attempts\":" << r.profile.duplicate_schedule_attempts << ",\"dependent_schedule_attempts\":" << r.profile.dependent_schedule_attempts << ",\"signal_writes\":" << r.profile.signal_writes << ",\"signal_changes\":" << r.profile.signal_changes << ",\"unchanged_writes\":" << r.profile.unchanged_writes << ",\"settle_calls\":" << r.profile.settle_calls << ",\"queue_time_ns\":" << r.profile.queue_time_ns << ",\"evaluation_time_ns\":" << r.profile.evaluation_time_ns << ",\"settle_call_count\":" << r.profile.settle_call_count << ",\"settle_iteration_count\":" << r.profile.settle_iteration_count << ",\"maximum_settle_iterations\":" << r.profile.maximum_settle_iterations << ",\"oscillation_limit_hits\":" << r.profile.oscillation_limit_hits << ",\"queue_enqueue_time_ns\":" << r.profile.queue_enqueue_time_ns << ",\"queue_dequeue_time_ns\":" << r.profile.queue_dequeue_time_ns << ",\"queue_enqueue_count\":" << r.profile.queue_enqueue_count << ",\"queue_dequeue_count\":" << r.profile.queue_dequeue_count << ",\"fanout_traversal_time_ns\":" << r.profile.fanout_traversal_time_ns << ",\"fanout_edges_visited\":" << r.profile.fanout_edges_visited << ",\"dependent_operations_found\":" << r.profile.dependent_operations_found << ",\"scheduling_time_ns\":" << r.profile.scheduling_time_ns << ",\"schedule_attempts\":" << r.profile.schedule_attempts << ",\"scheduled_operations\":" << r.profile.scheduled_operations << ",\"signal_update_time_ns\":" << r.profile.signal_update_time_ns << ",\"signal_write_count\":" << r.profile.signal_write_count << ",\"signal_change_count\":" << r.profile.signal_change_count << ",\"unchanged_signal_write_count\":" << r.profile.unchanged_signal_write_count << ",\"operation_evaluation_time_ns\":" << r.profile.operation_evaluation_time_ns << ",\"operation_evaluation_count\":" << r.profile.operation_evaluation_count << ",\"queue_max_length\":" << r.profile.queue_max_length << ",\"maximum_queue_length\":" << r.profile.maximum_queue_length << "},\"rss_after_setup_kb\":" << r.rss_after_setup_kb << ",\"rss_after_run_kb\":" << r.rss_after_run_kb << ",\"generated_source_bytes\":" << r.generated_source_bytes << ",\"shared_library_bytes\":" << r.shared_library_bytes << ",\"circuit_construction_ms\":" << r.circuit_construction_ms << ",\"requested_size\":" << r.requested_size << ",\"ir_operations\":" << r.ir_operations << ",\"signals\":" << r.signals << ",\"dependency_edges\":" << r.dependency_edges << ",\"scc_count\":" << r.scc_count << ",\"cyclic_scc_count\":" << r.cyclic_scc_count << ",\"sequential_operations\":" << r.sequential_operations << ",\"maximum_fanout\":" << r.maximum_fanout << ",\"regions\":" << r.regions << ",\"critical_path_length\":" << r.critical_path_length << ",\"observable_signals\":" << r.observable_signals << ",\"observable_operation_outputs\":" << r.observable_operation_outputs << ",\"forwarding_operations_introduced\":" << r.forwarding_operations_introduced << ",\"opcode_counts\":[" << r.opcode_counts[0] << "," << r.opcode_counts[1] << "," << r.opcode_counts[2] << "," << r.opcode_counts[3] << "," << r.opcode_counts[4] << "," << r.opcode_counts[5] << "," << r.opcode_counts[6] << "," << r.opcode_counts[7] << "," << r.opcode_counts[8] << "," << r.opcode_counts[9] << "],\"optimized_ir_operations\":" << r.optimized_ir_operations << ",\"constants_folded\":" << r.constants_folded << ",\"constants_propagated\":" << r.constants_propagated << ",\"dead_operations_removed\":" << r.dead_operations_removed << ",\"common_expressions_eliminated\":" << r.common_expressions_eliminated << ",\"redundant_operations_removed\":" << r.redundant_operations_removed << ",\"fused_operations\":" << r.fused_operations << ",\"operation_reduction_percent\":" << (r.ir_operations > 0 ? 100.0 * (static_cast<double>(r.ir_operations) - static_cast<double>(r.optimized_ir_operations)) / static_cast<double>(r.ir_operations) : 0.0) << ",\"break_even_steps_vs_interpreted_event\":" << (r.break_even_vs_event >= 0.0 ? std::to_string(r.break_even_vs_event) : "null") << ",\"break_even_steps_vs_optimized_ir_event\":" << (r.break_even_vs_ir_event >= 0.0 ? std::to_string(r.break_even_vs_ir_event) : "null") << "}" << (i + 1 == rows.size() ? "\n" : ",\n");
    }
    out << "]\n";
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> std::string { if (i + 1 >= argc) throw std::invalid_argument(std::string("Missing value for ") + name); return argv[++i]; };
        if (arg == "--size") o.size = static_cast<std::uint32_t>(std::stoul(value("--size")));
        else if (arg == "--iterations") o.iterations = std::stoull(value("--iterations"));
        else if (arg == "--runs") o.runs = static_cast<std::uint32_t>(std::stoul(value("--runs")));
        else if (arg == "--cold-runs") o.cold_runs = static_cast<std::uint32_t>(std::stoul(value("--cold-runs")));
        else if (arg == "--warmup") o.warmup = static_cast<std::uint32_t>(std::stoul(value("--warmup")));
        else if (arg == "--scenario") o.scenario = value("--scenario");
        else if (arg == "--cache") o.cache = value("--cache");
        else if (arg == "--output-prefix") o.output_prefix = value("--output-prefix");
        else if (arg == "--compile-mode") o.compile_mode = value("--compile-mode");
        else if (arg == "--profile") o.profile = true;
        else if (arg == "--min-sample-ms") o.min_sample_ms = std::stod(value("--min-sample-ms"));
        else if (arg == "--max-calibration-iterations") o.max_calibration_iterations = std::stoull(value("--max-calibration-iterations"));
        else if (arg == "--native-compiler") o.native_compiler = value("--native-compiler");
        else if (arg == "--optimization-flag") o.optimization_flag = value("--optimization-flag");
        else if (arg == "--help") { std::cout << "hcs_bench [--size N] [--iterations N] [--runs N=20] [--cold-runs N=3] [--warmup N=4] [--scenario all|sparse|counter|SCENARIO] [--cache DIR] [--output-prefix PATH] [--compile-mode both|warm|cold] [--profile] [--min-sample-ms N] [--max-calibration-iterations N] [--native-compiler CXX] [--optimization-flag FLAG]\n"; std::exit(0); }
        else throw std::invalid_argument("Unknown argument: " + arg);
    }
    if (!o.size || !o.iterations || !o.runs || !o.cold_runs || !o.max_calibration_iterations)
        throw std::invalid_argument("size, iterations, runs, cold-runs and max-calibration-iterations must be positive");
    const std::vector<std::string> scenario_names{"small-combinational", "large-combinational", "binary-counter", "large-sequential", "sparse-toggle", "high-switching", "deep-chain", "wide-fanout", "repeated-expressions", "redundant-logic", "feedback-stable", "feedback-dynamic", "independent-regions"};
    if (o.scenario != "all" && o.scenario != "sparse" && o.scenario != "counter" &&
        std::find(scenario_names.begin(), scenario_names.end(), o.scenario) == scenario_names.end())
        throw std::invalid_argument("Unknown benchmark scenario: " + o.scenario);
    if (o.compile_mode != "both" && o.compile_mode != "warm" && o.compile_mode != "cold") throw std::invalid_argument("compile-mode must be both, warm or cold");
    return o;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_args(argc, argv);
        std::vector<std::pair<std::string, BenchCircuit>> scenarios;
        auto add = [&](const std::string& name, auto factory) { scenarios.emplace_back(name, timed_circuit(factory)); };
        {
            add("small-combinational", [] { return make_chain(16); });
            add("large-combinational", [&] { return make_large_combinational(options.size); });
            add("large-sequential", [&] { return make_many_sequential(options.size); });
            add("sparse-toggle", [&] { return make_sparse_circuit(options.size); });
            add("high-switching", [&] { return make_fanout(options.size); });
            add("deep-chain", [&] { return make_chain(options.size); });
            add("wide-fanout", [&] { return make_wide_fanout(options.size); });
            add("repeated-expressions", [&] { return make_repeated(options.size / 2); });
            add("redundant-logic", [&] { return make_redundant(options.size); });
            add("feedback-stable", [&] { return make_feedback_stable(options.size); });
            add("feedback-dynamic", [&] { return make_feedback_dynamic(options.size); });
            add("independent-regions", [&] { return make_independent(options.size); });
        }
        add("binary-counter", [&] { return make_counter(std::min<std::uint32_t>(options.size, 64)); });
        if (options.scenario == "sparse") {
            scenarios.erase(std::remove_if(scenarios.begin(), scenarios.end(), [](const auto& item) { return item.first == "binary-counter"; }), scenarios.end());
        } else if (options.scenario == "counter") {
            scenarios.erase(std::remove_if(scenarios.begin(), scenarios.end(), [](const auto& item) { return item.first != "binary-counter"; }), scenarios.end());
        } else if (options.scenario != "all") {
            scenarios.erase(std::remove_if(scenarios.begin(), scenarios.end(), [&](const auto& item) { return item.first != options.scenario; }), scenarios.end());
            if (scenarios.empty()) throw std::invalid_argument("Unknown benchmark scenario: " + options.scenario);
        }

        std::vector<Summary> summaries;
        std::vector<std::pair<Summary, std::vector<Sample>>> raw;
        for (const auto& [scenario, circuit] : scenarios) {
            auto scenario_options = options;
            std::vector<std::string> backends{"interpreted-event", "interpreted-dense", "optimized-ir-dense", "optimized-ir-event"};
            if (options.compile_mode == "both" || options.compile_mode == "cold") backends.push_back("compiled-native-cold");
            if (options.compile_mode == "both" || options.compile_mode == "warm") backends.push_back("compiled-native-warm");
            scenario_options.iterations = calibrate_iterations(circuit, options, options.cache / (scenario + "-calibration"), backends);
            std::cerr << "[bench] calibrated " << scenario << " to " << scenario_options.iterations << " iterations\n";
            std::vector<std::pair<std::string, std::vector<Sample>>> scenario_samples;
            for (unsigned validation_attempt = 0; validation_attempt < 6; ++validation_attempt) {
                scenario_samples.clear();
                bool all_durations_valid = true;
                double shortest_sample_ms = std::numeric_limits<double>::max();
                for (const auto& backend : backends) {
                    const auto cache = options.cache / (scenario + "-" + backend);
                    if (backend == "compiled-native-warm") {
                        std::filesystem::create_directories(cache);
                        auto prime_config = config_for("compiled-native", cache, options);
                        hcs::Simulator prime(circuit.circuit, prime_config);
                    }
                    auto backend_options = scenario_options;
                    if (backend == "compiled-native-cold") backend_options.runs = options.cold_runs;
                    std::vector<Sample> samples;
                    samples.reserve(backend_options.runs);
                    for (std::uint32_t run = 0; run < backend_options.runs; ++run) {
                        auto sample = run_sample(circuit,
                            backend == "compiled-native-cold" || backend == "compiled-native-warm" ? "compiled-native" : backend,
                            backend_options, cache, backend == "compiled-native-cold");
                        all_durations_valid = all_durations_valid && sample.duration_target_reached;
                        shortest_sample_ms = std::min(shortest_sample_ms, sample.elapsed_ms);
                        samples.push_back(std::move(sample));
                    }
                    scenario_samples.emplace_back(backend, std::move(samples));
                }
                if (all_durations_valid || options.min_sample_ms <= 0.0 ||
                    scenario_options.iterations >= options.max_calibration_iterations || validation_attempt == 5) {
                    if (!all_durations_valid)
                        std::cerr << "[bench] WARNING: duration target not reached for " << scenario
                                  << " at calibration limit; affected samples are marked rejected\n";
                    break;
                }
                const auto scaled = static_cast<long double>(scenario_options.iterations) *
                    (static_cast<long double>(options.min_sample_ms) / std::max(0.001, shortest_sample_ms)) * 1.10L;
                const auto next = static_cast<std::uint64_t>(std::ceil(scaled));
                const auto new_iterations = std::min(options.max_calibration_iterations,
                    std::max(scenario_options.iterations + 1, next));
                if (new_iterations <= scenario_options.iterations) break;
                std::cerr << "[bench] duration validation failed for " << scenario << "; retrying all backends with "
                          << new_iterations << " iterations\n";
                scenario_options.iterations = new_iterations;
            }
            for (auto& [backend, samples] : scenario_samples) {
                auto backend_options = scenario_options;
                if (backend == "compiled-native-cold") backend_options.runs = options.cold_runs;
                auto summary = summarize(scenario, backend, backend_options, samples);
                summary.circuit_construction_ms = circuit.construction_ms;
                summary.requested_size = options.size;
                if (scenario == "feedback-dynamic" || scenario == "feedback-stable") {
                    if (summary.checksum == 0)
                        throw std::runtime_error("Feedback scenario produced a zero checksum: " + scenario + " / " + backend);
                    for (const auto& sample : samples) {
                        if (!sample.converged)
                            throw std::runtime_error("Feedback scenario did not converge: " + scenario + " / " + backend);
                        if (sample.feedback_initial_state != 0)
                            throw std::runtime_error("Feedback scenario did not start from zero: " + scenario + " / " + backend);
                        if (scenario == "feedback-stable" &&
                            (sample.feedback_state_changes != 1 || sample.feedback_final_state != 1))
                            throw std::runtime_error("feedback-stable did not make exactly one 0-to-1 transition: " + backend);
                        if (scenario == "feedback-dynamic" &&
                            (sample.feedback_state_changes != scenario_options.iterations ||
                             sample.feedback_final_state != (scenario_options.iterations & 1u)))
                            throw std::runtime_error("feedback-dynamic transition sequence mismatch: " + backend);
                    }
                }
                if (!summaries.empty()) {
                    const auto reference = std::find_if(summaries.rbegin(), summaries.rend(),
                        [&](const Summary& item) { return item.scenario == scenario && item.backend == "interpreted-event"; });
                    if (reference != summaries.rend() && summary.checksum != reference->checksum)
                        throw std::runtime_error("Backend checksum mismatch in " + scenario + " for " + backend);
                }
                summaries.push_back(summary);
                raw.emplace_back(summary, std::move(samples));
                std::cerr << "[bench] " << scenario << " / " << backend << " median=" << summary.median_ms
                          << " ms duration_target_reached=" << (summary.duration_target_reached ? "true" : "false")
                          << " accepted=" << (summary.accepted ? "true" : "false") << "\n";
            }
            auto find_row = [&](const std::string& backend_name) -> Summary* {
                const auto it = std::find_if(summaries.rbegin(), summaries.rend(), [&](Summary& row) {
                    return row.scenario == scenario && row.backend == backend_name;
                });
                return it == summaries.rend() ? nullptr : &*it;
            };
            auto* cold = find_row("compiled-native-cold");
            auto* event = find_row("interpreted-event");
            auto* ir_event = find_row("optimized-ir-event");
            if (cold && event && cold->steps && event->steps) {
                const auto event_step_ms = event->median_ms / static_cast<double>(event->steps);
                const auto native_step_ms = cold->median_ms / static_cast<double>(cold->steps);
                if (event_step_ms > native_step_ms) cold->break_even_vs_event = cold->preparation_ms / (event_step_ms - native_step_ms);
            }
            if (cold && ir_event && cold->steps && ir_event->steps) {
                const auto event_step_ms = ir_event->median_ms / static_cast<double>(ir_event->steps);
                const auto native_step_ms = cold->median_ms / static_cast<double>(cold->steps);
                if (event_step_ms > native_step_ms) cold->break_even_vs_ir_event = cold->preparation_ms / (event_step_ms - native_step_ms);
            }
        }
        write_summary_csv(options.output_prefix.string() + ".csv", summaries);
        write_raw_csv(options.output_prefix.string() + "-raw.csv", raw);
        write_json(options.output_prefix.string() + ".json", summaries);
        std::cout << "Wrote " << options.output_prefix << ".csv, .json and -raw.csv\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "BENCH ERROR: " << error.what() << '\n';
        return 1;
    }
}
