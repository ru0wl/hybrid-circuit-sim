#include "hcs/simulator.hpp"
#include "hcs/compiler_pipeline.hpp"
#include "hcs/hardware.hpp"

#include <filesystem>
#include <array>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t env_count(const char* name, std::uint32_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 1000000ul) throw std::invalid_argument(std::string("Invalid ") + name);
    return static_cast<std::uint32_t>(parsed);
}

hcs::SimulatorConfig interpreted(hcs::InterpreterStrategy strategy) {
    hcs::SimulatorConfig config;
    config.backend = hcs::Backend::Interpreted;
    config.strategy = strategy;
    return config;
}

hcs::Circuit make_xor_circuit(hcs::SignalId& a, hcs::SignalId& b, hcs::SignalId& out) {
    hcs::CircuitBuilder builder;
    a = builder.add_input();
    b = builder.add_input();
    out = builder.add_binary(hcs::LogicOp::Xor, a, b);
    return builder.build();
}

void test_truth_table() {
    hcs::SignalId a{}, b{}, out{};
    const auto circuit = make_xor_circuit(a, b, out);
    hcs::Simulator sim(circuit, interpreted(hcs::InterpreterStrategy::Dense));
    for (int av = 0; av <= 1; ++av) {
        for (int bv = 0; bv <= 1; ++bv) {
            sim.set_input(a, av != 0);
            sim.set_input(b, bv != 0);
            const auto result = sim.step();
            require(result.converged, "XOR circuit did not settle");
            require(sim.signal(out) == ((av != 0) != (bv != 0)), "XOR truth table mismatch");
        }
    }
}

void test_reverse_order_dependency_chain() {
    constexpr std::uint32_t depth = 256;
    hcs::CircuitBuilder builder;
    const auto input = builder.add_input();
    std::vector<hcs::SignalId> signals(depth + 1);
    signals[0] = input;
    for (std::uint32_t i = 1; i <= depth; ++i) signals[i] = builder.add_signal();
    for (std::uint32_t i = depth; i > 0; --i) {
        builder.add_gate(hcs::LogicOp::Buffer, signals[i - 1], hcs::kInvalidSignal, signals[i]);
    }
    const auto circuit = builder.build();

    auto dense_config = interpreted(hcs::InterpreterStrategy::Dense);
    dense_config.max_dense_passes = 4;
    hcs::Simulator dense(circuit, dense_config);
    dense.set_input(input, true);
    const auto dense_result = dense.step();
    require(dense_result.converged, "Topologically reordered dense chain should settle in one pass");
    require(dense.signal(signals.back()), "Reverse-order dense chain produced the wrong output");

    hcs::SimulatorConfig hybrid_config;
    hybrid_config.backend = hcs::Backend::HybridIR;
    hybrid_config.max_dense_passes = 4;
    hcs::Simulator hybrid(circuit, hybrid_config);
    hybrid.set_input(input, true);
    const auto hybrid_result = hybrid.step();
    require(hybrid_result.converged && hybrid.signal(signals.back()),
            "Hybrid topological scheduling failed on a reverse-order dependency chain");
    for (hcs::SignalId id = 0; id < hybrid.signal_count(); ++id)
        require(hybrid.signal(id) == dense.signal(id), "Hybrid reverse-order chain observability mismatch");

    hcs::SimulatorConfig compiled_config;
    compiled_config.backend = hcs::Backend::Compiled;
    compiled_config.max_dense_passes = 4;
    compiled_config.native_options.cache_directory =
        std::filesystem::temp_directory_path() / "hcs-test-reverse-order-cache";
    std::filesystem::remove_all(compiled_config.native_options.cache_directory);
    hcs::Simulator compiled(circuit, compiled_config);
    compiled.set_input(input, true);
    const auto compiled_result = compiled.step();
    require(compiled_result.converged, "Native reverse-order chain should settle in one pass");
    require(compiled.signal(signals.back()), "Native reverse-order chain produced the wrong output");
    std::filesystem::remove_all(compiled_config.native_options.cache_directory);
}

void test_event_and_dense_equivalence() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto c = builder.add_input();
    const auto x = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto y = builder.add_binary(hcs::LogicOp::And, b, c);
    const auto z = builder.add_binary(hcs::LogicOp::Or, x, y);
    const auto circuit = builder.build();

    hcs::Simulator dense(circuit, interpreted(hcs::InterpreterStrategy::Dense));
    hcs::Simulator events(circuit, interpreted(hcs::InterpreterStrategy::EventDriven));
    hcs::SimulatorConfig ir_config;
    ir_config.backend = hcs::Backend::OptimizedIR;
    hcs::Simulator optimized_ir(circuit, ir_config);
    std::mt19937 rng(0xC1AC017u);
    for (int i = 0; i < 5000; ++i) {
        const bool av = (rng() & 1u) != 0;
        const bool bv = (rng() & 1u) != 0;
        const bool cv = (rng() & 1u) != 0;
        dense.set_input(a, av); events.set_input(a, av); optimized_ir.set_input(a, av);
        dense.set_input(b, bv); events.set_input(b, bv); optimized_ir.set_input(b, bv);
        dense.set_input(c, cv); events.set_input(c, cv); optimized_ir.set_input(c, cv);
        const auto dr = dense.step();
        const auto er = events.step();
        const auto ir_result = optimized_ir.step();
        require(dr.converged && er.converged && ir_result.converged, "Acyclic circuit failed to settle");
        require(dense.signal(z) == events.signal(z), "Dense/event-driven mismatch");
        for (hcs::SignalId signal = 0; signal < dense.signal_count(); ++signal) {
            require(dense.signal(signal) == optimized_ir.signal(signal),
                    "Optimized IR changed an observable signal");
        }
    }
}

void test_dff_counter() {
    hcs::CircuitBuilder builder;
    const auto clock = builder.add_input();
    const auto one = builder.add_constant(true);
    const auto q0 = builder.add_signal();
    const auto q1 = builder.add_signal();
    const auto q2 = builder.add_signal();
    const auto q3 = builder.add_signal();
    const auto d0 = builder.add_unary(hcs::LogicOp::Not, q0);
    const auto carry1 = builder.add_binary(hcs::LogicOp::And, q0, one);
    const auto d1 = builder.add_binary(hcs::LogicOp::Xor, q1, carry1);
    const auto carry2 = builder.add_binary(hcs::LogicOp::And, q1, carry1);
    const auto d2 = builder.add_binary(hcs::LogicOp::Xor, q2, carry2);
    const auto carry3 = builder.add_binary(hcs::LogicOp::And, q2, carry2);
    const auto d3 = builder.add_binary(hcs::LogicOp::Xor, q3, carry3);
    builder.add_d_flip_flop(d0, clock, q0);
    builder.add_d_flip_flop(d1, clock, q1);
    builder.add_d_flip_flop(d2, clock, q2);
    builder.add_d_flip_flop(d3, clock, q3);
    const auto circuit = builder.build();

    hcs::Simulator sim(circuit, interpreted(hcs::InterpreterStrategy::EventDriven));
    for (std::uint32_t value = 1; value <= 16; ++value) {
        sim.set_input(clock, true);
        auto result = sim.step();
        require(result.converged, "Counter failed to settle on rising edge");
        const auto observed = (static_cast<std::uint32_t>(sim.signal(q0)) << 0u) |
                              (static_cast<std::uint32_t>(sim.signal(q1)) << 1u) |
                              (static_cast<std::uint32_t>(sim.signal(q2)) << 2u) |
                              (static_cast<std::uint32_t>(sim.signal(q3)) << 3u);
        require(observed == (value & 0xFu), "DFF counter state mismatch");
        sim.set_input(clock, false);
        (void)sim.step();
    }
}

void test_native_matches_interpreted_and_cache() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto c = builder.add_input();
    const auto x = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto y = builder.add_binary(hcs::LogicOp::Nand, b, c);
    const auto z = builder.add_binary(hcs::LogicOp::Or, x, y);
    const auto circuit = builder.build();

    const auto cache = std::filesystem::temp_directory_path() / "hcs-test-native-cache";
    std::filesystem::remove_all(cache);
    hcs::SimulatorConfig compiled_config;
    compiled_config.backend = hcs::Backend::Compiled;
    compiled_config.native_options.cache_directory = cache;
    hcs::Simulator compiled(circuit, compiled_config);
    require(!compiled.compiled_cache_hit(), "First native load should compile, not hit cache");
    hcs::SimulatorConfig interpreted_config = interpreted(hcs::InterpreterStrategy::Dense);
    hcs::Simulator reference(circuit, interpreted_config);

    std::mt19937 rng(0xBADC0DEu);
    for (int i = 0; i < 3000; ++i) {
        const bool av = (rng() & 1u) != 0;
        const bool bv = (rng() & 1u) != 0;
        const bool cv = (rng() & 1u) != 0;
        compiled.set_input(a, av); reference.set_input(a, av);
        compiled.set_input(b, bv); reference.set_input(b, bv);
        compiled.set_input(c, cv); reference.set_input(c, cv);
        const auto cr = compiled.step();
        const auto rr = reference.step();
        require(cr.converged && rr.converged, "Compiled/reference circuit failed to settle");
        require(compiled.signal(z) == reference.signal(z), "Native/reference output mismatch");
    }

    hcs::Simulator cached(circuit, compiled_config);
    require(cached.compiled_cache_hit(), "Second native load should hit compilation cache");
    std::filesystem::remove_all(cache);
}

void test_compiled_dff_semantics() {
    hcs::CircuitBuilder builder;
    const auto clock = builder.add_input();
    const auto d = builder.add_input();
    const auto q = builder.add_signal();
    builder.add_d_flip_flop(d, clock, q, false);
    const auto circuit = builder.build();

    const auto cache = std::filesystem::temp_directory_path() / "hcs-test-dff-native-cache";
    std::filesystem::remove_all(cache);
    hcs::SimulatorConfig native_config;
    native_config.backend = hcs::Backend::Compiled;
    native_config.native_options.cache_directory = cache;
    hcs::Simulator native(circuit, native_config);
    hcs::Simulator reference(circuit, interpreted(hcs::InterpreterStrategy::Dense));

    for (int i = 0; i < 100; ++i) {
        const bool input = (i & 1) != 0;
        native.set_input(d, input); reference.set_input(d, input);
        native.set_input(clock, true); reference.set_input(clock, true);
        (void)native.step(); (void)reference.step();
        require(native.signal(q) == reference.signal(q), "Compiled DFF state mismatch on rising edge");
        native.set_input(clock, false); reference.set_input(clock, false);
        (void)native.step(); (void)reference.step();
        require(native.signal(q) == reference.signal(q), "Compiled DFF state mismatch on falling edge");
    }
    std::filesystem::remove_all(cache);
}

void test_oscillation_limit() {
    hcs::CircuitBuilder builder;
    const auto loop = builder.add_signal();
    builder.add_gate(hcs::LogicOp::Not, loop, hcs::kInvalidSignal, loop);
    const auto circuit = builder.build();
    auto config = interpreted(hcs::InterpreterStrategy::Dense);
    config.max_dense_passes = 8;
    hcs::Simulator sim(circuit, config);
    const auto result = sim.step();
    require(!result.converged, "Inverter feedback loop should hit the delta-cycle limit");
    hcs::SimulatorConfig ir_config;
    ir_config.backend = hcs::Backend::OptimizedIR;
    ir_config.max_dense_passes = 8;
    hcs::Simulator optimized(circuit, ir_config);
    require(!optimized.step().converged,
            "Optimized IR must preserve non-convergent feedback behavior");
    hcs::SimulatorConfig hybrid_config;
    hybrid_config.backend = hcs::Backend::HybridIR;
    hybrid_config.max_dense_passes = 8;
    hybrid_config.max_events_per_settle = 8;
    hcs::Simulator hybrid(circuit, hybrid_config);
    const auto hybrid_result = hybrid.step();
    require(!hybrid_result.converged && hybrid_result.events == 8,
            "Hybrid IR must preserve event-budget handling for non-convergent feedback");
    auto event_config = interpreted(hcs::InterpreterStrategy::EventDriven);
    event_config.max_events_per_settle = 8;
    event_config.enable_profiling = true;
    hcs::Simulator event_sim(circuit, event_config);
    const auto event_result = event_sim.step();
    require(!event_result.converged && event_result.events == 8,
            "Event-driven inverter feedback must stop at the event limit");
    require(event_sim.execution_profile().oscillation_limit_hits == 1,
            "Event profiler must count the oscillation/event-limit hit");
}


void test_ir_lowering_and_optimization() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto zero = builder.add_constant(false);
    const auto folded = builder.add_binary(hcs::LogicOp::And, zero, a);
    const auto first = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto second = builder.add_binary(hcs::LogicOp::Xor, a, b);
    const auto circuit = builder.build();

    const auto prepared = hcs::prepare_circuit(circuit);
    require(!prepared.dependencies.shared_expressions.empty(),
            "Dependency analysis must identify repeated acyclic expressions");
    require(prepared.original_ir.operations.size() == circuit.gates().size(),
            "IR lowering must preserve operation count");
    require(prepared.optimized_ir.operations.size() == circuit.gates().size(),
            "Optimization must preserve observable signal drivers");
    require(prepared.optimized_ir.operations[2].source_component != UINT32_MAX,
            "IR source mapping must be retained");

    bool saw_folded_constant = false;
    bool saw_cse_buffer = false;
    for (const auto& op : prepared.optimized_ir.operations) {
        if (op.output == folded) saw_folded_constant = op.opcode == hcs::LogicOp::Constant0;
        if (op.output == second) saw_cse_buffer = op.opcode == hcs::LogicOp::Buffer && op.a == first;
    }
    require(saw_folded_constant, "Constant folding must simplify AND(false, x)");
    require(saw_cse_buffer, "CSE must preserve duplicate output through a forwarding operation");

    hcs::CircuitBuilder propagation_builder;
    const auto variable = propagation_builder.add_input();
    const auto repeated = propagation_builder.add_binary(hcs::LogicOp::Xor, variable, variable);
    const auto propagated = propagation_builder.add_binary(hcs::LogicOp::And, variable, repeated);
    const auto propagation_circuit = propagation_builder.build();
    const auto propagation = hcs::prepare_circuit(propagation_circuit);
    bool propagated_constant = false;
    for (const auto& op : propagation.optimized_ir.operations) {
        if (op.output == propagated) propagated_constant = op.opcode == hcs::LogicOp::Constant0;
    }
    require(propagated_constant, "Boolean simplification must feed constant propagation in later passes");
    require(propagation.statistics.constants_propagated > 0,
            "Constant propagation statistics must count propagated operands");
}

void test_dependency_scc_analysis() {
    hcs::CircuitBuilder builder;
    const auto loop = builder.add_signal();
    builder.add_gate(hcs::LogicOp::Not, loop, hcs::kInvalidSignal, loop);
    const auto circuit = builder.build();
    const auto prepared = hcs::prepare_circuit(circuit);
    require(prepared.dependencies.strongly_connected_components.size() == 1,
            "Self-feedback must form one strongly connected component");
    require(prepared.dependencies.cyclic_operation.size() == 1 &&
            prepared.dependencies.cyclic_operation[0],
            "Self-feedback operation must be marked cyclic");
    require(prepared.dependencies.regions.size() == 1 &&
            prepared.dependencies.regions[0].kind == hcs::analysis::RegionKind::FeedbackCombinational,
            "Feedback region must be identified");
}

void test_optimized_ir_backend() {
    hcs::SignalId a{}, b{}, out{};
    const auto circuit = make_xor_circuit(a, b, out);
    hcs::Simulator reference(circuit, interpreted(hcs::InterpreterStrategy::Dense));
    hcs::SimulatorConfig optimized_config;
    optimized_config.backend = hcs::Backend::OptimizedIR;
    hcs::Simulator optimized(circuit, optimized_config);
    for (int i = 0; i < 64; ++i) {
        const bool av = (i & 1) != 0;
        const bool bv = (i & 2) != 0;
        reference.set_input(a, av); optimized.set_input(a, av);
        reference.set_input(b, bv); optimized.set_input(b, bv);
        (void)reference.step();
        (void)optimized.step();
        require(reference.signal(out) == optimized.signal(out),
                "Optimized IR interpreter output differs from reference");
        require(reference.signal(a) == optimized.signal(a) && reference.signal(b) == optimized.signal(b),
                "Optimized IR interpreter changed observable input values");
    }
}

void test_optimized_ir_dff() {
    hcs::CircuitBuilder builder;
    const auto d = builder.add_input();
    const auto clock = builder.add_input();
    const auto q = builder.add_signal();
    builder.add_d_flip_flop(d, clock, q, false);
    const auto circuit = builder.build();
    hcs::Simulator reference(circuit, interpreted(hcs::InterpreterStrategy::Dense));
    hcs::SimulatorConfig config;
    config.backend = hcs::Backend::OptimizedIR;
    hcs::Simulator optimized(circuit, config);
    for (int i = 0; i < 20; ++i) {
        const bool data = (i & 1) != 0;
        reference.set_input(d, data); optimized.set_input(d, data);
        reference.set_input(clock, true); optimized.set_input(clock, true);
        (void)reference.step(); (void)optimized.step();
        require(reference.signal(q) == optimized.signal(q), "Optimized IR DFF rising-edge mismatch");
        reference.set_input(clock, false); optimized.set_input(clock, false);
        (void)reference.step(); (void)optimized.step();
    }
}


void test_all_logic_operations() {
    const std::vector<std::uint8_t> signals{0, 1};
    for (const auto op : {hcs::LogicOp::Constant0, hcs::LogicOp::Constant1}) {
        const hcs::Gate gate{op, hcs::kInvalidSignal, hcs::kInvalidSignal, 0};
        require(hcs::evaluate_gate(gate, signals) == (op == hcs::LogicOp::Constant1),
                "Constant operation truth table failed");
    }
    for (const auto op : {hcs::LogicOp::Buffer, hcs::LogicOp::Not}) {
        for (std::uint8_t a = 0; a <= 1; ++a) {
            const hcs::Gate gate{op, a, hcs::kInvalidSignal, 0};
            const bool expected = op == hcs::LogicOp::Buffer ? a != 0 : a == 0;
            require(hcs::evaluate_gate(gate, signals) == expected,
                    "Unary operation truth table failed");
        }
    }
    for (const auto op : {hcs::LogicOp::And, hcs::LogicOp::Or, hcs::LogicOp::Xor,
                          hcs::LogicOp::Nand, hcs::LogicOp::Nor, hcs::LogicOp::Xnor}) {
        for (std::uint8_t a = 0; a <= 1; ++a) {
            for (std::uint8_t b = 0; b <= 1; ++b) {
                const hcs::Gate gate{op, a, b, 0};
                bool expected = false;
                switch (op) {
                    case hcs::LogicOp::And: expected = a && b; break;
                    case hcs::LogicOp::Or: expected = a || b; break;
                    case hcs::LogicOp::Xor: expected = (a != 0) != (b != 0); break;
                    case hcs::LogicOp::Nand: expected = !(a && b); break;
                    case hcs::LogicOp::Nor: expected = !(a || b); break;
                    case hcs::LogicOp::Xnor: expected = (a != 0) == (b != 0); break;
                    default: break;
                }
                require(hcs::evaluate_gate(gate, signals) == expected,
                        "Binary operation truth table failed");
            }
        }
    }
}

void test_optimizer_pass_configuration() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    const auto first = builder.add_binary(hcs::LogicOp::And, a, b);
    const auto second = builder.add_binary(hcs::LogicOp::And, a, b);
    const auto circuit = builder.build();
    auto ir = hcs::ir::lower(circuit);
    const auto graph = hcs::analysis::analyze(ir);
    hcs::optimizer::Options options{{{hcs::optimizer::PassKind::CommonSubexpressionElimination, false}}};
    const auto disabled = hcs::optimizer::run(ir, graph, options);
    require(disabled.circuit.operations.back().opcode == hcs::LogicOp::And,
            "Disabled CSE pass must not rewrite operations");
    options.passes[0].enabled = true;
    const auto enabled = hcs::optimizer::run(ir, graph, options);
    require(enabled.circuit.operations.back().opcode == hcs::LogicOp::Buffer &&
            enabled.circuit.operations.back().a == first && second == enabled.circuit.operations.back().output,
            "Enabled CSE pass must forward duplicate expression");

    hcs::CircuitBuilder redundant_builder;
    const auto input = redundant_builder.add_input();
    const auto inverted = redundant_builder.add_unary(hcs::LogicOp::Not, input);
    const auto restored = redundant_builder.add_unary(hcs::LogicOp::Not, inverted);
    const auto redundant_circuit = redundant_builder.build();
    auto redundant_ir = hcs::ir::lower(redundant_circuit);
    const auto redundant_graph = hcs::analysis::analyze(redundant_ir);
    hcs::optimizer::Options redundant_options{{
        {hcs::optimizer::PassKind::RedundantOperationElimination, true}}};
    const auto redundant = hcs::optimizer::run(redundant_ir, redundant_graph, redundant_options);
    require(redundant.circuit.operations.back().opcode == hcs::LogicOp::Buffer &&
            redundant.circuit.operations.back().a == input,
            "Redundant double inversion must be replaced with a forwarding operation");

    hcs::CircuitBuilder dead_builder;
    const auto live_input = dead_builder.add_input();
    const auto dead_input = dead_builder.add_input();
    const auto dead_signal = dead_builder.add_unary(hcs::LogicOp::Not, dead_input);
    const auto live_output = dead_builder.add_unary(hcs::LogicOp::Buffer, live_input);
    const auto dead_circuit = dead_builder.build();
    auto dead_ir = hcs::ir::lower(dead_circuit);
    dead_ir.observable_signals = {live_output};
    const auto dead_graph = hcs::analysis::analyze(dead_ir);
    require(dead_graph.reachable_operation.size() == 2 &&
            !dead_graph.reachable_operation[0] && dead_graph.reachable_operation[1],
            "Dependency analysis must identify unreachable logic from the observable output set");
    require(dead_graph.unused_outputs.size() == 1 && dead_graph.unused_outputs[0] == dead_signal,
            "Dependency analysis must identify an unused output signal");
    hcs::optimizer::Options dead_options{{{hcs::optimizer::PassKind::DeadLogicElimination, true}}};
    const auto dead_result = hcs::optimizer::run(dead_ir, dead_graph, dead_options);
    require(dead_result.dead_operations_removed == 1 &&
            dead_result.circuit.operations.size() == 1 &&
            dead_result.circuit.operations.front().output == live_output,
            "DCE must remove unobservable logic when the IR output contract permits it");
    require(dead_signal != live_output, "Dead/live fixture must use distinct signals");
}


void test_native_compiler_timings() {
    hcs::SignalId a{}, b{}, out{};
    const auto circuit = make_xor_circuit(a, b, out);
    hcs::NativeCompileOptions options;
    options.cache_directory = std::filesystem::temp_directory_path() / "hcs-test-timing-cache";
    std::filesystem::remove_all(options.cache_directory);
    const auto cold = hcs::NativeCompiler::load_or_compile(circuit, options);
    require(!cold->cache_hit(), "First native load should be a cache miss");
    require(cold->timings().compiler_invocation_ms > 0.0,
            "Cold native compile timing must be recorded");
    require(cold->timings().library_load_ms >= 0.0 && cold->timings().total_preparation_ms > 0.0,
            "Native load and total preparation timings must be recorded");
    require(cold->timings().generated_source_bytes > 0 && cold->timings().shared_library_bytes > 0,
            "Native source and shared-library sizes must be recorded");
    require(cold->source_map().size() == circuit.gates().size(),
            "Compiled module must retain its IR-to-source component map");
    require(cold->compiler_statistics().initial_ir_operations == circuit.gates().size() &&
            cold->compiler_statistics().final_ir_operations == circuit.gates().size(),
            "Compiled module must retain compiler preparation statistics");
    const auto warm = hcs::NativeCompiler::load_or_compile(circuit, options);
    require(warm->cache_hit(), "Second native load should validate the cache");
    require(warm->timings().cache_lookup_ms > 0.0,
            "Warm cache lookup timing must be recorded");
    std::filesystem::remove_all(options.cache_directory);
}


void test_independent_region_grouping() {
    hcs::CircuitBuilder builder;
    const auto a = builder.add_input();
    const auto b = builder.add_input();
    (void)builder.add_unary(hcs::LogicOp::Not, a);
    (void)builder.add_unary(hcs::LogicOp::Buffer, b);
    const auto circuit = builder.build();
    const auto prepared = hcs::prepare_circuit(circuit);
    require(prepared.dependencies.regions.size() == 2,
            "Independent combinational components must be separate execution regions");
    require(prepared.optimized_ir.operations[0].region != prepared.optimized_ir.operations[1].region,
            "IR operations must retain their assigned region IDs");
}


void test_optimized_ir_event_equivalence_and_randomized() {
    constexpr std::uint32_t seed = 0x51A1E7u;
    std::mt19937 rng(seed);
    const auto circuit_count = env_count("HCS_RANDOM_CIRCUITS", 40);
    for (std::uint32_t circuit_index = 0; circuit_index < circuit_count; ++circuit_index) {
        hcs::CircuitBuilder builder;
        std::vector<hcs::SignalId> values;
        std::vector<hcs::SignalId> inputs;
        for (int i = 0; i < 5; ++i) { inputs.push_back(builder.add_input()); values.push_back(inputs.back()); }
        const auto zero = builder.add_constant(false);
        const auto one = builder.add_constant(true);
        values.push_back(zero); values.push_back(one);
        const hcs::LogicOp ops[] = {hcs::LogicOp::Buffer, hcs::LogicOp::Not, hcs::LogicOp::And,
                                    hcs::LogicOp::Or, hcs::LogicOp::Xor, hcs::LogicOp::Nand,
                                    hcs::LogicOp::Nor, hcs::LogicOp::Xnor};
        for (int i = 0; i < 32; ++i) {
            const auto op = ops[rng() % (sizeof(ops)/sizeof(ops[0]))];
            const auto a = values[rng() % values.size()];
            if (op == hcs::LogicOp::Buffer || op == hcs::LogicOp::Not)
                values.push_back(builder.add_unary(op, a));
            else {
                const auto b = values[rng() % values.size()];
                values.push_back(builder.add_binary(op, a, b));
            }
        }
        const auto circuit = builder.build();
        auto ref_cfg = interpreted(hcs::InterpreterStrategy::EventDriven);
        hcs::Simulator reference(circuit, ref_cfg);
        hcs::SimulatorConfig dense_cfg; dense_cfg.backend = hcs::Backend::OptimizedIR;
        dense_cfg.strategy = hcs::InterpreterStrategy::Dense;
        hcs::Simulator ir_dense(circuit, dense_cfg);
        hcs::SimulatorConfig event_cfg = dense_cfg;
        event_cfg.strategy = hcs::InterpreterStrategy::EventDriven;
        hcs::Simulator ir_event(circuit, event_cfg);
        hcs::SimulatorConfig hybrid_cfg;
        hybrid_cfg.backend = hcs::Backend::HybridIR;
        hcs::Simulator ir_hybrid(circuit, hybrid_cfg);
        for (int step = 0; step < 100; ++step) {
            for (const auto input : inputs) {
                const bool value = (rng() & 1u) != 0;
                reference.set_input(input, value);
                ir_dense.set_input(input, value);
                ir_event.set_input(input, value);
                ir_hybrid.set_input(input, value);
            }
            const auto rr = reference.step();
            const auto dr = ir_dense.step();
            const auto er = ir_event.step();
            const auto hr = ir_hybrid.step();
            require(rr.converged && dr.converged && er.converged && hr.converged,
                    "Random differential circuit did not converge; seed=" + std::to_string(seed));
            for (hcs::SignalId signal = 0; signal < reference.signal_count(); ++signal) {
                require(reference.signal(signal) == ir_dense.signal(signal) &&
                        reference.signal(signal) == ir_event.signal(signal) &&
                        reference.signal(signal) == ir_hybrid.signal(signal),
                        "Random differential mismatch; seed=" + std::to_string(seed) +
                        " circuit=" + std::to_string(circuit_index) +
                        " step=" + std::to_string(step) +
                        " signal=" + std::to_string(signal));
            }
        }
    }
}



void test_randomized_sequential_differential() {
    constexpr std::uint64_t seed = 0x8f3a21c5ull;
    std::mt19937_64 rng(seed);
    const auto circuit_count = env_count("HCS_RANDOM_SEQUENTIAL_CASES", 4);
    const auto step_count = env_count("HCS_RANDOM_SEQUENTIAL_STEPS", 128);
    const auto cache_dir = std::filesystem::temp_directory_path() / "hcs-random-sequential-test-cache";
    std::filesystem::remove_all(cache_dir);

    for (std::uint32_t circuit_index = 0; circuit_index < circuit_count; ++circuit_index) {
        hcs::CircuitBuilder builder;
        const auto clock = builder.add_input();
        std::vector<hcs::SignalId> data_inputs;
        for (int i = 0; i < 4; ++i) data_inputs.push_back(builder.add_input());
        std::vector<hcs::SignalId> q(12);
        for (auto& signal : q) signal = builder.add_signal();
        for (std::size_t i = 0; i < q.size(); ++i) {
            const auto mixed = builder.add_binary((rng() & 1u) ? hcs::LogicOp::Xor : hcs::LogicOp::And,
                                                   q[(i + q.size() - 1) % q.size()], data_inputs[i % data_inputs.size()]);
            const auto d = builder.add_binary((rng() & 1u) ? hcs::LogicOp::Or : hcs::LogicOp::Xnor,
                                              mixed, data_inputs[(i + 1) % data_inputs.size()]);
            builder.add_d_flip_flop(d, clock, q[i], (rng() & 1u) != 0);
        }
        const auto circuit = builder.build();
        auto ref_cfg = interpreted(hcs::InterpreterStrategy::EventDriven);
        hcs::Simulator reference(circuit, ref_cfg);
        hcs::SimulatorConfig dense_cfg;
        dense_cfg.backend = hcs::Backend::OptimizedIR;
        dense_cfg.strategy = hcs::InterpreterStrategy::Dense;
        hcs::Simulator ir_dense(circuit, dense_cfg);
        auto event_cfg = dense_cfg;
        event_cfg.strategy = hcs::InterpreterStrategy::EventDriven;
        hcs::Simulator ir_event(circuit, event_cfg);
        hcs::SimulatorConfig hybrid_cfg;
        hybrid_cfg.backend = hcs::Backend::HybridIR;
        hcs::Simulator ir_hybrid(circuit, hybrid_cfg);
        hcs::SimulatorConfig native_cfg;
        native_cfg.backend = hcs::Backend::Compiled;
        native_cfg.native_options.cache_directory = cache_dir;
        hcs::Simulator native(circuit, native_cfg);

        for (std::uint32_t step = 0; step < step_count; ++step) {
            for (const auto input : data_inputs) {
                const bool value = (rng() & 1u) != 0;
                reference.set_input(input, value);
                ir_dense.set_input(input, value);
                ir_event.set_input(input, value);
                ir_hybrid.set_input(input, value);
                native.set_input(input, value);
            }
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &ir_hybrid, &native}) simulator->set_input(clock, true);
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &ir_hybrid, &native}) {
                const auto result = simulator->step();
                require(result.converged, "Random sequential circuit did not converge; seed=" + std::to_string(seed));
            }
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &ir_hybrid, &native}) simulator->set_input(clock, false);
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &ir_hybrid, &native}) (void)simulator->step();
            for (hcs::SignalId signal = 0; signal < reference.signal_count(); ++signal) {
                const bool expected = reference.signal(signal);
                require(ir_dense.signal(signal) == expected && ir_event.signal(signal) == expected &&
                        ir_hybrid.signal(signal) == expected && native.signal(signal) == expected,
                        "Random sequential differential mismatch; seed=" + std::to_string(seed) +
                        " circuit=" + std::to_string(circuit_index) + " step=" + std::to_string(step) +
                        " signal=" + std::to_string(signal));
            }
        }
    }
    std::filesystem::remove_all(cache_dir);
}

void test_simultaneous_dff_updates_and_feedback_dynamic() {
    {
        hcs::CircuitBuilder builder;
        const auto clock = builder.add_input();
        const auto data = builder.add_input();
        const auto q0 = builder.add_signal();
        const auto q1 = builder.add_signal();
        const auto q2 = builder.add_signal();
        builder.add_d_flip_flop(data, clock, q0, false);
        builder.add_d_flip_flop(q0, clock, q1, false);
        builder.add_d_flip_flop(q1, clock, q2, false);
        const auto circuit = builder.build();
        for (const auto backend : {hcs::Backend::Interpreted, hcs::Backend::OptimizedIR, hcs::Backend::HybridIR, hcs::Backend::Compiled}) {
            hcs::SimulatorConfig config;
            config.backend = backend;
            config.strategy = hcs::InterpreterStrategy::EventDriven;
            config.native_options.cache_directory = std::filesystem::temp_directory_path() / "hcs-dff-scratch-test-cache";
            hcs::Simulator sim(circuit, config);
            sim.set_input(data, true);
            for (int edge = 0; edge < 3; ++edge) {
                sim.set_input(clock, true);
                require(sim.step().converged, "Cascaded DFF circuit did not converge");
                sim.set_input(clock, false);
                require(sim.step().converged, "Cascaded DFF falling edge did not converge");
                require(sim.signal(q0) == (edge >= 0), "First DFF did not capture input");
                require(sim.signal(q1) == (edge >= 1), "Second DFF did not capture previous Q0");
                require(sim.signal(q2) == (edge >= 2), "Third DFF did not capture previous Q1");
            }
        }
    }
    {
        hcs::CircuitBuilder builder;
        const auto set = builder.add_input();
        const auto hold = builder.add_input();
        const auto x = builder.add_signal();
        const auto held = builder.add_binary(hcs::LogicOp::And, x, hold);
        builder.add_gate(hcs::LogicOp::Or, held, set, x);
        const auto circuit = builder.build();
        const auto cache = std::filesystem::temp_directory_path() / "hcs-feedback-dynamic-test-cache";
        for (const auto backend : {hcs::Backend::Interpreted, hcs::Backend::OptimizedIR, hcs::Backend::HybridIR, hcs::Backend::Compiled}) {
            for (const auto strategy : {hcs::InterpreterStrategy::EventDriven, hcs::InterpreterStrategy::Dense}) {
                if (backend == hcs::Backend::Compiled && strategy == hcs::InterpreterStrategy::Dense) continue;
                hcs::SimulatorConfig config;
                config.backend = backend;
                config.strategy = strategy;
                config.native_options.cache_directory = cache;
                hcs::Simulator sim(circuit, config);
                bool expected = false;
                for (int i = 0; i < 32; ++i) {
                    const bool value = (i & 1) == 0;
                    sim.set_input(set, value);
                    sim.set_input(hold, false);
                    const auto result = sim.step();
                    require(result.converged, "Dynamic feedback circuit did not converge");
                    expected = value;
                    require(sim.signal(x) == expected, "Dynamic feedback state mismatch at iteration " + std::to_string(i));
                }
                require(!sim.signal(x), "Dynamic feedback final state should be zero");
            }
        }
        std::filesystem::remove_all(cache);
    }
}

void test_large_sequential_scratch_buffer() {
    constexpr std::size_t dff_count = 256;
    hcs::CircuitBuilder builder;
    const auto clock = builder.add_input();
    const auto data = builder.add_input();
    std::vector<hcs::SignalId> q(dff_count);
    for (auto& signal : q) signal = builder.add_signal();
    for (std::size_t i = 0; i < dff_count; ++i)
        builder.add_d_flip_flop(i == 0 ? data : q[i - 1], clock, q[i], false);
    const auto circuit = builder.build();
    const auto cache = std::filesystem::temp_directory_path() / "hcs-large-dff-scratch-test-cache";
    hcs::Simulator reference(circuit, interpreted(hcs::InterpreterStrategy::EventDriven));
    hcs::SimulatorConfig dense_config;
    dense_config.backend = hcs::Backend::OptimizedIR;
    dense_config.strategy = hcs::InterpreterStrategy::Dense;
    hcs::Simulator ir_dense(circuit, dense_config);
    auto event_config = dense_config;
    event_config.strategy = hcs::InterpreterStrategy::EventDriven;
    hcs::Simulator ir_event(circuit, event_config);
    hcs::SimulatorConfig native_config;
    native_config.backend = hcs::Backend::Compiled;
    native_config.native_options.cache_directory = cache;
    hcs::Simulator native(circuit, native_config);

    for (std::uint32_t cycle = 0; cycle < 300; ++cycle) {
        const bool input_value = (cycle % 7u) == 0u;
        for (auto* sim : {&reference, &ir_dense, &ir_event, &native}) {
            sim->set_input(data, input_value);
            sim->set_input(clock, true);
            require(sim->step().converged, "Large sequential rising edge did not converge");
            sim->set_input(clock, false);
            require(sim->step().converged, "Large sequential falling edge did not converge");
        }
        for (hcs::SignalId id = 0; id < reference.signal_count(); ++id) {
            const bool expected = reference.signal(id);
            require(ir_dense.signal(id) == expected && ir_event.signal(id) == expected && native.signal(id) == expected,
                    "Large sequential differential mismatch at cycle=" + std::to_string(cycle) +
                    " signal=" + std::to_string(id));
        }
    }
    std::filesystem::remove_all(cache);
}

void test_extended_event_profiling() {
    hcs::CircuitBuilder builder;
    const auto input = builder.add_input();
    const auto a = builder.add_unary(hcs::LogicOp::Not, input);
    (void)builder.add_unary(hcs::LogicOp::Buffer, a);
    const auto circuit = builder.build();
    auto config = interpreted(hcs::InterpreterStrategy::EventDriven);
    config.enable_profiling = true;
    hcs::Simulator sim(circuit, config);
    sim.set_input(input, true);
    (void)sim.step();
    const auto& p = sim.execution_profile();
    require(p.queue_enqueue_count > 0 && p.queue_dequeue_count > 0,
            "Event profiler did not record queue operations");
    require(p.fanout_edges_visited > 0 && p.schedule_attempts > 0,
            "Event profiler did not record dependency scheduling");
    require(p.operation_evaluation_count > 0 && p.signal_write_count > 0,
            "Event profiler did not record evaluation and signal updates");
    require(p.settle_calls > 0 && p.settle_iteration_count > 0,
            "Event profiler did not record settle iterations");
}

void test_compiler_statistics_topology() {
    hcs::CircuitBuilder builder;
    const auto input = builder.add_input();
    const auto one = builder.add_constant(true);
    const auto out = builder.add_binary(hcs::LogicOp::And, input, one);
    (void)out;
    const auto prepared = hcs::prepare_circuit(builder.build());
    require(prepared.statistics.signals == 3, "Compiler statistics signal count mismatch");
    require(prepared.statistics.initial_ir_operations == 2, "Compiler statistics IR operation count mismatch");
    require(prepared.statistics.dependency_edges > 0, "Compiler statistics dependency edge count missing");
    require(prepared.statistics.opcode_counts[static_cast<std::size_t>(hcs::LogicOp::Constant1)] == 1,
            "Compiler statistics opcode counts mismatch");
}

void test_randomized_feedback_differential() {
    constexpr std::uint64_t seed = 0x43f09b71ull;
    std::mt19937_64 rng(seed);
    hcs::CircuitBuilder builder;
    std::vector<std::pair<hcs::SignalId, hcs::SignalId>> controls;
    for (int i = 0; i < 12; ++i) {
        const auto set = builder.add_input();
        const auto hold = builder.add_input();
        const auto state = builder.add_signal();
        const auto retained = builder.add_binary(hcs::LogicOp::And, state, hold);
        builder.add_gate(hcs::LogicOp::Or, retained, set, state);
        controls.emplace_back(set, hold);
    }
    const auto circuit = builder.build();

    auto interpreted_dense = interpreted(hcs::InterpreterStrategy::Dense);
    hcs::Simulator reference(circuit, interpreted_dense);
    auto interpreted_event = interpreted(hcs::InterpreterStrategy::EventDriven);
    hcs::Simulator event(circuit, interpreted_event);
    hcs::SimulatorConfig ir_dense_cfg;
    ir_dense_cfg.backend = hcs::Backend::OptimizedIR;
    ir_dense_cfg.strategy = hcs::InterpreterStrategy::Dense;
    hcs::Simulator ir_dense(circuit, ir_dense_cfg);
    auto ir_event_cfg = ir_dense_cfg;
    ir_event_cfg.strategy = hcs::InterpreterStrategy::EventDriven;
    hcs::Simulator ir_event(circuit, ir_event_cfg);
    hcs::SimulatorConfig hybrid_cfg;
    hybrid_cfg.backend = hcs::Backend::HybridIR;
    hcs::Simulator hybrid(circuit, hybrid_cfg);
    hcs::SimulatorConfig native_cfg;
    native_cfg.backend = hcs::Backend::Compiled;
    native_cfg.native_options.cache_directory = std::filesystem::temp_directory_path() / "hcs-random-feedback-cache";
    hcs::Simulator native(circuit, native_cfg);

    for (int step = 0; step < 128; ++step) {
        for (const auto [set, hold] : controls) {
            const bool set_value = (rng() & 1u) != 0;
            const bool hold_value = (rng() & 1u) != 0;
            for (auto* simulator : {&reference, &event, &ir_dense, &ir_event, &hybrid, &native}) {
                simulator->set_input(set, set_value);
                simulator->set_input(hold, hold_value);
            }
        }
        for (auto* simulator : {&reference, &event, &ir_dense, &ir_event, &hybrid, &native}) {
            require(simulator->step().converged,
                    "Randomized feedback circuit failed to converge; seed=" + std::to_string(seed));
        }
        for (hcs::SignalId signal = 0; signal < ir_event.signal_count(); ++signal) {
            const bool expected = ir_event.signal(signal);
            require(event.signal(signal) == expected && hybrid.signal(signal) == expected,
                    "Randomized feedback event-semantics mismatch; seed=" + std::to_string(seed) +
                    " step=" + std::to_string(step) + " signal=" + std::to_string(signal));
        }
    }
}

void test_hybrid_multiple_interconnected_feedback_sccs() {
    hcs::CircuitBuilder builder;
    const auto set = builder.add_input();
    const auto hold = builder.add_input();
    const auto x = builder.add_signal();
    const auto y = builder.add_signal();
    const auto z = builder.add_signal();
    const auto x_hold = builder.add_binary(hcs::LogicOp::And, x, hold);
    const auto y_hold = builder.add_binary(hcs::LogicOp::And, y, hold);
    const auto z_hold = builder.add_binary(hcs::LogicOp::And, z, hold);
    builder.add_gate(hcs::LogicOp::Or, x_hold, set, x);
    builder.add_gate(hcs::LogicOp::Or, y_hold, x, y);
    builder.add_gate(hcs::LogicOp::Or, z_hold, y, z);
    const auto circuit = builder.build();

    hcs::SimulatorConfig dense_cfg;
    dense_cfg.backend = hcs::Backend::OptimizedIR;
    dense_cfg.strategy = hcs::InterpreterStrategy::Dense;
    hcs::Simulator dense(circuit, dense_cfg);
    hcs::SimulatorConfig event_cfg;
    event_cfg.backend = hcs::Backend::OptimizedIR;
    event_cfg.strategy = hcs::InterpreterStrategy::EventDriven;
    hcs::Simulator event(circuit, event_cfg);
    hcs::SimulatorConfig hybrid_cfg;
    hybrid_cfg.backend = hcs::Backend::HybridIR;
    hcs::Simulator hybrid(circuit, hybrid_cfg);

    for (const auto [set_value, hold_value] : {std::pair{true, false}, std::pair{false, true}, std::pair{false, false}}) {
        for (auto* simulator : {&dense, &event, &hybrid}) {
            simulator->set_input(set, set_value);
            simulator->set_input(hold, hold_value);
            require(simulator->step().converged, "Connected feedback SCCs failed to converge");
        }
        for (hcs::SignalId signal = 0; signal < hybrid.signal_count(); ++signal)
            require(hybrid.signal(signal) == event.signal(signal),
                    "Hybrid feedback SCC observability mismatch against event reference; signal=" + std::to_string(signal));
    }
    require(!hybrid.signal(x) && !hybrid.signal(y) && !hybrid.signal(z),
            "Connected feedback SCCs did not settle to the expected reset state");
}

void test_hybrid_dirty_region_scheduling() {
    hcs::CircuitBuilder builder;
    const auto active = builder.add_input();
    const auto inactive = builder.add_input();
    auto active_tail = active;
    auto inactive_tail = inactive;
    for (int i = 0; i < 6; ++i) {
        const auto control = builder.add_input();
        active_tail = builder.add_binary(hcs::LogicOp::Xor, active_tail, control);
    }
    for (int i = 0; i < 4; ++i) {
        const auto control = builder.add_input();
        inactive_tail = builder.add_binary(hcs::LogicOp::Xor, inactive_tail, control);
    }
    const auto constant = builder.add_constant(true);
    const auto circuit = builder.build();

    hcs::SimulatorConfig cfg;
    cfg.backend = hcs::Backend::HybridIR;
    cfg.enable_profiling = true;
    hcs::Simulator hybrid(circuit, cfg);
    hcs::SimulatorConfig event_cfg;
    event_cfg.backend = hcs::Backend::OptimizedIR;
    event_cfg.strategy = hcs::InterpreterStrategy::EventDriven;
    hcs::Simulator event(circuit, event_cfg);

    for (auto* simulator : {&hybrid, &event}) {
        simulator->set_input(active, true);
        require(simulator->step().converged, "Hybrid region initialization failed to converge");
    }
    for (hcs::SignalId signal = 0; signal < hybrid.signal_count(); ++signal)
        require(hybrid.signal(signal) == event.signal(signal), "Hybrid initial observability mismatch");

    const auto before = hybrid.execution_profile();
    hybrid.set_input(active, false);
    event.set_input(active, false);
    require(hybrid.step().converged && event.step().converged, "Sparse region step failed to converge");
    const auto& after = hybrid.execution_profile();
    require(after.regions_processed - before.regions_processed == 1,
            "A change in one independent chain must process exactly one dirty region; before=" + std::to_string(before.regions_processed) + " after=" + std::to_string(after.regions_processed) + " scheduled=" + std::to_string(after.regions_scheduled - before.regions_scheduled));
    require(after.region_operation_evaluations - before.region_operation_evaluations == 6,
            "Acyclic dirty regions must evaluate each operation exactly once in topological order");
    require(after.regions_scheduled - before.regions_scheduled == 1,
            "A changed input must schedule one region, not each dependent operation");
    for (hcs::SignalId signal = 0; signal < hybrid.signal_count(); ++signal)
        require(hybrid.signal(signal) == event.signal(signal), "Hybrid sparse-region observability mismatch");
    require(hybrid.signal(active_tail) == event.signal(active_tail) &&
            hybrid.signal(inactive_tail) == event.signal(inactive_tail) && hybrid.signal(constant),
            "Hybrid lost an observable region or constant signal");
}

void test_execution_profiling() {
    hcs::CircuitBuilder builder;
    const auto input = builder.add_input();
    const auto one = builder.add_constant(true);
    const auto first = builder.add_binary(hcs::LogicOp::And, input, one);
    const auto output = builder.add_unary(hcs::LogicOp::Not, first);
    const auto circuit = builder.build();
    hcs::SimulatorConfig cfg;
    cfg.backend = hcs::Backend::OptimizedIR;
    cfg.strategy = hcs::InterpreterStrategy::EventDriven;
    cfg.enable_profiling = true;
    hcs::Simulator sim(circuit, cfg);
    sim.set_input(input, true);
    (void)sim.step();
    const auto& profile = sim.execution_profile();
    require(profile.events_processed > 0, "Profiler did not count processed IR events");
    require(profile.events_enqueued >= profile.events_processed, "Profiler event enqueue count is inconsistent");
    require(profile.signal_changes > 0, "Profiler did not count signal changes");
    require(profile.maximum_queue_length > 0, "Profiler did not track queue length");
    require(sim.signal(output) == false, "Profiling changed simulation behavior");

    hcs::SimulatorConfig dense_cfg = cfg;
    dense_cfg.strategy = hcs::InterpreterStrategy::Dense;
    hcs::Simulator dense(circuit, dense_cfg);
    dense.set_input(input, true);
    (void)dense.step();
    require(dense.execution_profile().dense_operations > 0 && dense.execution_profile().settle_calls > 0,
            "Dense profiler did not count operations or settle calls");
}

void test_optimized_ir_event_dff() {
    hcs::CircuitBuilder builder;
    const auto clock = builder.add_input();
    const auto d = builder.add_input();
    const auto q = builder.add_signal();
    const auto out = builder.add_unary(hcs::LogicOp::Not, q);
    builder.add_d_flip_flop(d, clock, q, false);
    const auto circuit = builder.build();
    hcs::Simulator reference(circuit, interpreted(hcs::InterpreterStrategy::EventDriven));
    hcs::SimulatorConfig cfg; cfg.backend = hcs::Backend::OptimizedIR;
    cfg.strategy = hcs::InterpreterStrategy::EventDriven;
    hcs::Simulator ir(circuit, cfg);
    for (int i = 0; i < 64; ++i) {
        const bool value = (i & 3) < 2;
        reference.set_input(d, value); ir.set_input(d, value);
        reference.set_input(clock, true); ir.set_input(clock, true);
        (void)reference.step(); (void)ir.step();
        reference.set_input(clock, false); ir.set_input(clock, false);
        (void)reference.step(); (void)ir.step();
        require(reference.signal(q) == ir.signal(q) && reference.signal(out) == ir.signal(out),
                "Optimized IR event-driven DFF mismatch");
    }
}


void test_hardware_acceleration_capabilities_and_validation() {
    const auto capabilities = hcs::detect_hardware_capabilities();
    require(!capabilities.architecture.empty(), "Hardware detection did not report an architecture");
    require(capabilities.logical_cpu_count >= 1, "Hardware detection returned zero logical CPUs");
    require(capabilities.avx2_usable == (capabilities.avx2_cpu_supported && capabilities.avx2_kernel_compiled),
            "AVX2 usability must reflect both CPU support and whether the kernel was compiled");
    require(!capabilities.gpu_device_available && !capabilities.vulkan_compute_compiled,
            "GPU must not be advertised without a compiled Vulkan compute backend");

    hcs::CircuitBuilder builder;
    const auto input = builder.add_input();
    (void)builder.add_unary(hcs::LogicOp::Not, input);
    const auto circuit = builder.build();

    hcs::SimulatorConfig gpu_config;
    gpu_config.backend = hcs::Backend::HybridIR;
    gpu_config.acceleration.gpu_enabled = true;
    bool gpu_rejected = false;
    try { hcs::Simulator rejected(circuit, gpu_config); }
    catch (const std::runtime_error&) { gpu_rejected = true; }
    require(gpu_rejected, "Unavailable GPU execution must be rejected clearly");

    hcs::SimulatorConfig invalid_threads;
    invalid_threads.backend = hcs::Backend::HybridIR;
    invalid_threads.acceleration.parallel = hcs::CpuParallelMode::Custom;
    invalid_threads.acceleration.worker_threads = 0;
    bool threads_rejected = false;
    try { hcs::Simulator rejected(circuit, invalid_threads); }
    catch (const std::invalid_argument&) { threads_rejected = true; }
    require(threads_rejected, "Custom worker count zero must be rejected");

    if (!capabilities.avx2_usable) {
        hcs::SimulatorConfig avx_config;
        avx_config.backend = hcs::Backend::HybridIR;
        avx_config.acceleration.simd = hcs::SimdMode::AVX2;
        bool avx_rejected = false;
        try { hcs::Simulator rejected(circuit, avx_config); }
        catch (const std::runtime_error&) { avx_rejected = true; }
        require(avx_rejected, "Explicit unsupported AVX2 must be rejected");
    }
}

void test_hybrid_avx2_differential() {
    hcs::CircuitBuilder builder;
    constexpr std::size_t lanes = 35;
    const std::array<hcs::LogicOp, 10> opcodes{
        hcs::LogicOp::Constant0, hcs::LogicOp::Constant1, hcs::LogicOp::Buffer,
        hcs::LogicOp::Not, hcs::LogicOp::And, hcs::LogicOp::Or, hcs::LogicOp::Xor,
        hcs::LogicOp::Nand, hcs::LogicOp::Nor, hcs::LogicOp::Xnor,
    };
    std::vector<hcs::SignalId> inputs;
    std::vector<hcs::SignalId> outputs;
    for (const auto opcode : opcodes) {
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            hcs::SignalId output = hcs::kInvalidSignal;
            if (opcode == hcs::LogicOp::Constant0 || opcode == hcs::LogicOp::Constant1) {
                output = builder.add_constant(opcode == hcs::LogicOp::Constant1);
            } else if (opcode == hcs::LogicOp::Buffer || opcode == hcs::LogicOp::Not) {
                const auto input = builder.add_input();
                inputs.push_back(input);
                output = builder.add_unary(opcode, input);
            } else {
                const auto a = builder.add_input();
                const auto b = builder.add_input();
                inputs.push_back(a);
                inputs.push_back(b);
                output = builder.add_binary(opcode, a, b);
            }
            outputs.push_back(output);
        }
    }
    std::vector<hcs::SignalId> reduction = outputs;
    while (reduction.size() > 1) {
        std::vector<hcs::SignalId> next;
        next.reserve((reduction.size() + 1) / 2);
        for (std::size_t i = 0; i < reduction.size(); i += 2) {
            if (i + 1 < reduction.size()) next.push_back(builder.add_binary(hcs::LogicOp::Or, reduction[i], reduction[i + 1]));
            else next.push_back(reduction[i]);
        }
        reduction = std::move(next);
    }
    const auto circuit = builder.build();

    hcs::SimulatorConfig scalar_config;
    scalar_config.backend = hcs::Backend::HybridIR;
    hcs::Simulator scalar(circuit, scalar_config);

    hcs::SimulatorConfig auto_config;
    auto_config.backend = hcs::Backend::HybridIR;
    auto_config.acceleration.simd = hcs::SimdMode::Auto;
    hcs::Simulator automatic(circuit, auto_config);
    require(automatic.simd_implementation() == hcs::SimdImplementation::Scalar,
            "SIMD Auto must remain scalar until a consistent workload win is established");

    const auto capabilities = hcs::detect_hardware_capabilities();
    hcs::SimulatorConfig vector_config;
    vector_config.backend = hcs::Backend::HybridIR;
    vector_config.acceleration.simd = capabilities.avx2_usable ? hcs::SimdMode::AVX2 : hcs::SimdMode::Off;
    hcs::Simulator vectorized(circuit, vector_config);

    std::mt19937 random(0x52A52u);
    for (unsigned tick = 0; tick < 80; ++tick) {
        for (const auto input : inputs) {
            const bool value = (random() & 1u) != 0;
            scalar.set_input(input, value);
            vectorized.set_input(input, value);
        }
        const auto expected = scalar.step();
        const auto actual = vectorized.step();
        require(expected.converged && actual.converged, "AVX2 differential circuit failed to converge");
        for (hcs::SignalId id = 0; id < scalar.signal_count(); ++id)
            require(scalar.signal(id) == vectorized.signal(id), "AVX2 changed an observable signal");
    }

    if (capabilities.avx2_usable) {
        require(vectorized.simd_implementation() == hcs::SimdImplementation::AVX2,
                "Explicit AVX2 selection did not activate the AVX2 kernel");
        require(vectorized.acceleration_stats().simd_batches > 0 &&
                vectorized.acceleration_stats().simd_operations >= 32,
                "AVX2 path was selected but no SIMD batches were actually executed");

        hcs::SimulatorConfig profiled_config = vector_config;
        profiled_config.enable_profiling = true;
        hcs::Simulator profiled(circuit, profiled_config);
        require(profiled.simd_implementation() == hcs::SimdImplementation::Scalar &&
                profiled.worker_thread_count() == 1,
                "Detailed profiling must select the instrumented scalar path");
        for (const auto input : inputs) profiled.set_input(input, true);
        (void)profiled.step();
        require(profiled.execution_profile().operation_evaluation_count > 0,
                "Profiling did not record scalar HybridIR operations");
    }
}

void test_hybrid_multicore_independent_regions() {
    hcs::CircuitBuilder builder;
    constexpr std::size_t regions = 192;
    constexpr std::size_t depth = 24;
    const auto shared_control = builder.add_input();
    std::vector<hcs::SignalId> inputs(regions + 1);
    inputs[0] = shared_control;
    for (std::size_t i = 0; i < regions; ++i) {
        auto signal = builder.add_input();
        inputs[i + 1] = signal;
        for (std::size_t stage = 0; stage < depth; ++stage)
            signal = builder.add_binary(hcs::LogicOp::Xor, signal, shared_control);
    }
    const auto circuit = builder.build();

    hcs::SimulatorConfig scalar_config;
    scalar_config.backend = hcs::Backend::HybridIR;
    hcs::Simulator scalar(circuit, scalar_config);

    hcs::SimulatorConfig parallel_config;
    parallel_config.backend = hcs::Backend::HybridIR;
    parallel_config.acceleration.parallel = hcs::CpuParallelMode::Custom;
    parallel_config.acceleration.worker_threads = 3;
    hcs::Simulator parallel(circuit, parallel_config);
    require(parallel.worker_thread_count() == 3, "Persistent worker pool did not create requested workers");

    std::mt19937 random(0x51A2u);
    for (unsigned tick = 0; tick < 40; ++tick) {
        for (const auto input : inputs) {
            const bool value = (random() & 1u) != 0;
            scalar.set_input(input, value);
            parallel.set_input(input, value);
        }
        const auto expected = scalar.step();
        const auto actual = parallel.step();
        require(expected.converged && actual.converged, "Multicore region circuit failed to converge");
        require(expected.operations == actual.operations && expected.events == actual.events,
                "Multicore execution changed HybridIR work accounting");
        for (hcs::SignalId id = 0; id < scalar.signal_count(); ++id)
            require(scalar.signal(id) == parallel.signal(id), "Multicore execution changed an observable signal");
    }

    require(parallel.acceleration_stats().parallel_region_batches > 0 &&
            parallel.acceleration_stats().parallel_regions >= regions,
            "Multicore path was selected but no region batch was actually processed in parallel");

    hcs::SimulatorConfig profile_config = parallel_config;
    profile_config.enable_profiling = true;
    hcs::Simulator profiled(circuit, profile_config);
    require(profiled.worker_thread_count() == 1,
            "Detailed profiling should force serial execution until counters are thread-safe");
}


void test_hybrid_avx2_multicore_combined() {
    hcs::CircuitBuilder builder;
    constexpr std::size_t region_count = 16;
    constexpr std::size_t fanout_width = 256;
    const auto shared_control = builder.add_input();
    std::vector<hcs::SignalId> inputs{shared_control};
    for (std::size_t region = 0; region < region_count; ++region) {
        const auto root_input = builder.add_input();
        inputs.push_back(root_input);
        const auto root = builder.add_binary(hcs::LogicOp::And, shared_control, root_input);
        std::vector<hcs::SignalId> layer;
        layer.reserve(fanout_width);
        for (std::size_t lane = 0; lane < fanout_width; ++lane) {
            const auto leaf_input = builder.add_input();
            inputs.push_back(leaf_input);
            layer.push_back(builder.add_binary(hcs::LogicOp::Xor, root, leaf_input));
        }
        while (layer.size() > 1) {
            std::vector<hcs::SignalId> next;
            next.reserve((layer.size() + 1) / 2);
            for (std::size_t i = 0; i < layer.size(); i += 2) {
                if (i + 1 < layer.size())
                    next.push_back(builder.add_binary(hcs::LogicOp::Or, layer[i], layer[i + 1]));
                else
                    next.push_back(layer[i]);
            }
            layer = std::move(next);
        }
    }
    const auto circuit = builder.build();

    hcs::SimulatorConfig scalar_config;
    scalar_config.backend = hcs::Backend::HybridIR;
    hcs::Simulator scalar(circuit, scalar_config);

    hcs::SimulatorConfig accelerated_config = scalar_config;
    accelerated_config.acceleration.parallel = hcs::CpuParallelMode::Custom;
    accelerated_config.acceleration.worker_threads = 3;
    const auto capabilities = hcs::detect_hardware_capabilities();
    if (capabilities.avx2_usable) accelerated_config.acceleration.simd = hcs::SimdMode::AVX2;
    hcs::Simulator accelerated(circuit, accelerated_config);

    std::mt19937 random(0xA52B17u);
    for (const auto input : inputs) {
        const bool value = (random() & 1u) != 0;
        scalar.set_input(input, value);
        accelerated.set_input(input, value);
    }
    for (unsigned tick = 0; tick < 12; ++tick) {
        const bool value = (tick & 1u) != 0;
        scalar.set_input(shared_control, value);
        accelerated.set_input(shared_control, value);
        const auto expected = scalar.step();
        const auto actual = accelerated.step();
        require(expected.converged && actual.converged,
                "Combined AVX2/multicore workload failed to converge");
        require(expected.operations == actual.operations && expected.events == actual.events,
                "Combined AVX2/multicore execution changed work accounting");
        for (hcs::SignalId id = 0; id < scalar.signal_count(); ++id)
            require(scalar.signal(id) == accelerated.signal(id),
                    "Combined AVX2/multicore execution changed an observable signal");
    }
    require(accelerated.acceleration_stats().parallel_region_batches > 0 &&
            accelerated.acceleration_stats().parallel_regions >= region_count,
            "Combined acceleration did not enter the multicore region path");
    if (capabilities.avx2_usable) {
        require(accelerated.acceleration_stats().simd_batches > 0 &&
                accelerated.acceleration_stats().simd_operations >= region_count * fanout_width,
                "Combined acceleration did not execute AVX2 batches in parallel regions");
    }

    hcs::SimulatorConfig excessive_config = accelerated_config;
    excessive_config.acceleration.worker_threads = 65;
    bool excessive_rejected = false;
    try { hcs::Simulator rejected(circuit, excessive_config); }
    catch (const std::invalid_argument&) { excessive_rejected = true; }
    require(excessive_rejected, "Custom worker count above the documented limit must be rejected");
}

void test_hybrid_multicore_feedback_fallback() {
    hcs::CircuitBuilder builder;
    const auto set = builder.add_input();
    const auto first = builder.add_signal();
    const auto second = builder.add_signal();
    builder.add_gate(hcs::LogicOp::Or, first, set, first);
    builder.add_gate(hcs::LogicOp::Or, second, first, second);
    const auto out = builder.add_unary(hcs::LogicOp::Buffer, second);
    const auto circuit = builder.build();

    hcs::SimulatorConfig scalar_config;
    scalar_config.backend = hcs::Backend::HybridIR;
    hcs::Simulator scalar(circuit, scalar_config);
    hcs::SimulatorConfig parallel_config = scalar_config;
    parallel_config.acceleration.parallel = hcs::CpuParallelMode::Custom;
    parallel_config.acceleration.worker_threads = 2;
    hcs::Simulator parallel(circuit, parallel_config);
    for (unsigned tick = 0; tick < 16; ++tick) {
        const bool value = (tick & 1u) != 0;
        scalar.set_input(set, value);
        parallel.set_input(set, value);
        const auto a = scalar.step();
        const auto b = parallel.step();
        require(a.converged == b.converged, "Multicore feedback fallback changed convergence status");
        for (hcs::SignalId id = 0; id < scalar.signal_count(); ++id)
            require(scalar.signal(id) == parallel.signal(id), "Multicore feedback fallback changed signal state");
    }
    require(parallel.acceleration_stats().parallel_region_batches == 0,
            "Feedback regions must not enter the multicore acyclic scheduler");
    (void)out;
}

} // namespace

int main() {
    try {
        test_truth_table();
        test_reverse_order_dependency_chain();
        test_event_and_dense_equivalence();
        test_dff_counter();
        test_native_matches_interpreted_and_cache();
        test_compiled_dff_semantics();
        test_oscillation_limit();
        test_ir_lowering_and_optimization();
        test_dependency_scc_analysis();
        test_optimized_ir_backend();
        test_optimized_ir_dff();
        test_all_logic_operations();
        test_optimizer_pass_configuration();
        test_native_compiler_timings();
        test_independent_region_grouping();
        test_optimized_ir_event_equivalence_and_randomized();
        test_optimized_ir_event_dff();
        test_execution_profiling();
        test_randomized_feedback_differential();
        test_hybrid_multiple_interconnected_feedback_sccs();
        test_hybrid_dirty_region_scheduling();
        test_extended_event_profiling();
        test_simultaneous_dff_updates_and_feedback_dynamic();
        test_large_sequential_scratch_buffer();
        test_randomized_sequential_differential();
        test_compiler_statistics_topology();
        test_hardware_acceleration_capabilities_and_validation();
        test_hybrid_avx2_differential();
        test_hybrid_multicore_independent_regions();
        test_hybrid_multicore_feedback_fallback();
        test_hybrid_avx2_multicore_combined();
        std::cout << "All HCS tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
