#include "hcs/editor/custom_components.hpp"
#include "hcs/editor/runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

using namespace hcs::editor;

PinId input_pin(const CircuitEditor& editor, ComponentId component, std::size_t index = 0) {
    const auto* found = editor.find_component(component);
    return found && index < found->inputs.size() ? found->inputs[index].id : kInvalidPin;
}

PinId output_pin(const CircuitEditor& editor, ComponentId component, std::size_t index = 0) {
    const auto* found = editor.find_component(component);
    return found && index < found->outputs.size() ? found->outputs[index].id : kInvalidPin;
}

bool value(CircuitRuntime& runtime, const CircuitEditor& editor, ComponentId output) {
    return runtime.signal_for_pin(input_pin(editor, output)) == SignalState::One;
}

CustomComponentDefinition make_half_adder() {
    CustomComponentDefinition def;
    def.id = "arith.half-adder";
    def.name = "Half Adder";
    def.description = "Adds two one-bit values and exposes Sum and Carry";
    def.category = "Arithmetic";
    def.size = {160.0F, 110.0F};
    auto& c = def.implementation;
    require(c.add_component_with_id(0, ComponentType::Input, {0, 0}), "half adder input A");
    require(c.add_component_with_id(1, ComponentType::Input, {0, 80}), "half adder input B");
    require(c.add_component_with_id(2, ComponentType::Xor, {160, 0}), "half adder XOR");
    require(c.add_component_with_id(3, ComponentType::And, {160, 100}), "half adder AND");
    require(c.add_component_with_id(4, ComponentType::Output, {340, 0}), "half adder Sum");
    require(c.add_component_with_id(5, ComponentType::Output, {340, 100}), "half adder Carry");
    require(c.connect(output_pin(c, 0), input_pin(c, 2, 0)).has_value(), "HA A to XOR");
    require(c.connect(output_pin(c, 1), input_pin(c, 2, 1)).has_value(), "HA B to XOR");
    require(c.connect(output_pin(c, 0), input_pin(c, 3, 0)).has_value(), "HA A to AND");
    require(c.connect(output_pin(c, 1), input_pin(c, 3, 1)).has_value(), "HA B to AND");
    require(c.connect(output_pin(c, 2), input_pin(c, 4)).has_value(), "HA Sum output");
    require(c.connect(output_pin(c, 3), input_pin(c, 5)).has_value(), "HA Carry output");
    def.ports = {
        {"a", "A", PinDirection::Input, 0},
        {"b", "B", PinDirection::Input, 1},
        {"sum", "Sum", PinDirection::Output, 4},
        {"carry", "Carry", PinDirection::Output, 5},
    };
    return def;
}

CircuitEditor make_half_adder_root(ComponentId& a, ComponentId& b, ComponentId& instance,
                                   ComponentId& sum, ComponentId& carry,
                                   const ComponentLibrary& library) {
    CircuitEditor root;
    a = root.add_component(ComponentType::Input, {-100, 0});
    b = root.add_component(ComponentType::Input, {-100, 100});
    const auto* def = library.find("arith.half-adder");
    std::vector<CustomPortLayout> ports;
    for (const auto& p : def->ports) ports.push_back({p.id, p.name, p.direction});
    instance = root.add_custom_component(def->id, {120, 20}, ports, def->size);
    sum = root.add_component(ComponentType::Output, {400, 0});
    carry = root.add_component(ComponentType::Output, {400, 100});
    const auto* custom = root.find_component(instance);
    auto find_port = [&](PinDirection direction, const char* id) {
        const auto& pins = direction == PinDirection::Input ? custom->inputs : custom->outputs;
        for (const auto& pin : pins) if (pin.key == id) return pin.id;
        return kInvalidPin;
    };
    require(root.connect(output_pin(root, a), find_port(PinDirection::Input, "a")).has_value(), "root A wire");
    require(root.connect(output_pin(root, b), find_port(PinDirection::Input, "b")).has_value(), "root B wire");
    require(root.connect(find_port(PinDirection::Output, "sum"), input_pin(root, sum)).has_value(), "root Sum wire");
    require(root.connect(find_port(PinDirection::Output, "carry"), input_pin(root, carry)).has_value(), "root Carry wire");
    return root;
}

void test_half_adder_across_backends() {
    ComponentLibrary library;
    PersistenceError error;
    require(library.add(make_half_adder(), error), "add Half Adder definition");
    ComponentId a{}, b{}, instance{}, sum{}, carry{};
    auto root = make_half_adder_root(a, b, instance, sum, carry, library);
    // Reference graph: the same Half Adder explicitly expanded into built-in nodes.
    CircuitEditor expanded_reference;
    const auto ref_a = expanded_reference.add_component(ComponentType::Input, {0, 0});
    const auto ref_b = expanded_reference.add_component(ComponentType::Input, {0, 80});
    const auto ref_xor = expanded_reference.add_component(ComponentType::Xor, {160, 0});
    const auto ref_and = expanded_reference.add_component(ComponentType::And, {160, 100});
    const auto ref_sum = expanded_reference.add_component(ComponentType::Output, {340, 0});
    const auto ref_carry = expanded_reference.add_component(ComponentType::Output, {340, 100});
    require(expanded_reference.connect(output_pin(expanded_reference, ref_a), input_pin(expanded_reference, ref_xor, 0)).has_value(), "reference A to XOR");
    require(expanded_reference.connect(output_pin(expanded_reference, ref_b), input_pin(expanded_reference, ref_xor, 1)).has_value(), "reference B to XOR");
    require(expanded_reference.connect(output_pin(expanded_reference, ref_a), input_pin(expanded_reference, ref_and, 0)).has_value(), "reference A to AND");
    require(expanded_reference.connect(output_pin(expanded_reference, ref_b), input_pin(expanded_reference, ref_and, 1)).has_value(), "reference B to AND");
    require(expanded_reference.connect(output_pin(expanded_reference, ref_xor), input_pin(expanded_reference, ref_sum)).has_value(), "reference XOR output");
    require(expanded_reference.connect(output_pin(expanded_reference, ref_and), input_pin(expanded_reference, ref_carry)).has_value(), "reference AND output");

    const std::array backends{
        RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative,
    };
    for (const auto backend : backends) {
        CircuitRuntime runtime(backend);
        CircuitRuntime reference_runtime(backend);
        runtime.synchronize(root, library);
        reference_runtime.synchronize(expanded_reference);
        for (int av = 0; av < 2; ++av) {
            for (int bv = 0; bv < 2; ++bv) {
                (void)root.set_input_value(a, av != 0);
                (void)root.set_input_value(b, bv != 0);
                (void)expanded_reference.set_input_value(ref_a, av != 0);
                (void)expanded_reference.set_input_value(ref_b, bv != 0);
                runtime.synchronize(root, library);
                reference_runtime.synchronize(expanded_reference);
                const auto step = runtime.step();
                const auto reference_step = reference_runtime.step();
                require(step.converged && reference_step.converged, "Half Adder did not converge");
                const bool custom_sum = value(runtime, root, sum);
                const bool custom_carry = value(runtime, root, carry);
                const bool reference_sum = value(reference_runtime, expanded_reference, ref_sum);
                const bool reference_carry = value(reference_runtime, expanded_reference, ref_carry);
                require(custom_sum == ((av ^ bv) != 0), "Half Adder Sum truth table mismatch");
                require(custom_carry == ((av & bv) != 0), "Half Adder Carry truth table mismatch");
                require(custom_sum == reference_sum && custom_carry == reference_carry,
                        "custom Half Adder must match the manually expanded built-in circuit");
            }
        }
    }
}

CustomComponentDefinition make_full_adder(const ComponentLibrary& library) {
    CustomComponentDefinition def;
    def.id = "arith.full-adder";
    def.name = "Full Adder";
    def.description = "Full adder composed from two Half Adders and an OR gate";
    def.category = "Arithmetic";
    auto& c = def.implementation;
    require(c.add_component_with_id(0, ComponentType::Input, {0, 0}), "FA A");
    require(c.add_component_with_id(1, ComponentType::Input, {0, 80}), "FA B");
    require(c.add_component_with_id(2, ComponentType::Input, {0, 160}), "FA Cin");
    std::vector<CustomPortLayout> ha_ports;
    for (const auto& p : library.find("arith.half-adder")->ports) ha_ports.push_back({p.id, p.name, p.direction});
    require(c.add_custom_component_with_id(3, "arith.half-adder", {160, 0}, ha_ports), "FA HA1");
    require(c.add_custom_component_with_id(4, "arith.half-adder", {400, 0}, ha_ports), "FA HA2");
    require(c.add_component_with_id(5, ComponentType::Or, {650, 100}), "FA OR");
    require(c.add_component_with_id(6, ComponentType::Output, {850, 0}), "FA Sum");
    require(c.add_component_with_id(7, ComponentType::Output, {850, 130}), "FA Carry");
    auto port = [&](ComponentId id, PinDirection direction, const char* key) {
        const auto* component = c.find_component(id);
        const auto& pins = direction == PinDirection::Input ? component->inputs : component->outputs;
        for (const auto& p : pins) if (p.key == key) return p.id;
        return kInvalidPin;
    };
    require(c.connect(output_pin(c, 0), port(3, PinDirection::Input, "a")).has_value(), "FA A->HA1");
    require(c.connect(output_pin(c, 1), port(3, PinDirection::Input, "b")).has_value(), "FA B->HA1");
    require(c.connect(port(3, PinDirection::Output, "sum"), port(4, PinDirection::Input, "a")).has_value(), "FA HA1 Sum->HA2 A");
    require(c.connect(output_pin(c, 2), port(4, PinDirection::Input, "b")).has_value(), "FA Cin->HA2 B");
    require(c.connect(port(3, PinDirection::Output, "carry"), input_pin(c, 5, 0)).has_value(), "FA C1->OR");
    require(c.connect(port(4, PinDirection::Output, "carry"), input_pin(c, 5, 1)).has_value(), "FA C2->OR");
    require(c.connect(port(4, PinDirection::Output, "sum"), input_pin(c, 6)).has_value(), "FA Sum output");
    require(c.connect(output_pin(c, 5), input_pin(c, 7)).has_value(), "FA Carry output");
    def.ports = {
        {"a", "A", PinDirection::Input, 0},
        {"b", "B", PinDirection::Input, 1},
        {"cin", "Carry In", PinDirection::Input, 2},
        {"sum", "Sum", PinDirection::Output, 6},
        {"carry", "Carry Out", PinDirection::Output, 7},
    };
    return def;
}

void test_nested_custom_components() {
    ComponentLibrary library;
    PersistenceError error;
    require(library.add(make_half_adder(), error), "add nested base definition");
    require(library.add(make_full_adder(library), error), "add Full Adder definition");
    CircuitEditor root;
    const auto a = root.add_component(ComponentType::Input, {0, 0});
    const auto b = root.add_component(ComponentType::Input, {0, 80});
    const auto cin = root.add_component(ComponentType::Input, {0, 160});
    std::vector<CustomPortLayout> ports;
    for (const auto& p : library.find("arith.full-adder")->ports) ports.push_back({p.id, p.name, p.direction});
    const auto fa = root.add_custom_component("arith.full-adder", {180, 20}, ports);
    const auto sum = root.add_component(ComponentType::Output, {500, 0});
    const auto carry = root.add_component(ComponentType::Output, {500, 100});
    const auto* inst = root.find_component(fa);
    auto pin = [&](PinDirection dir, const char* id) {
        const auto& list = dir == PinDirection::Input ? inst->inputs : inst->outputs;
        for (const auto& p : list) if (p.key == id) return p.id;
        return kInvalidPin;
    };
    require(root.connect(output_pin(root, a), pin(PinDirection::Input, "a")).has_value(), "nested A");
    require(root.connect(output_pin(root, b), pin(PinDirection::Input, "b")).has_value(), "nested B");
    require(root.connect(output_pin(root, cin), pin(PinDirection::Input, "cin")).has_value(), "nested Cin");
    require(root.connect(pin(PinDirection::Output, "sum"), input_pin(root, sum)).has_value(), "nested Sum");
    require(root.connect(pin(PinDirection::Output, "carry"), input_pin(root, carry)).has_value(), "nested Carry");

    const std::array backends{
        RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative,
    };
    for (const auto backend : backends) {
        CircuitRuntime runtime(backend);
        runtime.synchronize(root, library);
        for (int av = 0; av < 2; ++av) for (int bv = 0; bv < 2; ++bv) for (int cv = 0; cv < 2; ++cv) {
            (void)root.set_input_value(a, av != 0);
            (void)root.set_input_value(b, bv != 0);
            (void)root.set_input_value(cin, cv != 0);
            runtime.synchronize(root, library);
            require(runtime.step().converged, "nested Full Adder did not converge");
            const int total = av + bv + cv;
            require(value(runtime, root, sum) == ((total & 1) != 0), "nested Full Adder Sum mismatch");
            require(value(runtime, root, carry) == (total >= 2), "nested Full Adder Carry mismatch");
        }
    }
}

CustomComponentDefinition make_dff_component(std::string id) {
    CustomComponentDefinition def;
    def.id = std::move(id);
    def.name = "DFF Cell";
    def.description = "A one-bit register cell using the built-in DFF";
    def.category = "Sequential";
    auto& c = def.implementation;
    require(c.add_component_with_id(0, ComponentType::Input, {0, 0}), "DFF cell D terminal");
    require(c.add_component_with_id(1, ComponentType::Input, {0, 80}), "DFF cell CLK terminal");
    require(c.add_component_with_id(2, ComponentType::DFlipFlop, {180, 20}), "DFF cell DFF");
    require(c.add_component_with_id(3, ComponentType::Output, {360, 20}), "DFF cell output");
    require(c.connect(output_pin(c, 0), input_pin(c, 2, 0)).has_value(), "DFF cell D wire");
    require(c.connect(output_pin(c, 1), input_pin(c, 2, 1)).has_value(), "DFF cell clock wire");
    require(c.connect(output_pin(c, 2), input_pin(c, 3)).has_value(), "DFF cell Q wire");
    def.ports = {
        {"d", "D", PinDirection::Input, 0},
        {"clk", "Clock", PinDirection::Input, 1},
        {"q", "Q", PinDirection::Output, 3},
    };
    return def;
}

void test_independent_sequential_instances() {
    ComponentLibrary library;
    PersistenceError error;
    require(library.add(make_dff_component("seq.dff-cell"), error), "add DFF cell");
    CircuitEditor root;
    const auto d1 = root.add_component(ComponentType::Input, {0, 0});
    const auto c1 = root.add_component(ComponentType::Input, {0, 50});
    const auto d2 = root.add_component(ComponentType::Input, {0, 140});
    const auto c2 = root.add_component(ComponentType::Input, {0, 190});
    std::vector<CustomPortLayout> ports;
    for (const auto& p : library.find("seq.dff-cell")->ports) ports.push_back({p.id, p.name, p.direction});
    const auto i1 = root.add_custom_component("seq.dff-cell", {180, 0}, ports);
    const auto i2 = root.add_custom_component("seq.dff-cell", {180, 150}, ports);
    const auto o1 = root.add_component(ComponentType::Output, {420, 0});
    const auto o2 = root.add_component(ComponentType::Output, {420, 150});
    auto inst_pin = [&](ComponentId id, PinDirection dir, const char* key) {
        const auto* inst = root.find_component(id);
        const auto& list = dir == PinDirection::Input ? inst->inputs : inst->outputs;
        for (const auto& p : list) if (p.key == key) return p.id;
        return kInvalidPin;
    };
    for (const auto& item : std::array<std::tuple<ComponentId, ComponentId, const char*>, 4>{
             std::tuple{d1, i1, "d"}, std::tuple{c1, i1, "clk"}, std::tuple{d2, i2, "d"}, std::tuple{c2, i2, "clk"}})
        require(root.connect(output_pin(root, std::get<0>(item)), inst_pin(std::get<1>(item), PinDirection::Input, std::get<2>(item))).has_value(), "DFF cell input connection");
    require(root.connect(inst_pin(i1, PinDirection::Output, "q"), input_pin(root, o1)).has_value(), "DFF cell output 1");
    require(root.connect(inst_pin(i2, PinDirection::Output, "q"), input_pin(root, o2)).has_value(), "DFF cell output 2");

    const std::array backends{
        RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
        RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
        RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative,
    };
    for (const auto backend : backends) {
        (void)root.set_input_value(d1, false);
        (void)root.set_input_value(c1, false);
        (void)root.set_input_value(d2, false);
        (void)root.set_input_value(c2, false);
        CircuitRuntime runtime(backend);
        runtime.synchronize(root, library);
        (void)runtime.step();
        (void)root.set_input_value(d1, true);
        (void)root.set_input_value(c1, true);
        (void)root.set_input_value(d2, false);
        (void)root.set_input_value(c2, false);
        runtime.synchronize(root, library);
        (void)runtime.step();
        require(value(runtime, root, o1), "first DFF custom instance did not capture its input");
        require(!value(runtime, root, o2), "second DFF custom instance must remain independent");
        (void)root.set_input_value(d2, true);
        (void)root.set_input_value(c2, true);
        runtime.synchronize(root, library);
        (void)runtime.step();
        require(value(runtime, root, o1), "first DFF state changed with second clock");
        require(value(runtime, root, o2), "second DFF custom instance failed to capture its own input");
    }
}

void test_definition_and_circuit_persistence() {
    ComponentLibrary library;
    PersistenceError error;
    auto definition = make_half_adder();
    definition.symbol = make_default_symbol(definition.size, {{"a","input"},{"b","input"},
        {"sum","output"},{"carry","output"}});
    SymbolShape custom_body;
    custom_body.id = "hex_body";
    custom_body.type = SymbolShapeType::Polygon;
    custom_body.points = {{80,8},{150,30},{150,80},{80,102},{10,80},{10,30}};
    definition.symbol.objects = {custom_body};
    SymbolText title;
    title.id = "caption"; title.value = "HALF ADDER"; title.x = 80; title.y = 52;
    title.size = 12; title.alignment = SymbolTextAlignment::Center;
    definition.symbol.objects.emplace_back(title);
    require(library.add(definition, error), "add persistence fixture definition");
    std::string component_text;
    require(serialize_component_definition(definition, library, component_text, error), "serialize component definition");
    CustomComponentDefinition parsed_definition;
    require(parse_component_definition(component_text, library, parsed_definition, error), "parse component definition");
    require(component_text.find("symbol-begin\nHCSS 1\n") != std::string::npos,
            "component definition embeds its versioned symbol section");
    require(parsed_definition.symbol.objects.size() == 2 && parsed_definition.symbol.ports.size() == 4,
            "component definition roundtrip retains custom shapes, text and port coordinates");
    require(parsed_definition.symbol.objects.front().index() == definition.symbol.objects.front().index(),
            "component definition roundtrip retains vector object kind");
    std::string second_text;
    require(serialize_component_definition(parsed_definition, library, second_text, error), "reserialize component definition");
    require(component_text == second_text, "component definition serialization must be deterministic");

    ComponentId a{}, b{}, instance{}, sum{}, carry{};
    auto root = make_half_adder_root(a, b, instance, sum, carry, library);
    std::string circuit_text;
    require(serialize_circuit(root, "Half Adder Instance", library, circuit_text, error), "serialize custom circuit");
    CircuitEditor loaded;
    std::string name;
    require(parse_circuit(circuit_text, loaded, name, library, error), "parse custom circuit");
    require(name == "Half Adder Instance", "custom circuit name was not preserved");
    require(loaded.components().size() == root.components().size(), "custom circuit component count mismatch");
    std::string again;
    require(serialize_circuit(loaded, name, library, again, error), "reserialize custom circuit");
    require(circuit_text == again, "custom circuit serialization must be deterministic");
}

void test_selection_conversion_and_library_metadata() {
    ComponentLibrary library;
    PersistenceError error;
    CircuitEditor root;
    const auto a = root.add_component(ComponentType::Input, {0, 0});
    const auto b = root.add_component(ComponentType::Input, {0, 80});
    const auto gate = root.add_component(ComponentType::Xor, {170, 30});
    const auto output = root.add_component(ComponentType::Output, {350, 30});
    require(root.connect(output_pin(root, a), input_pin(root, gate, 0)).has_value(), "selection wire A");
    require(root.connect(output_pin(root, b), input_pin(root, gate, 1)).has_value(), "selection wire B");
    require(root.connect(output_pin(root, gate), input_pin(root, output)).has_value(), "selection wire output");
    CustomComponentDefinition def;
    def.id = "custom.xor-block";
    def.name = "XOR Block";
    def.category = "Logic Gates";
    ComponentId instance = kInvalidComponent;
    require(create_custom_component_from_selection(root, {a, b, gate, output}, def, library, instance, error), "selection-to-component conversion");
    require(root.components().size() == 1 && root.find_component(instance)->type == ComponentType::Custom,
            "selection conversion did not replace the region with a custom instance");
    const auto catalog = library.catalog();
    require(std::any_of(catalog.begin(), catalog.end(), [](const ComponentMetadata& item) {
        return item.id == "custom.xor-block" && item.input_count == 2 && item.output_count == 1 && !item.built_in;
    }), "component library catalog is missing custom metadata");

    CircuitEditor parent;
    const auto x = parent.add_component(ComponentType::Input, {0, 0});
    const auto y = parent.add_component(ComponentType::Input, {0, 80});
    std::vector<CustomPortLayout> instance_ports;
    for (const auto& p : library.find("custom.xor-block")->ports) instance_ports.push_back({p.id, p.name, p.direction});
    const auto inst = parent.add_custom_component("custom.xor-block", {160, 0}, instance_ports);
    const auto sink = parent.add_component(ComponentType::Output, {400, 0});
    const auto* instance_component = parent.find_component(inst);
    require(instance_component && instance_component->inputs.size() == 2 && instance_component->outputs.size() == 1,
            "created selection instance has incorrect external ports");
    require(parent.connect(output_pin(parent, x), instance_component->inputs[0].id).has_value(), "selection instance input A");
    require(parent.connect(output_pin(parent, y), instance_component->inputs[1].id).has_value(), "selection instance input B");
    require(parent.connect(instance_component->outputs[0].id, input_pin(parent, sink)).has_value(), "selection instance output");
    CircuitRuntime runtime(RuntimeBackend::OptimizedIREvent);
    runtime.synchronize(parent, library);
    for (int xv = 0; xv < 2; ++xv) for (int yv = 0; yv < 2; ++yv) {
        (void)parent.set_input_value(x, xv != 0);
        (void)parent.set_input_value(y, yv != 0);
        runtime.synchronize(parent, library);
        require(runtime.step().converged, "selection-created component did not converge");
        require(value(runtime, parent, sink) == ((xv ^ yv) != 0), "selection-created component logic mismatch");
    }
}

void test_empty_definition_and_stable_port_rename() {
    CustomComponentDefinition definition;
    PersistenceError error;
    require(create_empty_component_definition("custom.empty", "Empty Block", "Custom", definition, error),
            "create an empty custom definition");
    require(add_custom_component_port(definition, "data_a", "A", PinDirection::Input, error),
            "add the first external input port");
    require(add_custom_component_port(definition, "data_b", "B", PinDirection::Input, error),
            "add the second external input port");
    require(add_custom_component_port(definition, "result", "Result", PinDirection::Output, error),
            "add the external output port");
    const auto input_a_terminal = definition.ports[0].terminal_component;
    const auto input_b_terminal = definition.ports[1].terminal_component;
    const auto output_terminal = definition.ports[2].terminal_component;
    require(definition.implementation.find_component(input_a_terminal) &&
            definition.implementation.find_component(input_b_terminal) &&
            definition.implementation.find_component(output_terminal),
            "empty definition terminal components must exist");
    const auto xor_gate = definition.implementation.add_component(ComponentType::Xor, {160, 60});
    require(xor_gate != kInvalidComponent, "add XOR to the empty definition");
    require(definition.implementation.connect(output_pin(definition.implementation, input_a_terminal),
                                               input_pin(definition.implementation, xor_gate, 0)).has_value(),
            "connect empty definition input A");
    require(definition.implementation.connect(output_pin(definition.implementation, input_b_terminal),
                                               input_pin(definition.implementation, xor_gate, 1)).has_value(),
            "connect empty definition input B");
    require(definition.implementation.connect(output_pin(definition.implementation, xor_gate),
                                               input_pin(definition.implementation, output_terminal)).has_value(),
            "connect empty definition output");
    require(rename_custom_component_port(definition, "data_a", "First Operand", error),
            "rename a port without changing its ID");
    require(definition.ports.front().id == "data_a" && definition.ports.front().name == "First Operand",
            "port rename must preserve the stable ID and update only the display name");
    ComponentLibrary library;
    require(library.add(definition, error), "add edited empty-definition fixture to the library");
    require(remove_custom_component_port(definition, "data_b", error), "remove an external port and its terminal");
    require(definition.implementation.find_component(1) == nullptr,
            "removing a port must remove its internal terminal component");
}

void test_definition_layout_migration_is_safe() {
    ComponentLibrary library;
    PersistenceError error;
    auto definition = make_half_adder();
    require(library.add(definition, error), "add component for layout migration");
    ComponentId a{}, b{}, instance{}, sum{}, carry{};
    auto root = make_half_adder_root(a, b, instance, sum, carry, library);
    const auto* before = root.find_component(instance);
    require(before && before->inputs.size() == 2 && before->outputs.size() == 2,
            "initial custom instance layout is incorrect");
    const auto existing_a_pin = before->inputs[0].id;
    const auto old_wire_count = root.wires().size();

    require(add_custom_component_port(definition, "enable", "Enable", PinDirection::Input, error),
            "add compatible custom component port");
    std::vector<CustomPortLayout> expanded_layout;
    for (const auto& port : definition.ports)
        expanded_layout.push_back({port.id, port.name, port.direction});
    require(root.update_custom_component_layout(definition.id, expanded_layout, definition.size),
            "migrate current root instances to an added port");
    const auto* after_add = root.find_component(instance);
    require(after_add && after_add->inputs.size() == 3 && after_add->outputs.size() == 2,
            "new port was not added to existing instances");
    require(after_add->inputs[0].id == existing_a_pin && root.wires().size() == old_wire_count,
            "compatible migration must preserve stable pin IDs and existing wires");

    auto incompatible_layout = expanded_layout;
    incompatible_layout.erase(std::remove_if(incompatible_layout.begin(), incompatible_layout.end(),
        [](const CustomPortLayout& port) { return port.id == "a"; }), incompatible_layout.end());
    require(!root.update_custom_component_layout(definition.id, incompatible_layout, definition.size),
            "removing a connected port must be rejected");
    require(root.find_component(instance)->inputs.size() == 3 && root.wires().size() == old_wire_count,
            "rejected port migration must leave all instances and connections unchanged");

    require(rename_custom_component_port(definition, "a", "Operand A", error),
            "rename a port display label");
    std::vector<CustomPortLayout> renamed_layout;
    for (const auto& port : definition.ports)
        renamed_layout.push_back({port.id, port.name, port.direction});
    require(root.update_custom_component_layout(definition.id, renamed_layout, definition.size),
            "apply display-name-only port update");
    require(root.find_component(instance)->inputs[0].id == existing_a_pin,
            "renaming a port display label must preserve its connected pin ID");
}

void test_definition_apply_is_transactional() {
    ComponentLibrary library;
    PersistenceError error;
    require(library.add(make_half_adder(), error), "add transactional Half Adder");

    ComponentId a{}, b{}, instance{}, sum{}, carry{};
    auto root = make_half_adder_root(a, b, instance, sum, carry, library);
    const auto* original_instance = root.find_component(instance);
    require(original_instance != nullptr, "find original Half Adder instance");
    const auto stable_a_pin = std::find_if(original_instance->inputs.begin(), original_instance->inputs.end(),
        [](const Pin& pin) { return pin.key == "a"; })->id;
    const auto old_wire_count = root.wires().size();

    auto edited = *library.find("arith.half-adder");
    require(rename_custom_component_port(edited, "a", "Operand A", error), "rename port in definition editor copy");
    require(apply_component_definition_transactionally(library, root, edited, error),
            "apply compatible definition update transactionally");
    const auto* migrated_instance = root.find_component(instance);
    require(migrated_instance && migrated_instance->inputs.size() == 2, "updated instance should retain both inputs");
    const auto migrated_a = std::find_if(migrated_instance->inputs.begin(), migrated_instance->inputs.end(),
        [](const Pin& pin) { return pin.key == "a"; });
    require(migrated_a != migrated_instance->inputs.end() && migrated_a->id == stable_a_pin,
            "compatible port rename must preserve stable pin ID");
    require(root.wires().size() == old_wire_count, "compatible port rename must preserve all wires");
    require(library.find("arith.half-adder")->ports.front().name == "Operand A",
            "library should publish the updated definition");

    auto incompatible = *library.find("arith.half-adder");
    require(remove_custom_component_port(incompatible, "a", error), "remove input from edit candidate");
    require(!apply_component_definition_transactionally(library, root, incompatible, error),
            "connected port removal must reject the whole definition update");
    require(error.message.find("connected port") != std::string::npos,
            "incompatible update should explain the connected-port conflict");
    require(library.find("arith.half-adder")->ports.size() == 4 &&
            library.find("arith.half-adder")->ports.front().name == "Operand A",
            "failed update must leave the published library definition unchanged");
    require(root.find_component(instance)->inputs.size() == 2 && root.wires().size() == old_wire_count,
            "failed update must leave all root instances and wires unchanged");

    CustomComponentDefinition empty;
    require(create_empty_component_definition("user.empty-cell", "Empty Cell", "Custom", empty, error),
            "create a new empty definition");
    require(add_custom_component_port(empty, "data", "Data", PinDirection::Input, error),
            "add input to empty definition");
    require(add_custom_component_port(empty, "result", "Result", PinDirection::Output, error),
            "add output to empty definition");
    require(apply_component_definition_transactionally(library, root, empty, error),
            "publish new empty definition");
    require(library.find("user.empty-cell") != nullptr, "new definition should be in library");

    CustomComponentDefinition many_ports;
    require(create_empty_component_definition("user.many-ports", "Many Ports", "Custom", many_ports, error),
            "create large-port definition");
    for (std::size_t i = 0; i < 8; ++i) {
        require(add_custom_component_port(many_ports, "in" + std::to_string(i), "Input " + std::to_string(i),
                                          PinDirection::Input, error), "add input port to large definition");
        require(add_custom_component_port(many_ports, "out" + std::to_string(i), "Output " + std::to_string(i),
                                          PinDirection::Output, error), "add output port to large definition");
    }
    require(many_ports.size.y >= 48.0F + 32.0F * 8.0F,
            "component visual height should grow to fit external port layout");
    for (std::size_t i = 8; i < 64; ++i)
        require(add_custom_component_port(many_ports, "in" + std::to_string(i), "Input " + std::to_string(i),
                                          PinDirection::Input, error), "ports up to configured safety limit should be accepted");
    require(!add_custom_component_port(many_ports, "in64", "Input 64", PinDirection::Input, error),
            "port count above safety limit must be rejected");
}

void test_component_library_load_is_transactional() {
    const auto directory = std::filesystem::temp_directory_path() / "hcs-phase54-transactional-library-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory);

    ComponentLibrary library;
    PersistenceError error;
    require(library.add(make_half_adder(), error), "seed existing library definition");
    CustomComponentDefinition candidate;
    candidate.id = "user.good-definition";
    candidate.name = "Good Definition";
    candidate.description = "Valid library file";
    candidate.category = "Custom";
    require(save_component_definition_file(directory / "good.hcscomponent", candidate, library, error),
            "write valid temporary library file");
    {
        std::ofstream invalid(directory / "broken.hcscomponent", std::ios::binary);
        invalid << "HCSC 99\nid invalid.version\n";
    }

    std::vector<std::string> diagnostics;
    require(!library.load_directory(directory, diagnostics), "malformed library file should reject directory import");
    require(library.find("arith.half-adder") != nullptr && library.find("user.good-definition") == nullptr,
            "failed directory import must not publish a partial library");
    require(!diagnostics.empty(), "failed directory import should report diagnostics");
    std::filesystem::remove_all(directory, cleanup_error);
}

void test_library_dependency_loading_and_limits() {
    const auto directory = std::filesystem::temp_directory_path() / "hcs-phase54-component-library-test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::create_directories(directory);

    ComponentLibrary source;
    PersistenceError error;
    require(source.add(make_half_adder(), error), "add base component to source library");
    require(source.add(make_full_adder(source), error), "add nested component to source library");
    require(source.save_definition(directory, "arith.full-adder", error), "save Full Adder dependency first");
    require(source.save_definition(directory, "arith.half-adder", error), "save Half Adder dependency second");

    ComponentLibrary loaded;
    std::vector<std::string> diagnostics;
    require(loaded.load_directory(directory, diagnostics), "load dependency-ordered custom library");
    require(diagnostics.empty(), "valid component library should not produce diagnostics");
    require(loaded.find("arith.full-adder") && loaded.find("arith.half-adder"),
            "loader must resolve forward file dependencies across passes");

    ComponentId a{}, b{}, cin{}, sum{}, carry{};
    CircuitEditor root;
    a = root.add_component(ComponentType::Input, {0, 0});
    b = root.add_component(ComponentType::Input, {0, 60});
    cin = root.add_component(ComponentType::Input, {0, 120});
    std::vector<CustomPortLayout> ports;
    for (const auto& port : loaded.find("arith.full-adder")->ports)
        ports.push_back({port.id, port.name, port.direction});
    const auto instance = root.add_custom_component("arith.full-adder", {160, 0}, ports);
    sum = root.add_component(ComponentType::Output, {480, 0});
    carry = root.add_component(ComponentType::Output, {480, 80});
    const auto* custom = root.find_component(instance);
    auto custom_pin = [&](PinDirection direction, const char* id) {
        const auto& pins = direction == PinDirection::Input ? custom->inputs : custom->outputs;
        const auto found = std::find_if(pins.begin(), pins.end(), [&](const Pin& pin) { return pin.key == id; });
        return found == pins.end() ? kInvalidPin : found->id;
    };
    require(root.connect(output_pin(root, a), custom_pin(PinDirection::Input, "a")).has_value(), "limit test A wire");
    require(root.connect(output_pin(root, b), custom_pin(PinDirection::Input, "b")).has_value(), "limit test B wire");
    require(root.connect(output_pin(root, cin), custom_pin(PinDirection::Input, "cin")).has_value(), "limit test Cin wire");
    require(root.connect(custom_pin(PinDirection::Output, "sum"), input_pin(root, sum)).has_value(), "limit test Sum wire");
    require(root.connect(custom_pin(PinDirection::Output, "carry"), input_pin(root, carry)).has_value(), "limit test Carry wire");

    HierarchyError hierarchy_error;
    HierarchyLimits depth_limit;
    depth_limit.maximum_depth = 1;
    require(!validate_component_hierarchy(root, loaded, depth_limit, hierarchy_error),
            "maximum hierarchy depth must be enforced");
    require(hierarchy_error.message.find("depth") != std::string::npos,
            "maximum depth error should be descriptive");

    HierarchyLimits expansion_limit;
    expansion_limit.maximum_expanded_components = 1;
    ExpandedCircuit expanded;
    require(!expand_component_hierarchy(root, loaded, expanded, hierarchy_error, expansion_limit),
            "maximum expanded component count must be enforced");
    require(hierarchy_error.message.find("expanded component") != std::string::npos,
            "expanded-count error should be descriptive");

    HierarchyLimits dependency_limit;
    dependency_limit.maximum_dependencies = 2;
    require(!expand_component_hierarchy(root, loaded, expanded, hierarchy_error, dependency_limit),
            "maximum hierarchy dependency count must be enforced");

    // A failed parse caused by a missing definition must leave an existing model untouched.
    CircuitEditor existing;
    const auto retained = existing.add_component(ComponentType::Input, {12, 34});
    std::string serialized;
    require(serialize_circuit(root, "Needs Full Adder", loaded, serialized, error), "serialize a custom dependency reference");
    ComponentLibrary empty_library;
    std::string parsed_name;
    require(!parse_circuit(serialized, existing, parsed_name, empty_library, error),
            "missing custom dependency must reject a circuit load");
    require(existing.components().size() == 1 && existing.find_component(retained),
            "failed missing-dependency load must preserve the active editor model");

    std::filesystem::remove_all(directory, cleanup_error);
}

void test_documented_component_examples() {
    ComponentLibrary library;
    std::vector<std::string> diagnostics;
    const auto directory = std::filesystem::path(HCS_SOURCE_DIR) / "examples" / "components";
    require(library.load_directory(directory, diagnostics), "documented component examples should load with dependencies resolved");
    require(library.find("arithmetic.half-adder") != nullptr, "Half Adder example should load");
    require(library.find("arithmetic.full-adder") != nullptr, "Full Adder example should load after its dependency");
    require(library.find("sequential.dff-cell") != nullptr, "DFF Cell example should load");

    for (const auto* definition : library.definitions()) {
        std::string first;
        std::string second;
        PersistenceError error;
        require(serialize_component_definition(*definition, library, first, error), "serialize documented component example");
        require(serialize_component_definition(*definition, library, second, error), "serialize documented component example again");
        require(first == second, "documented component examples must serialize deterministically");
    }
}

void test_hierarchy_rejections() {
    ComponentLibrary library;
    PersistenceError error;
    CustomComponentDefinition recursive;
    recursive.id = "bad.recursive";
    recursive.name = "Recursive";
    require(recursive.implementation.add_custom_component_with_id(0, recursive.id, {0, 0}, {}), "create recursive reference");
    require(library.add(recursive, error), "store recursive fixture so validation can reject it");
    require(!library.add(recursive, error) && error.message.find("duplicate") != std::string::npos,
            "duplicate definition IDs must be rejected by the library");

    CircuitEditor root;
    require(root.add_custom_component_with_id(0, recursive.id, {0, 0}, {}), "create recursive root instance");
    HierarchyError hierarchy_error;
    require(!validate_component_hierarchy(root, library, {}, hierarchy_error), "recursive component dependency should be rejected");
    require(hierarchy_error.message.find("recursive") != std::string::npos, "recursive validation should explain the problem");

    // Two distinct definitions that refer to each other form a circular dependency.
    CustomComponentDefinition cycle_a;
    cycle_a.id = "bad.cycle-a";
    cycle_a.name = "Cycle A";
    require(cycle_a.implementation.add_custom_component_with_id(0, "bad.cycle-b", {0, 0}, {}), "create A to B reference");
    CustomComponentDefinition cycle_b;
    cycle_b.id = "bad.cycle-b";
    cycle_b.name = "Cycle B";
    require(cycle_b.implementation.add_custom_component_with_id(0, "bad.cycle-a", {0, 0}, {}), "create B to A reference");
    require(library.add(cycle_a, error) && library.add(cycle_b, error), "store circular dependency fixtures");
    CircuitEditor cycle_root;
    require(cycle_root.add_custom_component_with_id(0, "bad.cycle-a", {0, 0}, {}), "create circular root instance");
    require(!validate_component_hierarchy(cycle_root, library, {}, hierarchy_error) &&
            hierarchy_error.message.find("recursive") != std::string::npos,
            "indirect circular dependencies must be rejected");

    auto duplicate_ports = make_half_adder();
    duplicate_ports.id = "bad.duplicate-ports";
    duplicate_ports.ports[1].id = duplicate_ports.ports[0].id;
    require(!library.add(duplicate_ports, error) && error.message.find("duplicate external port ID") != std::string::npos,
            "duplicate external port IDs must be rejected");

    auto invalid_terminal = make_half_adder();
    invalid_terminal.id = "bad.invalid-terminal";
    invalid_terminal.ports.front().terminal_component = 999999;
    require(!library.add(invalid_terminal, error) && error.message.find("missing terminal component") != std::string::npos,
            "missing interface terminal references must be rejected");

    ComponentLibrary valid_library;
    require(valid_library.add(make_half_adder(), error), "add valid definition for instance-layout rejection");
    CircuitEditor bad_layout;
    const std::vector<CustomPortLayout> wrong_ports{{"not-a-port", "Invalid", PinDirection::Input}};
    require(bad_layout.add_custom_component_with_id(0, "arith.half-adder", {0, 0}, wrong_ports),
            "create an instance with an incompatible external port layout");
    require(!validate_component_hierarchy(bad_layout, valid_library, {}, hierarchy_error) &&
            hierarchy_error.message.find("port layout") != std::string::npos,
            "invalid instance port mappings must be rejected");

    CustomComponentDefinition orphan;
    orphan.id = "bad.orphan";
    orphan.name = "Orphan Definition";
    require(orphan.implementation.add_custom_component_with_id(0, "missing.nested", {0, 0}, {}),
            "create a definition with a missing nested dependency");
    require(library.add(orphan, error), "store nested missing-dependency fixture");
    CircuitEditor nested_missing;
    require(nested_missing.add_custom_component_with_id(0, "bad.orphan", {0, 0}, {}),
            "create root instance with a missing nested dependency");
    require(!validate_component_hierarchy(nested_missing, library, {}, hierarchy_error) &&
            hierarchy_error.message.find("missing nested") != std::string::npos,
            "missing nested component definitions must be rejected");

    CircuitEditor missing;
    require(missing.add_custom_component_with_id(0, "missing.definition", {0, 0}, {}), "create missing reference");
    require(!validate_component_hierarchy(missing, library, {}, hierarchy_error), "missing component dependency should be rejected");
    require(hierarchy_error.message.find("missing") != std::string::npos, "missing dependency should be reported");
}

} // namespace

int main() {
    try {
        test_half_adder_across_backends();
        test_nested_custom_components();
        test_independent_sequential_instances();
        test_definition_and_circuit_persistence();
        test_selection_conversion_and_library_metadata();
        test_empty_definition_and_stable_port_rename();
        test_definition_layout_migration_is_safe();
        test_definition_apply_is_transactional();
        test_component_library_load_is_transactional();
        test_library_dependency_loading_and_limits();
        test_hierarchy_rejections();
        test_documented_component_examples();
    } catch (const std::exception& exception) {
        std::cerr << "Custom component test failure: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "All custom component tests passed.\n";
    return EXIT_SUCCESS;
}
