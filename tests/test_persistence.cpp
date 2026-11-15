#include "hcs/editor/persistence.hpp"
#include "hcs/editor/runtime.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

using hcs::editor::CircuitEditor;
using hcs::editor::CircuitRuntime;
using hcs::editor::ComponentId;
using hcs::editor::ComponentType;
using hcs::editor::PersistenceError;
using hcs::editor::RuntimeBackend;
using hcs::editor::Vec2;

hcs::editor::PinId input_pin(const CircuitEditor& editor, ComponentId id, std::size_t index = 0) {
    const auto* component = editor.find_component(id);
    return component && index < component->inputs.size() ? component->inputs[index].id : hcs::editor::kInvalidPin;
}
hcs::editor::PinId output_pin(const CircuitEditor& editor, ComponentId id, std::size_t index = 0) {
    const auto* component = editor.find_component(id);
    return component && index < component->outputs.size() ? component->outputs[index].id : hcs::editor::kInvalidPin;
}

std::string serialize(const CircuitEditor& editor, const std::string& name = "Persistence Test") {
    std::string text;
    PersistenceError error;
    require(hcs::editor::serialize_circuit(editor, name, text, error), "Serialization failed: " + error.describe());
    return text;
}

CircuitEditor make_not_circuit() {
    CircuitEditor editor;
    const auto input = editor.add_component(ComponentType::Input, {-40.5F, 20.25F});
    const auto gate = editor.add_component(ComponentType::Not, {130.0F, 25.0F});
    const auto output = editor.add_component(ComponentType::Output, {340.0F, 25.0F});
    (void)editor.set_input_value(input, true);
    require(editor.connect(output_pin(editor, input), input_pin(editor, gate)).has_value(), "NOT source connection failed");
    require(editor.connect(output_pin(editor, gate), input_pin(editor, output)).has_value(), "NOT sink connection failed");
    return editor;
}

CircuitEditor make_xor_circuit() {
    CircuitEditor editor;
    const auto a = editor.add_component(ComponentType::Input, {0.0F, 0.0F});
    const auto b = editor.add_component(ComponentType::Input, {0.0F, 140.0F});
    const auto gate = editor.add_component(ComponentType::Xor, {190.0F, 50.0F});
    const auto output = editor.add_component(ComponentType::Output, {390.0F, 50.0F});
    (void)editor.set_input_value(a, true);
    require(editor.connect(output_pin(editor, a), input_pin(editor, gate, 0)).has_value(), "XOR A connection failed");
    require(editor.connect(output_pin(editor, b), input_pin(editor, gate, 1)).has_value(), "XOR B connection failed");
    require(editor.connect(output_pin(editor, gate), input_pin(editor, output)).has_value(), "XOR output connection failed");
    return editor;
}

CircuitEditor make_dff_circuit() {
    CircuitEditor editor;
    const auto data = editor.add_component(ComponentType::Input, {0.0F, 0.0F});
    const auto clock = editor.add_component(ComponentType::Input, {0.0F, 120.0F});
    const auto dff = editor.add_component(ComponentType::DFlipFlop, {190.0F, 50.0F});
    const auto output = editor.add_component(ComponentType::Output, {410.0F, 55.0F});
    (void)editor.set_dff_initial_state(dff, true);
    require(editor.connect(output_pin(editor, data), input_pin(editor, dff, 0)).has_value(), "DFF D connection failed");
    require(editor.connect(output_pin(editor, clock), input_pin(editor, dff, 1)).has_value(), "DFF CLK connection failed");
    require(editor.connect(output_pin(editor, dff), input_pin(editor, output)).has_value(), "DFF Q connection failed");
    return editor;
}

bool output_value(CircuitRuntime& runtime, CircuitEditor& editor, ComponentId output) {
    runtime.synchronize(editor);
    (void)runtime.step();
    return runtime.binary_signal_for_pin(input_pin(editor, output));
}

void require_round_trip(const CircuitEditor& original, const std::string& name) {
    const auto text = serialize(original, name);
    CircuitEditor restored;
    std::string restored_name;
    PersistenceError error;
    require(hcs::editor::parse_circuit(text, restored, restored_name, error), "Round-trip parse failed: " + error.describe());
    require(restored_name == name, "Circuit name did not survive round-trip");
    require(serialize(restored, restored_name) == text, "Canonical round-trip output changed");
    require(restored.components().size() == original.components().size(), "Component count changed during round-trip");
    require(restored.wires().size() == original.wires().size(), "Wire count changed during round-trip");
}

void test_round_trip_examples() {
    auto not_circuit = make_not_circuit();
    require_round_trip(not_circuit, "NOT circuit");
    auto xor_circuit = make_xor_circuit();
    require_round_trip(xor_circuit, "XOR circuit");
    auto dff_circuit = make_dff_circuit();
    require_round_trip(dff_circuit, "DFF circuit");

    CircuitEditor many;
    const auto a = many.add_component(ComponentType::Input, {-200.0F, -100.0F});
    const auto b = many.add_component(ComponentType::Input, {-200.0F, 100.0F});
    const auto c = many.add_component(ComponentType::Input, {-200.0F, 300.0F});
    const auto first = many.add_component(ComponentType::And, {50.0F, -40.0F});
    const auto second = many.add_component(ComponentType::Or, {260.0F, 60.0F});
    const auto out = many.add_component(ComponentType::Output, {480.0F, 80.0F});
    require(many.move_component(first, {75.5F, -25.25F}), "Component move failed");
    require(many.connect(output_pin(many, a), input_pin(many, first, 0)).has_value(), "Many-input A wire failed");
    require(many.connect(output_pin(many, b), input_pin(many, first, 1)).has_value(), "Many-input B wire failed");
    require(many.connect(output_pin(many, first), input_pin(many, second, 0)).has_value(), "Inter-gate wire failed");
    require(many.connect(output_pin(many, c), input_pin(many, second, 1)).has_value(), "Third input wire failed");
    require(many.connect(output_pin(many, second), input_pin(many, out)).has_value(), "Output wire failed");
    require_round_trip(many, "Multiple inputs and wires");
}

void test_deterministic_serialization() {
    auto editor = make_xor_circuit();
    const auto first = serialize(editor, "Stable");
    for (int i = 0; i < 20; ++i) require(serialize(editor, "Stable") == first, "Repeated serialization is not deterministic");

    CircuitEditor reverse;
    require(reverse.add_component_with_id(90, ComponentType::Output, {50, 0}), "Could not add stable ID 90");
    require(reverse.add_component_with_id(12, ComponentType::Input, {-50, 0}, true), "Could not add stable ID 12");
    require(reverse.connect(output_pin(reverse, 12), input_pin(reverse, 90)).has_value(), "Stable ID wire failed");
    const auto text = serialize(reverse);
    require(text.find("component 12 Input") < text.find("component 90 Output"), "Components are not sorted by stable ID");
    require(text.find("wire 12.out -> 90.in") != std::string::npos, "Wire endpoints were not serialized by stable IDs");
}

void test_text_edit_apply_and_runtime() {
    CircuitEditor editor = make_not_circuit();
    auto text = serialize(editor, "Editable");
    const auto value_position = text.find("value=1");
    require(value_position != std::string::npos, "Input state was not serialized");
    text.replace(value_position, 7, "value=0");

    std::string name;
    PersistenceError error;
    require(hcs::editor::parse_circuit(text, editor, name, error), "Edited text failed to apply: " + error.describe());
    require(name == "Editable", "Edited circuit name was not preserved");
    const auto input = editor.components().front().id;
    require(!editor.find_component(input)->input_value, "Text edit did not update the input value");

    CircuitRuntime runtime(RuntimeBackend::OptimizedIREvent);
    runtime.synchronize(editor);
    const auto output = editor.components().back().id;
    require(output_value(runtime, editor, output), "Loaded NOT circuit did not produce NOT(0)=1");
    require(editor.set_input_value(input, true), "Could not toggle loaded input");
    require(!output_value(runtime, editor, output), "Loaded circuit did not react to input change");
}

void test_invalid_files_and_atomic_parse() {
    CircuitEditor target = make_not_circuit();
    const auto before = serialize(target, "Existing");
    const std::vector<std::pair<std::string, std::string>> invalid{
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0\n", "missing y"},
        {"HCS 2\ncircuit \"x\"\n", "version"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\ncomponent 1 Output x=1 y=1\n", "duplicate ID"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\nwire 1.out -> 2.in\n", "missing component"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\ncomponent 2 Output x=1 y=0\nwire 1.in -> 2.in\n", "invalid source pin"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\ncomponent 2 Output x=1 y=0\nwire 1.out -> 2.missing\n", "missing destination pin"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 x=1 y=0\n", "duplicate field"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=10000001 y=0\n", "out-of-range position"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Mystery x=0 y=0\n", "unknown component"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=nan y=0\n", "invalid numeric value"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\ncomponent 2 Output x=10 y=0\nwire 1.out -> 2.in\nwire 1.out -> 2.in\n", "multiple drivers"},
        {"HCS 1\ncircuit \"x\"\ncomponent 1 Input x=0 y=0\nthis is not a statement\n", "malformed syntax"},
    };
    for (const auto& [bad_text, label] : invalid) {
        std::string name = "must stay the same";
        PersistenceError error;
        require(!hcs::editor::parse_circuit(bad_text, target, name, error), "Invalid file was accepted: " + label);
        require(serialize(target, "Existing") == before, "Failed parse modified the existing model: " + label);
        require(name == "must stay the same", "Failed parse changed the circuit name: " + label);
        require(!error.message.empty(), "Invalid file did not report an error: " + label);
    }
}

void test_loaded_dff_runtime() {
    CircuitEditor original = make_dff_circuit();
    std::string text = serialize(original, "DFF Runtime");
    CircuitEditor loaded;
    std::string name;
    PersistenceError error;
    require(hcs::editor::parse_circuit(text, loaded, name, error), "DFF load failed: " + error.describe());

    const auto data = loaded.components()[0].id;
    const auto clock = loaded.components()[1].id;
    const auto dff = loaded.components()[2].id;
    const auto output = loaded.components()[3].id;
    CircuitRuntime runtime(RuntimeBackend::InterpretedEvent);
    runtime.synchronize(loaded);
    require(output_value(runtime, loaded, output), "Loaded DFF initial state was not restored");

    require(loaded.set_dff_initial_state(dff, false), "DFF initial state edit failed");
    runtime.synchronize(loaded);
    require(!output_value(runtime, loaded, output), "DFF reset did not use edited initial state");
    require(loaded.set_input_value(data, true), "DFF data input edit failed");
    require(loaded.set_input_value(clock, false), "DFF clock low setup failed");
    (void)output_value(runtime, loaded, output);
    require(loaded.set_input_value(clock, true), "DFF clock rising edge failed");
    require(output_value(runtime, loaded, output), "Loaded DFF did not capture data on rising edge");
}


void test_loaded_runtime_backend_matrix() {
    constexpr std::array backends{
        RuntimeBackend::InterpretedEvent,
        RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense,
        RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR,
        RuntimeBackend::CompiledNative,
    };

    const auto xor_text = serialize(make_xor_circuit(), "Backend XOR");
    for (const auto backend : backends) {
        CircuitEditor loaded;
        std::string name;
        PersistenceError error;
        require(hcs::editor::parse_circuit(xor_text, loaded, name, error),
                "XOR load failed for backend: " + error.describe());
        CircuitRuntime runtime(backend);
        runtime.synchronize(loaded);
        const auto a = loaded.components()[0].id;
        const auto b = loaded.components()[1].id;
        const auto output = loaded.components()[3].id;
        for (const bool av : {false, true}) {
            for (const bool bv : {false, true}) {
                require(loaded.set_input_value(a, av), "Could not update XOR input A");
                require(loaded.set_input_value(b, bv), "Could not update XOR input B");
                require(output_value(runtime, loaded, output) == (av != bv),
                        "Loaded XOR mismatch on runtime backend " + std::to_string(static_cast<int>(backend)));
            }
        }

        const auto dff_text = serialize(make_dff_circuit(), "Backend DFF");
        CircuitEditor dff_loaded;
        require(hcs::editor::parse_circuit(dff_text, dff_loaded, name, error),
                "DFF load failed for backend: " + error.describe());
        CircuitRuntime dff_runtime(backend);
        dff_runtime.synchronize(dff_loaded);
        const auto data = dff_loaded.components()[0].id;
        const auto clock = dff_loaded.components()[1].id;
        const auto output_pin_id = dff_loaded.components()[3].id;
        require(output_value(dff_runtime, dff_loaded, output_pin_id),
                "Loaded DFF initial state mismatch on runtime backend " + std::to_string(static_cast<int>(backend)));
        require(dff_loaded.set_dff_initial_state(dff_loaded.components()[2].id, false),
                "Could not edit DFF initial state in backend matrix");
        dff_runtime.synchronize(dff_loaded);
        require(!output_value(dff_runtime, dff_loaded, output_pin_id),
                "Loaded DFF reset mismatch on runtime backend " + std::to_string(static_cast<int>(backend)));
        require(dff_loaded.set_input_value(data, true), "Could not set DFF data input");
        require(dff_loaded.set_input_value(clock, false), "Could not set DFF clock low");
        (void)output_value(dff_runtime, dff_loaded, output_pin_id);
        require(dff_loaded.set_input_value(clock, true), "Could not set DFF clock high");
        require(output_value(dff_runtime, dff_loaded, output_pin_id),
                "Loaded DFF capture mismatch on runtime backend " + std::to_string(static_cast<int>(backend)));
    }
}

void test_atomic_file_save_load() {
    const auto path = std::filesystem::temp_directory_path() / "hcs-persistence-test.hcs";
    auto original = make_xor_circuit();
    PersistenceError error;
    require(hcs::editor::save_circuit_file(path, original, "File Round Trip", error), "Atomic save failed: " + error.describe());
    require(original.set_input_value(original.components().front().id, false), "Could not update model before atomic replacement");
    require(hcs::editor::save_circuit_file(path, original, "File Round Trip", error),
            "Atomic replacement save failed: " + error.describe());
    CircuitEditor loaded;
    std::string name;
    require(hcs::editor::load_circuit_file(path, loaded, name, error), "File load failed: " + error.describe());
    require(name == "File Round Trip", "File load lost circuit name");
    require(serialize(loaded, name) == serialize(original, name), "File round trip changed circuit contents");

    const auto before_failed_load = serialize(loaded, name);
    {
        std::ofstream malformed(path, std::ios::binary | std::ios::trunc);
        malformed << "HCS 1\ncircuit \"broken\"\nwire 1.out -> 2.in\n";
    }
    require(!hcs::editor::load_circuit_file(path, loaded, name, error), "Malformed file load unexpectedly succeeded");
    require(serialize(loaded, "File Round Trip") == before_failed_load,
            "Failed file load modified the existing editor model");
    require(name == "File Round Trip", "Failed file load changed the active circuit name");

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main() {
    try {
        test_round_trip_examples();
        test_deterministic_serialization();
        test_text_edit_apply_and_runtime();
        test_invalid_files_and_atomic_parse();
        test_loaded_dff_runtime();
        test_loaded_runtime_backend_matrix();
        test_atomic_file_save_load();
        std::cout << "All persistence tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Persistence test failure: " << error.what() << '\n';
        return 1;
    }
}
