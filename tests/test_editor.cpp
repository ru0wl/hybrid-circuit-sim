#include "hcs/editor/camera.hpp"
#include "hcs/editor/model.hpp"
#include "hcs/editor/runtime.hpp"
#include "hcs/editor/simulation_controller.hpp"

#include <array>
#include <cstdlib>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

hcs::editor::PinId input_pin(const hcs::editor::CircuitEditor& editor, hcs::editor::ComponentId id, std::size_t index = 0) {
    const auto* component = editor.find_component(id);
    if (!component || index >= component->inputs.size()) throw std::runtime_error("Missing input pin");
    return component->inputs[index].id;
}
hcs::editor::PinId output_pin(const hcs::editor::CircuitEditor& editor, hcs::editor::ComponentId id, std::size_t index = 0) {
    const auto* component = editor.find_component(id);
    if (!component || index >= component->outputs.size()) throw std::runtime_error("Missing output pin");
    return component->outputs[index].id;
}
bool output_value(const hcs::editor::CircuitRuntime& runtime, const hcs::editor::CircuitEditor& editor,
                  hcs::editor::ComponentId output) {
    return runtime.signal_for_pin(input_pin(editor, output)) == hcs::editor::SignalState::One;
}

void test_model_direction_and_single_driver() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto a = editor.add_component(ComponentType::Input, {0, 0});
    const auto b = editor.add_component(ComponentType::Output, {200, 0});
    const auto c = editor.add_component(ComponentType::Output, {400, 0});
    require(editor.connect(output_pin(editor, a), input_pin(editor, b)).has_value(), "Output-to-input connection rejected");
    require(!editor.connect(input_pin(editor, b), input_pin(editor, c)).has_value(), "Input-to-input connection accepted");
    require(!editor.connect(output_pin(editor, a), output_pin(editor, a)).has_value(), "Output-to-output connection accepted");
    require(!editor.connect(output_pin(editor, a), input_pin(editor, b)).has_value(), "Duplicate input driver accepted");
    require(editor.wires().size() == 1, "Invalid connection changed the wire list");
}

void test_not_integration() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto input = editor.add_component(ComponentType::Input, {0, 0});
    const auto inverter = editor.add_component(ComponentType::Not, {160, 0});
    const auto output = editor.add_component(ComponentType::Output, {340, 0});
    require(editor.connect(output_pin(editor, input), input_pin(editor, inverter)).has_value(), "Failed to connect NOT input");
    require(editor.connect(output_pin(editor, inverter), input_pin(editor, output)).has_value(), "Failed to connect NOT output");

    CircuitRuntime runtime;
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, output), "NOT(0) should be 1");

    const auto structure_before_input_change = editor.structural_revision();
    const auto* circuit_before_input_change = runtime.circuit();
    require(editor.set_input_value(input, true), "Could not set input component");
    require(editor.structural_revision() == structure_before_input_change, "Input changes must not mark the circuit structure dirty");
    runtime.synchronize(editor);
    require(runtime.circuit() == circuit_before_input_change, "Input changes should not rebuild the HCS circuit");
    (void)runtime.step();
    require(!output_value(runtime, editor, output), "NOT(1) should be 0");

    require(editor.set_input_value(input, false), "Could not set input component low");
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, output), "NOT(0) should return to 1");
    require(editor.find_pin(output_pin(editor, inverter))->simulation_signal != hcs::kInvalidSignal,
            "Runtime did not assign the output pin simulation mapping");
    require(editor.find_pin(input_pin(editor, inverter))->simulation_signal ==
            editor.find_pin(output_pin(editor, input))->simulation_signal,
            "Wire input pin was not mapped to its source signal");
}

void test_and_truth_table() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto a = editor.add_component(ComponentType::Input, {0, 0});
    const auto b = editor.add_component(ComponentType::Input, {0, 100});
    const auto gate = editor.add_component(ComponentType::And, {170, 40});
    const auto output = editor.add_component(ComponentType::Output, {350, 40});
    require(editor.connect(output_pin(editor, a), input_pin(editor, gate, 0)).has_value(), "AND A connection failed");
    require(editor.connect(output_pin(editor, b), input_pin(editor, gate, 1)).has_value(), "AND B connection failed");
    require(editor.connect(output_pin(editor, gate), input_pin(editor, output)).has_value(), "AND output connection failed");

    CircuitRuntime runtime(RuntimeBackend::OptimizedIRDense);
    runtime.synchronize(editor);
    for (int av = 0; av <= 1; ++av) {
        for (int bv = 0; bv <= 1; ++bv) {
            (void)editor.set_input_value(a, av != 0);
            (void)editor.set_input_value(b, bv != 0);
            runtime.synchronize(editor);
            const auto result = runtime.step();
            require(result.converged, "AND circuit did not converge");
            require(output_value(runtime, editor, output) == ((av != 0) && (bv != 0)), "AND truth-table mismatch");
        }
    }
}

void test_or_truth_table() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto a = editor.add_component(ComponentType::Input, {0, 0});
    const auto b = editor.add_component(ComponentType::Input, {0, 100});
    const auto gate = editor.add_component(ComponentType::Or, {170, 40});
    const auto output = editor.add_component(ComponentType::Output, {350, 40});
    (void)editor.connect(output_pin(editor, a), input_pin(editor, gate, 0));
    (void)editor.connect(output_pin(editor, b), input_pin(editor, gate, 1));
    (void)editor.connect(output_pin(editor, gate), input_pin(editor, output));

    CircuitRuntime runtime(RuntimeBackend::HybridIR);
    runtime.synchronize(editor);
    for (int av = 0; av <= 1; ++av) {
        for (int bv = 0; bv <= 1; ++bv) {
            (void)editor.set_input_value(a, av != 0);
            (void)editor.set_input_value(b, bv != 0);
            runtime.synchronize(editor);
            (void)runtime.step();
            require(output_value(runtime, editor, output) == ((av != 0) || (bv != 0)), "OR truth-table mismatch");
        }
    }
}

void test_xor_truth_table_and_backends() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto a = editor.add_component(ComponentType::Input, {0, 0});
    const auto b = editor.add_component(ComponentType::Input, {0, 100});
    const auto gate = editor.add_component(ComponentType::Xor, {170, 40});
    const auto output = editor.add_component(ComponentType::Output, {350, 40});
    (void)editor.connect(output_pin(editor, a), input_pin(editor, gate, 0));
    (void)editor.connect(output_pin(editor, b), input_pin(editor, gate, 1));
    (void)editor.connect(output_pin(editor, gate), input_pin(editor, output));

    const RuntimeBackend backends[]{RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative};
    for (const auto backend : backends) {
        CircuitRuntime runtime(backend);
        runtime.synchronize(editor);
        for (int av = 0; av <= 1; ++av) {
            for (int bv = 0; bv <= 1; ++bv) {
                (void)editor.set_input_value(a, av != 0);
                (void)editor.set_input_value(b, bv != 0);
                runtime.synchronize(editor);
                const auto result = runtime.step();
                require(result.converged, "XOR circuit did not converge");
                require(output_value(runtime, editor, output) == ((av != 0) != (bv != 0)), "XOR truth-table mismatch");
            }
        }
    }
}

void test_backend_switching_on_live_runtime() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto source = editor.add_component(ComponentType::Input, {0, 0});
    const auto inverter = editor.add_component(ComponentType::Not, {160, 0});
    const auto sink = editor.add_component(ComponentType::Output, {340, 0});
    (void)editor.connect(output_pin(editor, source), input_pin(editor, inverter));
    (void)editor.connect(output_pin(editor, inverter), input_pin(editor, sink));

    const std::array backends{
        RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative,
    };
    CircuitRuntime runtime(backends.front());
    runtime.synchronize(editor);
    for (const auto backend : backends) {
        runtime.set_backend(backend);
        runtime.synchronize(editor);
        (void)runtime.step();
        require(runtime.backend() == backend, "Runtime did not retain the selected backend");
        require(output_value(runtime, editor, sink), "Backend switch changed NOT output semantics");
        (void)editor.set_input_value(source, true);
        runtime.synchronize(editor);
        (void)runtime.step();
        require(!output_value(runtime, editor, sink), "Switched backend did not propagate updated input");
        (void)editor.set_input_value(source, false);
        runtime.synchronize(editor);
    }
}

void test_dff_simultaneous_update_through_editor() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto data = editor.add_component(ComponentType::Input, {0, 0});
    const auto clock = editor.add_component(ComponentType::Input, {0, 100});
    const auto dff = editor.add_component(ComponentType::DFlipFlop, {180, 30});
    const auto output = editor.add_component(ComponentType::Output, {380, 45});
    (void)editor.connect(output_pin(editor, data), input_pin(editor, dff, 0));
    (void)editor.connect(output_pin(editor, clock), input_pin(editor, dff, 1));
    (void)editor.connect(output_pin(editor, dff), input_pin(editor, output));

    CircuitRuntime runtime(RuntimeBackend::OptimizedIREvent);
    runtime.synchronize(editor);
    (void)runtime.step();
    require(!output_value(runtime, editor, output), "DFF should start at zero");

    (void)editor.set_input_value(data, true);
    (void)editor.set_input_value(clock, false);
    runtime.synchronize(editor);
    (void)runtime.step();
    require(!output_value(runtime, editor, output), "DFF changed without a rising edge");

    (void)editor.set_input_value(clock, true);
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, output), "DFF did not capture D on the rising edge");

    (void)editor.set_input_value(data, false);
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, output), "DFF changed while clock remained high");
    (void)editor.set_input_value(clock, false);
    runtime.synchronize(editor);
    (void)runtime.step();
    (void)editor.set_input_value(clock, true);
    runtime.synchronize(editor);
    (void)runtime.step();
    require(!output_value(runtime, editor, output), "DFF did not capture the next rising edge");
}

void test_structural_resynchronize() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto input = editor.add_component(ComponentType::Input, {0, 0});
    const auto inverter = editor.add_component(ComponentType::Not, {150, 0});
    const auto output = editor.add_component(ComponentType::Output, {330, 0});
    const auto first = editor.connect(output_pin(editor, input), input_pin(editor, inverter));
    const auto second = editor.connect(output_pin(editor, inverter), input_pin(editor, output));
    require(first && second, "Initial circuit wiring failed");

    CircuitRuntime runtime;
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, output), "Initial NOT output should be high");

    require(editor.remove_wire(*second), "Could not remove old output wire");
    require(editor.connect(output_pin(editor, input), input_pin(editor, output)).has_value(), "Could not connect input directly to output");
    runtime.synchronize(editor);
    (void)runtime.step();
    require(!output_value(runtime, editor, output), "Runtime did not rebuild after structural edit");

    const auto revision = editor.structural_revision();
    const auto* circuit_before_move = runtime.circuit();
    (void)editor.move_component(inverter, {220, 80});
    runtime.synchronize(editor);
    require(editor.structural_revision() == revision, "Moving a component should not change simulation structure");
    require(runtime.circuit() == circuit_before_move, "Moving a component should not rebuild the HCS circuit");
}

void test_component_removal_cleans_wires() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto source = editor.add_component(ComponentType::Input, {0, 0});
    const auto gate = editor.add_component(ComponentType::Not, {160, 0});
    const auto sink = editor.add_component(ComponentType::Output, {340, 0});
    (void)editor.connect(output_pin(editor, source), input_pin(editor, gate));
    (void)editor.connect(output_pin(editor, gate), input_pin(editor, sink));
    require(editor.wires().size() == 2, "Initial component removal fixture is not wired");

    CircuitRuntime runtime;
    runtime.synchronize(editor);
    (void)runtime.step();
    require(output_value(runtime, editor, sink), "Initial NOT output should be high");

    require(editor.remove_component(gate), "Could not remove selected component");
    require(editor.wires().empty(), "Removing a component must remove all attached wires");
    runtime.synchronize(editor);
    (void)runtime.step();
    require(runtime.ready(), "Runtime was not rebuilt after component removal");
    require(editor.find_component(gate) == nullptr, "Removed component remains in the editor model");
}

void test_simulation_controller_and_camera() {
    using namespace hcs::editor;
    CircuitEditor editor;
    const auto input = editor.add_component(ComponentType::Input, {0, 0});
    const auto output = editor.add_component(ComponentType::Output, {170, 0});
    (void)editor.connect(output_pin(editor, input), input_pin(editor, output));
    CircuitRuntime runtime;
    runtime.synchronize(editor);
    SimulationController controller(runtime);
    controller.notify_input_changed();
    controller.update(0.0);
    require(controller.total_steps() == 1, "Paused input changes should schedule one propagation step");
    controller.set_rate(SimulationRate::Hz10);
    controller.resume();
    controller.update(0.25);
    require(controller.total_steps() == 3, "Fixed-rate scheduler should execute elapsed simulation ticks");
    controller.pause();

    Camera2D camera;
    camera.set_viewport({0, 72, 1000, 600});
    const Vec2 world{123.0F, -42.0F};
    const auto screen = camera.world_to_screen(world);
    const auto restored = camera.screen_to_world(screen);
    require(std::abs(restored.x - world.x) < 0.001F && std::abs(restored.y - world.y) < 0.001F,
            "Camera coordinate conversion is not reversible");
    const Vec2 anchor{300.0F, 250.0F};
    const auto before = camera.screen_to_world(anchor);
    camera.zoom_at(anchor, 2.0F);
    const auto after = camera.screen_to_world(anchor);
    require(std::abs(before.x - after.x) < 0.001F && std::abs(before.y - after.y) < 0.001F,
            "Camera zoom should preserve the world point under the cursor");
}

} // namespace

int main() {
    try {
        test_model_direction_and_single_driver();
        test_not_integration();
        test_and_truth_table();
        test_or_truth_table();
        test_xor_truth_table_and_backends();
        test_backend_switching_on_live_runtime();
        test_dff_simultaneous_update_through_editor();
        test_structural_resynchronize();
        test_component_removal_cleans_wires();
        test_simulation_controller_and_camera();
    } catch (const std::exception& error) {
        std::cerr << "Editor test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "All editor/runtime tests passed.\n";
    return EXIT_SUCCESS;
}
