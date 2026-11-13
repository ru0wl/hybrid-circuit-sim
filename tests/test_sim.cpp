#include "hcs/simulator.hpp"
#include "hcs/compiler_pipeline.hpp"

#include <filesystem>
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
        for (int step = 0; step < 100; ++step) {
            for (const auto input : inputs) {
                const bool value = (rng() & 1u) != 0;
                reference.set_input(input, value);
                ir_dense.set_input(input, value);
                ir_event.set_input(input, value);
            }
            const auto rr = reference.step();
            const auto dr = ir_dense.step();
            const auto er = ir_event.step();
            require(rr.converged && dr.converged && er.converged,
                    "Random differential circuit did not converge; seed=" + std::to_string(seed));
            for (hcs::SignalId signal = 0; signal < reference.signal_count(); ++signal) {
                require(reference.signal(signal) == ir_dense.signal(signal) &&
                        reference.signal(signal) == ir_event.signal(signal),
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
                native.set_input(input, value);
            }
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &native}) simulator->set_input(clock, true);
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &native}) {
                const auto result = simulator->step();
                require(result.converged, "Random sequential circuit did not converge; seed=" + std::to_string(seed));
            }
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &native}) simulator->set_input(clock, false);
            for (auto* simulator : {&reference, &ir_dense, &ir_event, &native}) (void)simulator->step();
            for (hcs::SignalId signal = 0; signal < reference.signal_count(); ++signal) {
                const bool expected = reference.signal(signal);
                require(ir_dense.signal(signal) == expected && ir_event.signal(signal) == expected && native.signal(signal) == expected,
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
        for (const auto backend : {hcs::Backend::Interpreted, hcs::Backend::OptimizedIR, hcs::Backend::Compiled}) {
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
        for (const auto backend : {hcs::Backend::Interpreted, hcs::Backend::OptimizedIR, hcs::Backend::Compiled}) {
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
        test_extended_event_profiling();
        test_simultaneous_dff_updates_and_feedback_dynamic();
        test_large_sequential_scratch_buffer();
        test_randomized_sequential_differential();
        test_compiler_statistics_topology();
        std::cout << "All HCS tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
