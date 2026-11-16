#include "hcs/editor/runtime.hpp"
#include "hcs/editor/custom_components.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace hcs::editor {

CircuitRuntime::CircuitRuntime() = default;
CircuitRuntime::CircuitRuntime(RuntimeBackend backend) : backend_(backend) {}

SimulatorConfig CircuitRuntime::make_config() const {
    SimulatorConfig config;
    config.acceleration = acceleration_;
    switch (backend_) {
        case RuntimeBackend::InterpretedEvent:
            config.backend = Backend::Interpreted;
            config.strategy = InterpreterStrategy::EventDriven;
            break;
        case RuntimeBackend::InterpretedDense:
            config.backend = Backend::Interpreted;
            config.strategy = InterpreterStrategy::Dense;
            break;
        case RuntimeBackend::OptimizedIRDense:
            config.backend = Backend::OptimizedIR;
            config.strategy = InterpreterStrategy::Dense;
            break;
        case RuntimeBackend::OptimizedIREvent:
            config.backend = Backend::OptimizedIR;
            config.strategy = InterpreterStrategy::EventDriven;
            break;
        case RuntimeBackend::HybridIR:
            config.backend = Backend::HybridIR;
            config.strategy = InterpreterStrategy::EventDriven;
            break;
        case RuntimeBackend::CompiledNative:
            config.backend = Backend::Compiled;
            config.strategy = InterpreterStrategy::Dense;
            config.native_options.cache_directory = ".hcs-gui-cache";
            break;
    }
    return config;
}

void CircuitRuntime::invalidate() noexcept {
    synchronized_revision_ = 0;
    synchronized_input_revision_ = 0;
    synchronized_library_revision_ = 0;
}

void CircuitRuntime::set_acceleration(const AccelerationConfig& config) noexcept {
    acceleration_ = config;
    synchronized_revision_ = 0;
}

SimdImplementation CircuitRuntime::simd_implementation() const noexcept {
    return simulator_ ? simulator_->simd_implementation() : SimdImplementation::Scalar;
}

std::uint32_t CircuitRuntime::worker_thread_count() const noexcept {
    return simulator_ ? simulator_->worker_thread_count() : 1u;
}

const AccelerationStats* CircuitRuntime::acceleration_stats() const noexcept {
    return simulator_ ? &simulator_->acceleration_stats() : nullptr;
}

void CircuitRuntime::set_backend(RuntimeBackend backend) noexcept {
    if (backend_ == backend) return;
    backend_ = backend;
    synchronized_revision_ = 0;
}

void CircuitRuntime::synchronize(CircuitEditor& editor) {
    const auto has_custom = std::any_of(editor.components().begin(), editor.components().end(),
        [](const Component& component) { return component.type == ComponentType::Custom; });
    if (has_custom) throw std::logic_error("custom components require synchronize(editor, component_library)");
    if (!simulator_ || synchronized_revision_ != editor.structural_revision()) rebuild(editor);
    synchronized_library_revision_ = 0;
    if (synchronized_input_revision_ != editor.input_revision()) {
        synchronize_inputs(editor);
        synchronized_input_revision_ = editor.input_revision();
    }
}

void CircuitRuntime::synchronize(CircuitEditor& editor, const ComponentLibrary& library) {
    const auto has_custom = std::any_of(editor.components().begin(), editor.components().end(),
        [](const Component& component) { return component.type == ComponentType::Custom; });
    const bool rebuild_required = !simulator_ || synchronized_revision_ != editor.structural_revision() ||
        (has_custom && synchronized_library_revision_ != library.revision());
    if (rebuild_required) {
        if (!has_custom) {
            rebuild(editor);
            synchronized_library_revision_ = library.revision();
        } else {
            ExpandedCircuit expanded;
            HierarchyError hierarchy_error;
            if (!expand_component_hierarchy(editor, library, expanded, hierarchy_error))
                throw std::runtime_error("custom component hierarchy validation failed: " + hierarchy_error.describe());

            // Build the ordinary HCS runtime from the flattened editor graph. Keep a copy of
            // the public root-to-expanded mapping before translating pins and inputs back.
            rebuild(expanded.circuit);
            const auto flat_pin_signals = pin_signals_;
            const auto flat_input_bindings = input_bindings_;
            std::unordered_map<PinId, SignalId> root_pin_signals;
            root_pin_signals.reserve(expanded.root_pin_to_expanded_pin.size());
            for (const auto& [root_pin, expanded_pin] : expanded.root_pin_to_expanded_pin) {
                const auto found = flat_pin_signals.find(expanded_pin);
                if (found != flat_pin_signals.end()) {
                    root_pin_signals.emplace(root_pin, found->second);
                    editor.assign_simulation_signal(root_pin, found->second);
                }
            }
            std::vector<InputBinding> root_inputs;
            root_inputs.reserve(expanded.root_input_to_expanded_input.size());
            for (const auto& [root_component, expanded_component] : expanded.root_input_to_expanded_input) {
                const auto binding = std::find_if(flat_input_bindings.begin(), flat_input_bindings.end(),
                    [&](const InputBinding& candidate) { return candidate.component == expanded_component; });
                if (binding != flat_input_bindings.end()) {
                    auto translated = *binding;
                    translated.component = root_component;
                    root_inputs.push_back(translated);
                }
            }
            pin_signals_ = std::move(root_pin_signals);
            input_bindings_ = std::move(root_inputs);
            synchronized_revision_ = editor.structural_revision();
            synchronized_library_revision_ = library.revision();
            synchronized_input_revision_ = 0;
        }
    }
    if (synchronized_input_revision_ != editor.input_revision()) {
        synchronize_inputs(editor);
        synchronized_input_revision_ = editor.input_revision();
    }
}

void CircuitRuntime::rebuild(CircuitEditor& editor) {
    // Simulator stores a reference to the immutable Circuit, so destroy it before
    // replacing the Circuit object it references.
    simulator_.reset();
    CircuitBuilder builder;
    pin_signals_.clear();
    input_bindings_.clear();

    // Allocate every driven output first. This allows wires to form feedback loops
    // without depending on component storage order.
    for (const auto& component : editor.components()) {
        if (component.type == ComponentType::Input) {
            if (component.outputs.size() != 1) throw std::logic_error("Input component must have one output pin");
            pin_signals_[component.outputs.front().id] = builder.add_input();
        } else {
            for (const auto& output : component.outputs)
                pin_signals_[output.id] = builder.add_signal();
        }
    }

    std::unordered_map<PinId, PinId> source_for_input;
    source_for_input.reserve(editor.wires().size());
    for (const auto& wire : editor.wires()) source_for_input.emplace(wire.input_pin, wire.output_pin);

    // Connect input pins to their upstream source signal. Unconnected editor inputs
    // are intentionally tied to logic 0 until the user wires them.
    for (const auto& component : editor.components()) {
        for (const auto& input : component.inputs) {
            const auto source = source_for_input.find(input.id);
            SignalId signal = kInvalidSignal;
            if (source != source_for_input.end()) {
                const auto mapped = pin_signals_.find(source->second);
                if (mapped == pin_signals_.end()) throw std::logic_error("Wire source has no simulation signal");
                signal = mapped->second;
            } else {
                signal = builder.add_constant(false);
            }
            pin_signals_[input.id] = signal;
        }
    }

    // Add drivers only after all signal IDs are allocated.
    for (const auto& component : editor.components()) {
        switch (component.type) {
            case ComponentType::Input:
            case ComponentType::Output:
                break;
            case ComponentType::Not: {
                if (component.inputs.size() != 1 || component.outputs.size() != 1)
                    throw std::logic_error("NOT component pin layout is invalid");
                builder.add_gate(LogicOp::Not, pin_signals_.at(component.inputs[0].id), kInvalidSignal,
                                 pin_signals_.at(component.outputs[0].id));
                break;
            }
            case ComponentType::And:
            case ComponentType::Or:
            case ComponentType::Xor: {
                if (component.inputs.size() != 2 || component.outputs.size() != 1)
                    throw std::logic_error("Binary gate pin layout is invalid");
                const auto op = component.type == ComponentType::And ? LogicOp::And :
                                component.type == ComponentType::Or ? LogicOp::Or : LogicOp::Xor;
                builder.add_gate(op, pin_signals_.at(component.inputs[0].id),
                                 pin_signals_.at(component.inputs[1].id), pin_signals_.at(component.outputs[0].id));
                break;
            }
            case ComponentType::DFlipFlop: {
                if (component.inputs.size() != 2 || component.outputs.size() != 1)
                    throw std::logic_error("DFF component pin layout is invalid");
                builder.add_d_flip_flop(pin_signals_.at(component.inputs[0].id),
                                        pin_signals_.at(component.inputs[1].id),
                                        pin_signals_.at(component.outputs[0].id), component.dff_initial_state);
                break;
            }
            case ComponentType::Custom:
                throw std::logic_error("unexpanded custom component reached CircuitBuilder integration");
        }
    }

    for (const auto& [pin, signal] : pin_signals_) editor.assign_simulation_signal(pin, signal);

    circuit_ = std::make_unique<Circuit>(builder.build());
    simulator_ = std::make_unique<Simulator>(*circuit_, make_config());
    for (const auto& component : editor.components()) {
        if (component.type == ComponentType::Input) {
            const auto signal = pin_signals_.at(component.outputs.front().id);
            input_bindings_.push_back(InputBinding{component.id, signal, false, false});
        }
    }
    synchronized_revision_ = editor.structural_revision();
    synchronized_input_revision_ = 0;
    last_result_ = {};
}

void CircuitRuntime::synchronize_inputs(const CircuitEditor& editor) {
    if (!simulator_) return;
    for (auto& binding : input_bindings_) {
        const auto* component = editor.find_component(binding.component);
        if (!component || component->type != ComponentType::Input) continue;
        if (!binding.initialized || binding.last_value != component->input_value) {
            simulator_->set_input(binding.signal, component->input_value);
            binding.last_value = component->input_value;
            binding.initialized = true;
        }
    }
}

StepResult CircuitRuntime::step() {
    if (!simulator_) return {};
    last_result_ = simulator_->step();
    return last_result_;
}

void CircuitRuntime::reset(CircuitEditor& editor) {
    if (!circuit_ || synchronized_revision_ != editor.structural_revision() || synchronized_revision_ == 0) {
        synchronize(editor);
        return;
    }
    simulator_ = std::make_unique<Simulator>(*circuit_, make_config());
    for (auto& binding : input_bindings_) binding.initialized = false;
    synchronize_inputs(editor);
    synchronized_input_revision_ = editor.input_revision();
    last_result_ = {};
}

void CircuitRuntime::reset(CircuitEditor& editor, const ComponentLibrary& library) {
    synchronize(editor, library);
    simulator_ = std::make_unique<Simulator>(*circuit_, make_config());
    for (auto& binding : input_bindings_) binding.initialized = false;
    synchronize_inputs(editor);
    synchronized_input_revision_ = editor.input_revision();
    last_result_ = {};
}

SignalState CircuitRuntime::signal_for_pin(PinId pin) const noexcept {
    if (!simulator_) return SignalState::Unknown;
    const auto found = pin_signals_.find(pin);
    if (found == pin_signals_.end()) return SignalState::Unknown;
    try {
        return simulator_->signal(found->second) ? SignalState::One : SignalState::Zero;
    } catch (...) {
        return SignalState::Error;
    }
}

bool CircuitRuntime::binary_signal_for_pin(PinId pin) const noexcept {
    return signal_for_pin(pin) == SignalState::One;
}

std::uint64_t CircuitRuntime::tick_count() const noexcept {
    return simulator_ ? simulator_->tick_count() : 0;
}

const char* backend_name(RuntimeBackend backend) noexcept {
    switch (backend) {
        case RuntimeBackend::InterpretedEvent: return "INTERPRETED EVENT";
        case RuntimeBackend::InterpretedDense: return "INTERPRETED DENSE";
        case RuntimeBackend::OptimizedIRDense: return "OPTIMIZED IR DENSE";
        case RuntimeBackend::OptimizedIREvent: return "OPTIMIZED IR EVENT";
        case RuntimeBackend::HybridIR: return "HYBRID IR";
        case RuntimeBackend::CompiledNative: return "COMPILED NATIVE";
    }
    return "UNKNOWN";
}

} // namespace hcs::editor
