#include "hcs/editor/custom_components.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hcs::editor {
namespace {

constexpr std::size_t kMaximumDefinitionBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumDefinitionText = 256;
constexpr float kMaximumDimension = 10'000'000.0F;

bool valid_symbol(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

bool fail(PersistenceError& error, std::size_t line, std::string message) {
    error.line = line;
    error.message = std::move(message);
    return false;
}

std::string quote(std::string_view value) {
    std::ostringstream out;
    out << std::quoted(std::string(value));
    return out.str();
}

std::string float_string(float value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return out.str();
}

bool parse_dimension(std::string_view text, float& value) {
    if (text.empty()) return false;
    float parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !std::isfinite(parsed) || parsed < 48.0F || parsed > kMaximumDimension) return false;
    value = parsed;
    return true;
}

const Pin* find_pin_by_key(const Component& component, PinDirection direction, std::string_view key) {
    const auto& pins = direction == PinDirection::Input ? component.inputs : component.outputs;
    const auto found = std::find_if(pins.begin(), pins.end(), [&](const Pin& pin) { return pin.key == key; });
    return found == pins.end() ? nullptr : &*found;
}

SymbolDefinition effective_symbol(const CustomComponentDefinition& definition) {
    std::vector<std::pair<std::string, std::string>> records;
    records.reserve(definition.ports.size());
    for (const auto& port : definition.ports)
        records.emplace_back(port.id, port.direction == PinDirection::Input ? "input" : "output");
    if (definition.symbol.objects.empty() && definition.symbol.ports.empty())
        return make_default_symbol(definition.size, records);
    auto symbol = definition.symbol;
    symbol.size = definition.size;
    const auto defaults = make_default_symbol(definition.size, records);
    for (const auto& port : defaults.ports) {
        if (std::none_of(symbol.ports.begin(), symbol.ports.end(), [&](const SymbolPortVisual& item) {
                return item.port_id == port.port_id;
            })) symbol.ports.push_back(port);
    }
    return symbol;
}

std::vector<CustomPortLayout> layouts(const CustomComponentDefinition& definition) {
    const auto symbol = effective_symbol(definition);
    std::vector<CustomPortLayout> result;
    result.reserve(definition.ports.size());
    for (const auto& port : definition.ports) {
        CustomPortLayout layout{port.id, port.name, port.direction};
        const auto visual = std::find_if(symbol.ports.begin(), symbol.ports.end(), [&](const SymbolPortVisual& item) {
            return item.port_id == port.id;
        });
        if (visual != symbol.ports.end()) {
            layout.local_position = {visual->x, visual->y};
            layout.explicit_position = true;
        }
        result.push_back(std::move(layout));
    }
    return result;
}

bool same_definition_logic(const CustomComponentDefinition& a, const CustomComponentDefinition& b) {
    if (a.id != b.id || a.ports.size() != b.ports.size() || a.implementation.components().size() != b.implementation.components().size() ||
        a.implementation.wires().size() != b.implementation.wires().size()) return false;
    for (const auto& port : a.ports) {
        const auto found = std::find_if(b.ports.begin(), b.ports.end(), [&](const CustomPort& other) {
            return other.id == port.id && other.direction == port.direction && other.terminal_component == port.terminal_component;
        });
        if (found == b.ports.end()) return false;
    }
    std::map<ComponentId, const Component*> components_b;
    for (const auto& item : b.implementation.components()) components_b.emplace(item.id, &item);
    for (const auto& item : a.implementation.components()) {
        const auto found = components_b.find(item.id);
        if (found == components_b.end()) return false;
        const auto& other = *found->second;
        if (item.type != other.type || item.input_value != other.input_value ||
            item.dff_initial_state != other.dff_initial_state || item.definition_id != other.definition_id) return false;
    }
    auto wire_keys = [](const CircuitEditor& circuit) {
        std::vector<std::tuple<ComponentId, std::string, ComponentId, std::string>> keys;
        for (const auto& wire : circuit.wires()) {
            const auto* output = circuit.find_pin(wire.output_pin);
            const auto* input = circuit.find_pin(wire.input_pin);
            if (!output || !input) continue;
            keys.emplace_back(output->owner, output->key, input->owner, input->key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    };
    return wire_keys(a.implementation) == wire_keys(b.implementation);
}

bool clone_component(const Component& source, CircuitEditor& destination,
                     const ComponentLibrary& library, Vec2 position) {
    bool ok = false;
    if (source.type == ComponentType::Custom) {
        const auto* definition = library.find(source.definition_id);
        if (!definition) return false;
        ok = destination.add_custom_component_with_id(source.id, source.definition_id,
                                                       position, layouts(*definition), definition->size);
    } else {
        ok = destination.add_component_with_id(source.id, source.type, position,
                                               source.input_value, source.dff_initial_state);
    }
    if (!ok) return false;
    auto* target = destination.find_component(source.id);
    if (!target) return false;
    for (auto& target_pin : target->inputs) {
        const auto* source_pin = find_pin_by_key(source, PinDirection::Input, target_pin.key);
        if (!source_pin) return false;
        target_pin.local_position = source_pin->local_position;
    }
    for (auto& target_pin : target->outputs) {
        const auto* source_pin = find_pin_by_key(source, PinDirection::Output, target_pin.key);
        if (!source_pin) return false;
        target_pin.local_position = source_pin->local_position;
    }
    target->size = source.size;
    if (source.type == ComponentType::Custom)
        (void)destination.set_component_transform(source.id, source.rotation_quadrants,
                                                  source.flip_horizontal, source.flip_vertical);
    return true;
}

bool clone_wire(const CircuitEditor& source, const Wire& wire, CircuitEditor& destination) {
    const auto* source_pin = source.find_pin(wire.output_pin);
    const auto* destination_pin = source.find_pin(wire.input_pin);
    if (!source_pin || !destination_pin) return false;
    const auto* source_component = destination.find_component(source_pin->owner);
    const auto* destination_component = destination.find_component(destination_pin->owner);
    if (!source_component || !destination_component) return false;
    const auto* mapped_source = find_pin_by_key(*source_component, PinDirection::Output, source_pin->key);
    const auto* mapped_destination = find_pin_by_key(*destination_component, PinDirection::Input, destination_pin->key);
    return mapped_source && mapped_destination && destination.connect(mapped_source->id, mapped_destination->id).has_value();
}

bool validate_definition_shape(const CustomComponentDefinition& definition, PersistenceError& error) {
    if (!valid_symbol(definition.id)) return fail(error, 0, "definition ID must contain only letters, digits, '.', '_' or '-' and be at most 128 characters");
    if (definition.name.empty() || definition.name.size() > kMaximumDefinitionText)
        return fail(error, 0, "component name must contain 1..256 bytes");
    if (definition.description.size() > 4096 || definition.category.empty() || definition.category.size() > 128)
        return fail(error, 0, "description or category exceeds its size limit");
    if (definition.version != 1) return fail(error, 0, "unsupported component definition version " + std::to_string(definition.version));
    if (!std::isfinite(definition.size.x) || !std::isfinite(definition.size.y) ||
        definition.size.x < 48.0F || definition.size.y < 48.0F ||
        definition.size.x > kMaximumDimension || definition.size.y > kMaximumDimension)
        return fail(error, 0, "component visual dimensions must be finite and between 48 and 10000000");

    std::set<std::string> port_ids;
    std::set<std::pair<PinDirection, std::string>> port_names;
    std::set<ComponentId> terminal_components;
    std::size_t input_ports = 0;
    std::size_t output_ports = 0;
    for (const auto& port : definition.ports) {
        if (!valid_symbol(port.id)) return fail(error, 0, "invalid external port ID '" + port.id + "'");
        if (port.name.empty() || port.name.size() > kMaximumDefinitionText)
            return fail(error, 0, "external port name must contain 1..256 bytes");
        if (!port_ids.insert(port.id).second) return fail(error, 0, "duplicate external port ID '" + port.id + "'");
        if (!port_names.emplace(port.direction, port.name).second)
            return fail(error, 0, "duplicate external port name '" + port.name + "'");
        if (port.direction == PinDirection::Input) ++input_ports;
        else if (port.direction == PinDirection::Output) ++output_ports;
        else return fail(error, 0, "external port direction must be Input or Output");
        if (input_ports > 64 || output_ports > 64)
            return fail(error, 0, "a component definition may expose at most 64 ports per direction");
        if (!terminal_components.insert(port.terminal_component).second)
            return fail(error, 0, "one internal terminal component is mapped to multiple external ports");
        const auto* terminal = definition.implementation.find_component(port.terminal_component);
        if (!terminal) return fail(error, 0, "port '" + port.id + "' references missing terminal component " + std::to_string(port.terminal_component));
        if (port.direction == PinDirection::Input && terminal->type != ComponentType::Input)
            return fail(error, 0, "input port '" + port.id + "' must map to an internal Input component");
        if (port.direction == PinDirection::Output && terminal->type != ComponentType::Output)
            return fail(error, 0, "output port '" + port.id + "' must map to an internal Output component");
    }
    for (const auto& component : definition.implementation.components()) {
        if (component.type == ComponentType::Input || component.type == ComponentType::Output) {
            if (!terminal_components.contains(component.id))
                return fail(error, 0, "internal " + std::string(component_name(component.type)) + " component " + std::to_string(component.id) + " is not mapped to an external port");
        }
    }
    const auto visual = effective_symbol(definition);
    SymbolError visual_error;
    if (!validate_symbol(visual, visual_error))
        return fail(error, visual_error.line, "invalid visual symbol: " + visual_error.describe());
    for (const auto& port : definition.ports) {
        if (std::none_of(visual.ports.begin(), visual.ports.end(), [&](const SymbolPortVisual& item) { return item.port_id == port.id; }))
            return fail(error, 0, "visual symbol is missing external port '" + port.id + "'");
    }
    for (const auto& visual_port : visual.ports) {
        if (!port_ids.contains(visual_port.port_id))
            return fail(error, 0, "visual symbol references unknown external port '" + visual_port.port_id + "'");
    }
    error = {};
    return true;
}

struct EndpointKey {
    std::string path;
    ComponentId component{kInvalidComponent};
    std::string pin;
    friend bool operator<(const EndpointKey& a, const EndpointKey& b) {
        return std::tie(a.path, a.component, a.pin) < std::tie(b.path, b.component, b.pin);
    }
};

class Expander final {
public:
    Expander(const ComponentLibrary& library, const HierarchyLimits& limits, HierarchyError& error)
        : library_(library), limits_(limits), error_(error) {}

    bool run(const CircuitEditor& root, ExpandedCircuit& result) {
        if (!expand_context(root, nullptr, "root", {}, 0)) return false;
        for (const auto& pending : pending_wires_) {
            PinId source = kInvalidPin;
            PinId destination = kInvalidPin;
            if (!resolve_source(pending.source, source)) continue; // Unconnected custom inputs are tied low by CircuitRuntime.
            const auto physical_destination = physical_pins_.find(pending.destination);
            if (physical_destination == physical_pins_.end()) {
                error_.message = "expanded wire destination does not map to a built-in input pin";
                return false;
            }
            destination = physical_destination->second;
            if (!result_.circuit.connect(source, destination)) {
                error_.message = "expanded hierarchy creates a multiply-driven or invalid input";
                return false;
            }
        }
        for (const auto& [root_pin, endpoint] : root_pin_endpoints_) {
            PinId flattened = kInvalidPin;
            const auto direct = physical_pins_.find(endpoint);
            if (direct != physical_pins_.end()) flattened = direct->second;
            else (void)resolve_source(endpoint, flattened);
            if (flattened != kInvalidPin) result_.root_pin_to_expanded_pin.emplace(root_pin, flattened);
        }
        result = std::move(result_);
        return true;
    }

private:
    bool expand_context(const CircuitEditor& source, const CustomComponentDefinition* definition,
                        const std::string& path,
                        const std::map<std::string, EndpointKey, std::less<>>& incoming,
                        std::size_t depth) {
        if (depth > limits_.maximum_depth) {
            error_.definition_id = definition ? definition->id : std::string{};
            error_.message = "maximum custom-component nesting depth exceeded";
            return false;
        }
        if (++context_count_ > limits_.maximum_dependencies) {
            error_.message = "maximum expanded dependency count exceeded";
            return false;
        }

        std::map<ComponentId, ComponentId> interface_terminal;
        std::set<ComponentId> interface_outputs;
        if (definition) {
            for (const auto& port : definition->ports) {
                const auto* terminal = source.find_component(port.terminal_component);
                if (!terminal) {
                    error_.definition_id = definition->id;
                    error_.message = "external port '" + port.id + "' references a missing terminal component";
                    return false;
                }
                interface_terminal.emplace(port.terminal_component, port.terminal_component);
                if (port.direction == PinDirection::Output) interface_outputs.insert(port.terminal_component);
                else {
                    const auto alias = incoming.find(port.id);
                    if (alias != incoming.end() && !terminal->outputs.empty())
                        aliases_[key(path, *terminal, terminal->outputs.front())] = alias->second;
                }
            }
        }

        std::map<ComponentId, ComponentId> cloned_ids;
        for (const auto& component : source.components()) {
            if (interface_terminal.contains(component.id)) continue;
            if (component.type == ComponentType::Custom) continue;
            if (next_component_id_ >= kInvalidComponent || expanded_component_count_ >= limits_.maximum_expanded_components) {
                error_.definition_id = definition ? definition->id : std::string{};
                error_.message = "maximum expanded component count exceeded";
                return false;
            }
            const auto new_id = next_component_id_++;
            if (!result_.circuit.add_component_with_id(new_id, component.type, component.position,
                                                       component.input_value, component.dff_initial_state)) {
                error_.message = "could not allocate an expanded built-in component";
                return false;
            }
            ++expanded_component_count_;
            cloned_ids.emplace(component.id, new_id);
            const auto* flattened_component = result_.circuit.find_component(new_id);
            if (!flattened_component) return false;
            for (const auto& pin : component.inputs) {
                const auto* target = find_pin_by_key(*flattened_component, PinDirection::Input, pin.key);
                if (!target) { error_.message = "built-in input pin layout changed during hierarchy expansion"; return false; }
                physical_pins_[key(path, component, pin)] = target->id;
                if (path == "root") root_pin_endpoints_[pin.id] = key(path, component, pin);
            }
            for (const auto& pin : component.outputs) {
                const auto* target = find_pin_by_key(*flattened_component, PinDirection::Output, pin.key);
                if (!target) { error_.message = "built-in output pin layout changed during hierarchy expansion"; return false; }
                physical_pins_[key(path, component, pin)] = target->id;
                if (path == "root") root_pin_endpoints_[pin.id] = key(path, component, pin);
            }
            if (path == "root" && component.type == ComponentType::Input)
                result_.root_input_to_expanded_input.emplace(component.id, new_id);
        }

        // Recursively expand custom instances. Their input wires become aliases into the child
        // context; their output pins become aliases to the child's internal output sources.
        for (const auto& component : source.components()) {
            if (component.type != ComponentType::Custom) continue;
            const auto* child_definition = library_.find(component.definition_id);
            if (!child_definition) {
                error_.definition_id = component.definition_id;
                error_.instance = component.id;
                error_.message = "missing custom component definition '" + component.definition_id + "'";
                return false;
            }
            std::map<std::string, EndpointKey, std::less<>> child_inputs;
            for (const auto& port : child_definition->ports) {
                if (port.direction != PinDirection::Input) continue;
                const auto* instance_pin = find_pin_by_key(component, PinDirection::Input, port.id);
                if (!instance_pin) {
                    error_.definition_id = child_definition->id;
                    error_.instance = component.id;
                    error_.message = "instance is missing external input port '" + port.id + "'";
                    return false;
                }
                const auto wire = std::find_if(source.wires().begin(), source.wires().end(), [&](const Wire& candidate) {
                    return candidate.input_pin == instance_pin->id;
                });
                if (wire != source.wires().end()) {
                    const auto* source_pin = source.find_pin(wire->output_pin);
                    const auto* source_component = source_pin ? source.find_component(source_pin->owner) : nullptr;
                    if (!source_pin || !source_component) return false;
                    child_inputs.emplace(port.id, key(path, *source_component, *source_pin));
                }
            }
            const auto child_path = path + "/c" + std::to_string(component.id) + "[" + child_definition->id + "]";
            std::map<std::string, EndpointKey, std::less<>> child_outputs;
            if (!expand_context(child_definition->implementation, child_definition, child_path,
                                child_inputs, depth + 1)) return false;
            for (const auto& port : child_definition->ports) {
                if (port.direction != PinDirection::Output) continue;
                const auto* terminal = child_definition->implementation.find_component(port.terminal_component);
                if (!terminal || terminal->inputs.empty()) continue;
                const auto terminal_input = terminal->inputs.front().id;
                const auto wire = std::find_if(child_definition->implementation.wires().begin(),
                    child_definition->implementation.wires().end(), [&](const Wire& candidate) { return candidate.input_pin == terminal_input; });
                if (wire == child_definition->implementation.wires().end()) continue;
                const auto* source_pin = child_definition->implementation.find_pin(wire->output_pin);
                const auto* source_component = source_pin ? child_definition->implementation.find_component(source_pin->owner) : nullptr;
                if (!source_pin || !source_component) continue;
                child_outputs.emplace(port.id, key(child_path, *source_component, *source_pin));
            }
            for (const auto& pin : component.outputs) {
                const auto output = child_outputs.find(pin.key);
                if (output != child_outputs.end()) {
                    aliases_[key(path, component, pin)] = output->second;
                    if (path == "root") root_pin_endpoints_[pin.id] = key(path, component, pin);
                }
            }
            if (path == "root") {
                for (const auto& pin : component.inputs) {
                    const auto wire = std::find_if(source.wires().begin(), source.wires().end(), [&](const Wire& candidate) { return candidate.input_pin == pin.id; });
                    if (wire != source.wires().end()) {
                        const auto* source_pin = source.find_pin(wire->output_pin);
                        const auto* source_component = source_pin ? source.find_component(source_pin->owner) : nullptr;
                        if (source_pin && source_component) root_pin_endpoints_[pin.id] = key(path, *source_component, *source_pin);
                    }
                }
            }
        }

        // Record normal wires in this context. Wires feeding an interface Output are aliases,
        // while wires feeding a nested custom instance are consumed as that instance's inputs.
        for (const auto& wire : source.wires()) {
            const auto* source_pin = source.find_pin(wire.output_pin);
            const auto* destination_pin = source.find_pin(wire.input_pin);
            const auto* source_component = source_pin ? source.find_component(source_pin->owner) : nullptr;
            const auto* destination_component = destination_pin ? source.find_component(destination_pin->owner) : nullptr;
            if (!source_pin || !destination_pin || !source_component || !destination_component) return false;
            if (definition && interface_outputs.contains(destination_component->id)) continue;
            if (destination_component->type == ComponentType::Custom) continue;
            pending_wires_.push_back({key(path, *source_component, *source_pin), key(path, *destination_component, *destination_pin)});
        }

        // Root-level custom output aliases are now known from the recursive calls.
        (void)cloned_ids;
        return true;
    }

    EndpointKey key(const std::string& path, const Component& component, const Pin& pin) const {
        return EndpointKey{path, component.id, pin.key};
    }

    bool resolve_source(const EndpointKey& original, PinId& result) const {
        EndpointKey current = original;
        std::set<EndpointKey> visited;
        for (std::size_t depth = 0; depth <= limits_.maximum_depth * 4 + 16; ++depth) {
            if (!visited.insert(current).second) {
                error_.message = "custom component port aliases contain a cycle";
                return false;
            }
            const auto alias = aliases_.find(current);
            if (alias != aliases_.end()) {
                current = alias->second;
                continue;
            }
            const auto physical = physical_pins_.find(current);
            if (physical == physical_pins_.end()) return false;
            const auto* pin = result_.circuit.find_pin(physical->second);
            if (!pin || pin->direction != PinDirection::Output) return false;
            result = physical->second;
            return true;
        }
        error_.message = "custom component port alias resolution exceeded its safety limit";
        return false;
    }

    const ComponentLibrary& library_;
    const HierarchyLimits& limits_;
    HierarchyError& error_;
    ExpandedCircuit result_;
    ComponentId next_component_id_{0};
    std::size_t context_count_{0};
    std::size_t expanded_component_count_{0};
    std::map<EndpointKey, PinId> physical_pins_;
    std::map<EndpointKey, EndpointKey> aliases_;
    std::map<PinId, EndpointKey> root_pin_endpoints_;
    struct PendingWire { EndpointKey source; EndpointKey destination; };
    std::vector<PendingWire> pending_wires_;
};

bool validate_definition_refs(const CustomComponentDefinition& definition, const ComponentLibrary& library,
                              const HierarchyLimits& limits, std::set<std::string>& visiting,
                              std::set<std::string>& visited, std::size_t depth,
                              HierarchyError& error) {
    if (depth > limits.maximum_depth) {
        error.definition_id = definition.id;
        error.message = "maximum custom-component nesting depth exceeded";
        return false;
    }
    if (visited.contains(definition.id)) return true;
    if (!visiting.insert(definition.id).second) {
        error.definition_id = definition.id;
        error.message = "recursive custom-component dependency detected";
        return false;
    }
    for (const auto& component : definition.implementation.components()) {
        if (component.type != ComponentType::Custom) continue;
        const auto* child = library.find(component.definition_id);
        if (!child) {
            error.definition_id = component.definition_id;
            error.instance = component.id;
            error.message = "missing nested custom component definition '" + component.definition_id + "'";
            visiting.erase(definition.id);
            return false;
        }
        if (!validate_definition_refs(*child, library, limits, visiting, visited, depth + 1, error)) {
            if (error.instance == kInvalidComponent) error.instance = component.id;
            visiting.erase(definition.id);
            return false;
        }
    }
    visiting.erase(definition.id);
    visited.insert(definition.id);
    return true;
}

} // namespace

bool ComponentLibrary::add(CustomComponentDefinition definition, PersistenceError& error) {
    if (!validate_definition_shape(definition, error)) return false;
    if (definitions_.contains(definition.id)) return fail(error, 0, "duplicate component definition ID '" + definition.id + "'");
    definition.symbol = effective_symbol(definition);
    definitions_.emplace(definition.id, std::move(definition));
    ++revision_;
    if (revision_ == 0) ++revision_;
    ++logic_revision_;
    if (logic_revision_ == 0) ++logic_revision_;
    error = {};
    return true;
}

bool ComponentLibrary::replace(CustomComponentDefinition definition, PersistenceError& error) {
    if (!validate_definition_shape(definition, error)) return false;
    const auto found = definitions_.find(definition.id);
    if (found == definitions_.end()) return fail(error, 0, "component definition does not exist");
    const bool logic_changed = !same_definition_logic(found->second, definition);
    definition.symbol = effective_symbol(definition);
    found->second = std::move(definition);
    ++revision_;
    if (revision_ == 0) ++revision_;
    if (logic_changed) {
        ++logic_revision_;
        if (logic_revision_ == 0) ++logic_revision_;
    }
    error = {};
    return true;
}

bool ComponentLibrary::erase(std::string_view id) {
    if (definitions_.erase(std::string(id)) == 0) return false;
    ++revision_;
    if (revision_ == 0) ++revision_;
    ++logic_revision_;
    if (logic_revision_ == 0) ++logic_revision_;
    return true;
}

const CustomComponentDefinition* ComponentLibrary::find(std::string_view id) const noexcept {
    const auto found = definitions_.find(id);
    return found == definitions_.end() ? nullptr : &found->second;
}

std::vector<const CustomComponentDefinition*> ComponentLibrary::definitions() const {
    std::vector<const CustomComponentDefinition*> result;
    result.reserve(definitions_.size());
    for (const auto& [id, definition] : definitions_) { (void)id; result.push_back(&definition); }
    return result;
}

std::vector<ComponentMetadata> ComponentLibrary::catalog() const {
    std::vector<ComponentMetadata> result{
        {"builtin.input", "Input", "Control", "Boolean input source", true, 0, 1, {}, {"out"}},
        {"builtin.output", "Output", "Control", "Boolean output probe", true, 1, 0, {"in"}, {}},
        {"builtin.and", "AND", "Logic Gates", "Two-input AND gate", true, 2, 1, {"A", "B"}, {"out"}},
        {"builtin.or", "OR", "Logic Gates", "Two-input OR gate", true, 2, 1, {"A", "B"}, {"out"}},
        {"builtin.not", "NOT", "Logic Gates", "Inverter", true, 1, 1, {"in"}, {"out"}},
        {"builtin.xor", "XOR", "Logic Gates", "Two-input XOR gate", true, 2, 1, {"A", "B"}, {"out"}},
        {"builtin.dff", "D Flip-Flop", "Sequential", "Rising-edge triggered D flip-flop", true, 2, 1, {"D", "CLK"}, {"Q"}},
    };
    for (const auto& [id, definition] : definitions_) {
        ComponentMetadata metadata{id, definition.name, definition.category, definition.description, false, 0, 0, {}, {}};
        for (const auto& port : definition.ports) {
            if (port.direction == PinDirection::Input) {
                ++metadata.input_count;
                metadata.input_names.push_back(port.name);
            } else {
                ++metadata.output_count;
                metadata.output_names.push_back(port.name);
            }
        }
        result.push_back(std::move(metadata));
    }
    return result;
}

std::filesystem::path ComponentLibrary::default_directory() {
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "hcs" / "components";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local" / "share" / "hcs" / "components";
    return std::filesystem::path("components");
}

bool ComponentLibrary::save_definition(const std::filesystem::path& directory,
                                       std::string_view id, PersistenceError& error) const {
    const auto* definition = find(id);
    if (!definition) return fail(error, 0, "component definition not found");
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return fail(error, 0, "cannot create component library directory: " + ec.message());
    return save_component_definition_file(directory / (definition->id + std::string(kCustomComponentFileExtension)),
                                          *definition, *this, error);
}

bool ComponentLibrary::load_directory(const std::filesystem::path& directory,
                                      std::vector<std::string>& diagnostics) {
    diagnostics.clear();
    std::error_code ec;
    if (!std::filesystem::exists(directory, ec)) {
        if (ec) diagnostics.push_back("Cannot inspect component library: " + ec.message());
        return !ec;
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file() && it->path().extension() == kCustomComponentFileExtension)
            files.push_back(it->path());
    }
    if (ec) { diagnostics.push_back("Cannot enumerate component library: " + ec.message()); return false; }
    std::sort(files.begin(), files.end());

    // Load into a private catalogue. A malformed or unresolved file must not leave a
    // half-imported dependency graph visible to the currently open circuit.
    ComponentLibrary candidate = *this;
    std::vector<std::filesystem::path> pending = files;
    std::map<std::filesystem::path, std::string> last_error;
    bool progress = true;
    while (!pending.empty() && progress) {
        progress = false;
        for (auto it = pending.begin(); it != pending.end();) {
            CustomComponentDefinition definition;
            PersistenceError error;
            if (load_component_definition_file(*it, candidate, definition, error)) {
                if (candidate.add(std::move(definition), error)) {
                    last_error.erase(*it);
                    it = pending.erase(it);
                    progress = true;
                    continue;
                }
            }
            last_error[*it] = error.describe();
            ++it;
        }
    }
    for (const auto& path : pending) {
        const auto found = last_error.find(path);
        diagnostics.push_back(path.filename().string() + ": " +
            (found == last_error.end() || found->second.empty() ? "unresolved component dependency" : found->second));
    }
    if (!pending.empty()) return false;
    *this = std::move(candidate);
    return true;
}

std::string HierarchyError::describe() const {
    std::string result;
    if (!definition_id.empty()) result = "definition '" + definition_id + "'";
    if (instance != kInvalidComponent) {
        if (!result.empty()) result += ", ";
        result += "instance " + std::to_string(instance);
    }
    if (!result.empty()) result += ": ";
    result += message;
    return result;
}

bool validate_component_hierarchy(const CircuitEditor& root, const ComponentLibrary& library,
                                  const HierarchyLimits& limits, HierarchyError& error) {
    error = {};
    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::size_t root_refs = 0;
    for (const auto& component : root.components()) {
        if (component.type != ComponentType::Custom) continue;
        if (++root_refs > limits.maximum_dependencies) {
            error.instance = component.id;
            error.message = "maximum root custom-component dependency count exceeded";
            return false;
        }
        const auto* definition = library.find(component.definition_id);
        if (!definition) {
            error.definition_id = component.definition_id;
            error.instance = component.id;
            error.message = "missing custom component definition";
            return false;
        }
        PersistenceError shape_error;
        if (!validate_definition_shape(*definition, shape_error)) {
            error.definition_id = definition->id;
            error.instance = component.id;
            error.message = shape_error.describe();
            return false;
        }
        std::set<std::string> current_visiting;
        std::set<std::string> current_visited;
        if (!validate_definition_refs(*definition, library, limits, current_visiting, current_visited, 1, error)) {
            if (error.instance == kInvalidComponent) error.instance = component.id;
            return false;
        }
        const auto input_count = static_cast<std::size_t>(std::count_if(definition->ports.begin(), definition->ports.end(), [](const CustomPort& port) { return port.direction == PinDirection::Input; }));
        const auto output_count = definition->ports.size() - input_count;
        const auto* mutable_instance = root.find_component(component.id);
        if (!mutable_instance || mutable_instance->inputs.size() != input_count || mutable_instance->outputs.size() != output_count) {
            error.definition_id = definition->id;
            error.instance = component.id;
            error.message = "instance port layout is incompatible with its definition";
            return false;
        }
        for (const auto& port : definition->ports) {
            const auto& collection = port.direction == PinDirection::Input ? mutable_instance->inputs : mutable_instance->outputs;
            if (std::none_of(collection.begin(), collection.end(), [&](const Pin& pin) { return pin.key == port.id; })) {
                error.definition_id = definition->id;
                error.instance = component.id;
                error.message = "instance is missing port ID '" + port.id + "'";
                return false;
            }
        }
    }
    return true;
}

bool expand_component_hierarchy(const CircuitEditor& root, const ComponentLibrary& library,
                                ExpandedCircuit& expanded, HierarchyError& error,
                                const HierarchyLimits& limits) {
    if (!validate_component_hierarchy(root, library, limits, error)) return false;
    Expander expander(library, limits, error);
    ExpandedCircuit candidate;
    if (!expander.run(root, candidate)) {
        if (error.message.empty()) error.message = "failed to expand custom component hierarchy";
        return false;
    }
    expanded = std::move(candidate);
    return true;
}

bool serialize_component_definition(const CustomComponentDefinition& definition,
                                    const ComponentLibrary& library,
                                    std::string& text, PersistenceError& error) {
    if (!validate_definition_shape(definition, error)) return false;
    std::string implementation;
    if (!serialize_circuit(definition.implementation, definition.name + " implementation", library, implementation, error)) return false;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "HCSC " << kCustomComponentFormatVersion << '\n';
    out << "id " << definition.id << '\n';
    out << "name " << quote(definition.name) << '\n';
    out << "description " << quote(definition.description) << '\n';
    out << "category " << quote(definition.category) << '\n';
    out << "size width=" << float_string(definition.size.x) << " height=" << float_string(definition.size.y) << '\n';
    for (const auto& port : definition.ports) {
        out << "port " << (port.direction == PinDirection::Input ? "input" : "output") << ' '
            << port.id << ' ' << quote(port.name) << " terminal=" << port.terminal_component << '\n';
    }
    std::string symbol_text;
    SymbolError symbol_error;
    if (!serialize_symbol(effective_symbol(definition), symbol_text, symbol_error))
        return fail(error, symbol_error.line, "cannot serialize visual symbol: " + symbol_error.describe());
    out << "symbol-begin\n" << symbol_text << "symbol-end\n";
    out << "implementation\n" << implementation;
    text = out.str();
    error = {};
    return true;
}

bool parse_component_definition(std::string_view text, const ComponentLibrary& library,
                                CustomComponentDefinition& destination, PersistenceError& error) {
    if (text.size() > kMaximumDefinitionBytes) return fail(error, 0, "component definition exceeds the 16 MiB size limit");
    CustomComponentDefinition candidate;
    bool header = false, id_seen = false, name_seen = false, description_seen = false, category_seen = false, size_seen = false;
    std::size_t line_number = 0;
    std::string_view implementation;
    std::string symbol_text;
    bool in_symbol = false;
    while (!text.empty()) {
        ++line_number;
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        const auto after = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line == "symbol-begin") {
            if (in_symbol || !symbol_text.empty()) return fail(error, line_number, "duplicate symbol section");
            in_symbol = true;
            text = after;
            continue;
        }
        if (line == "symbol-end") {
            if (!in_symbol) return fail(error, line_number, "symbol-end without symbol-begin");
            in_symbol = false;
            text = after;
            continue;
        }
        if (line == "implementation") {
            if (in_symbol) return fail(error, line_number, "missing symbol-end before implementation");
            implementation = after;
            text = {};
            break;
        }
        if (in_symbol) {
            symbol_text.append(line);
            symbol_text.push_back('\n');
            text = after;
            continue;
        }
        std::istringstream in{std::string(line)};
        std::string keyword;
        in >> keyword;
        if (keyword.empty() || keyword.starts_with('#')) {
            text = after;
            continue;
        }
        if (!header) {
            unsigned version{};
            if (keyword != "HCSC" || !(in >> version) || version != kCustomComponentFormatVersion)
                return fail(error, line_number, "expected component file header 'HCSC 1'");
            header = true;
        } else if (keyword == "id") {
            std::string value;
            if (!(in >> value) || id_seen) return fail(error, line_number, "invalid or duplicate definition ID");
            candidate.id = value;
            id_seen = true;
        } else if (keyword == "name" || keyword == "description" || keyword == "category") {
            std::string value;
            if (!(in >> std::quoted(value))) return fail(error, line_number, "expected a quoted string after '" + keyword + "'");
            if (keyword == "name") { if (name_seen) return fail(error, line_number, "duplicate name"); candidate.name = std::move(value); name_seen = true; }
            else if (keyword == "description") { if (description_seen) return fail(error, line_number, "duplicate description"); candidate.description = std::move(value); description_seen = true; }
            else { if (category_seen) return fail(error, line_number, "duplicate category"); candidate.category = std::move(value); category_seen = true; }
        } else if (keyword == "size") {
            std::string width_field, height_field;
            if (!(in >> width_field >> height_field) || size_seen || !width_field.starts_with("width=") || !height_field.starts_with("height="))
                return fail(error, line_number, "expected: size width=<number> height=<number>");
            if (!parse_dimension(std::string_view(width_field).substr(6), candidate.size.x) ||
                !parse_dimension(std::string_view(height_field).substr(7), candidate.size.y))
                return fail(error, line_number, "invalid component dimensions; expected finite values between 48 and 10000000");
            size_seen = true;
        } else if (keyword == "port") {
            std::string direction, id, name, terminal_field;
            if (!(in >> direction >> id >> std::quoted(name) >> terminal_field) || !terminal_field.starts_with("terminal="))
                return fail(error, line_number, "expected: port input|output <id> \"name\" terminal=<component-id>");
            ComponentId terminal{};
            const auto terminal_text = terminal_field.substr(9);
            const auto parsed = std::from_chars(terminal_text.data(), terminal_text.data() + terminal_text.size(), terminal);
            if (parsed.ec != std::errc{} || parsed.ptr != terminal_text.data() + terminal_text.size())
                return fail(error, line_number, "invalid port terminal component ID");
            if (direction != "input" && direction != "output") return fail(error, line_number, "port direction must be input or output");
            candidate.ports.push_back({id, name, direction == "input" ? PinDirection::Input : PinDirection::Output, terminal});
        } else {
            return fail(error, line_number, "unknown component definition statement '" + keyword + "'");
        }
        std::string trailing;
        if (in >> trailing) return fail(error, line_number, "unexpected trailing field '" + trailing + "'");
        text = after;
    }
    if (!header) return fail(error, 1, "missing component definition header");
    if (!id_seen || !name_seen || !description_seen || !category_seen || !size_seen || implementation.empty())
        return fail(error, 0, "component definition is missing required metadata or implementation section");
    if (!symbol_text.empty()) {
        SymbolError symbol_error;
        if (!parse_symbol(symbol_text, candidate.symbol, symbol_error))
            return fail(error, symbol_error.line, "invalid component symbol: " + symbol_error.describe());
        if (std::abs(candidate.symbol.size.x - candidate.size.x) > 0.001F ||
            std::abs(candidate.symbol.size.y - candidate.size.y) > 0.001F)
            return fail(error, 0, "component size and symbol size must match");
    } else {
        candidate.symbol = effective_symbol(candidate);
    }
    std::string implementation_name;
    if (!parse_circuit(implementation, candidate.implementation, implementation_name, library, error)) return false;
    if (!validate_definition_shape(candidate, error)) return false;
    destination = std::move(candidate);
    error = {};
    return true;
}

bool save_component_definition_file(const std::filesystem::path& path,
                                    const CustomComponentDefinition& definition,
                                    const ComponentLibrary& library, PersistenceError& error) {
    std::string text;
    if (!serialize_component_definition(definition, library, text, error)) return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return fail(error, 0, "cannot open temporary component file for writing");
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) { file.close(); std::error_code ignored; std::filesystem::remove(temporary, ignored); return fail(error, 0, "failed writing component definition"); }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) { std::error_code ignored; std::filesystem::remove(temporary, ignored); return fail(error, 0, "cannot atomically replace component file: " + ec.message()); }
    error = {};
    return true;
}

bool load_component_definition_file(const std::filesystem::path& path,
                                    const ComponentLibrary& library,
                                    CustomComponentDefinition& destination, PersistenceError& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(error, 0, "cannot open component definition '" + path.string() + "'");
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaximumDefinitionBytes)
        return fail(error, 0, "invalid or oversized component definition file");
    file.seekg(0, std::ios::beg);
    std::string contents(static_cast<std::size_t>(length), '\0');
    if (!contents.empty()) file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file && !file.eof()) return fail(error, 0, "failed reading component definition file");
    return parse_component_definition(contents, library, destination, error);
}

bool create_empty_component_definition(std::string id, std::string name, std::string category,
                                       CustomComponentDefinition& definition, PersistenceError& error) {
    CustomComponentDefinition candidate;
    candidate.id = std::move(id);
    candidate.name = std::move(name);
    candidate.category = std::move(category);
    candidate.description = "User-defined component";
    candidate.symbol = make_default_symbol(candidate.size);
    if (!validate_definition_shape(candidate, error)) return false;
    definition = std::move(candidate);
    error = {};
    return true;
}

bool add_custom_component_port(CustomComponentDefinition& definition, std::string id,
                               std::string name, PinDirection direction, PersistenceError& error) {
    if (!valid_symbol(id) || name.empty() || name.size() > kMaximumDefinitionText)
        return fail(error, 0, "port ID or display name is invalid");
    if (direction != PinDirection::Input && direction != PinDirection::Output)
        return fail(error, 0, "port direction is invalid");
    if (std::any_of(definition.ports.begin(), definition.ports.end(), [&](const CustomPort& p) {
        return p.id == id || (p.direction == direction && p.name == name);
    })) return fail(error, 0, "port ID or same-direction display name already exists");

    constexpr std::size_t kMaximumPortsPerDirection = 64;
    const auto input_count = static_cast<std::size_t>(std::count_if(definition.ports.begin(), definition.ports.end(),
        [](const CustomPort& port) { return port.direction == PinDirection::Input; }));
    const auto output_count = definition.ports.size() - input_count;
    const auto direction_count = direction == PinDirection::Input ? input_count : output_count;
    if (direction_count >= kMaximumPortsPerDirection)
        return fail(error, 0, "a component definition may expose at most 64 ports per direction");

    ComponentId terminal_id = 0;
    for (const auto& component : definition.implementation.components()) {
        if (component.id == kInvalidComponent - 1U) return fail(error, 0, "no component ID remains for a new port terminal");
        terminal_id = std::max(terminal_id, static_cast<ComponentId>(component.id + 1U));
    }
    const auto row = static_cast<float>(direction_count);
    const Vec2 terminal_position = direction == PinDirection::Input
        ? Vec2{-180.0F, 40.0F + row * 32.0F}
        : Vec2{420.0F, 40.0F + row * 32.0F};
    const auto terminal_type = direction == PinDirection::Input ? ComponentType::Input : ComponentType::Output;
    if (!definition.implementation.add_component_with_id(terminal_id, terminal_type, terminal_position))
        return fail(error, 0, "could not create the internal terminal component for this port");
    definition.ports.push_back({std::move(id), std::move(name), direction, terminal_id});
    const auto new_input_count = input_count + (direction == PinDirection::Input ? 1U : 0U);
    const auto new_output_count = output_count + (direction == PinDirection::Output ? 1U : 0U);
    const auto max_rows = std::max(new_input_count, new_output_count);
    definition.size.y = std::max(definition.size.y, 48.0F + 32.0F * static_cast<float>(max_rows));
    if (definition.symbol.objects.empty() && definition.symbol.ports.empty())
        definition.symbol = make_default_symbol(definition.size);
    definition.symbol.size = definition.size;
    SymbolPortVisual visual_port;
    visual_port.port_id = definition.ports.back().id;
    visual_port.label = definition.ports.back().name;
    if (direction == PinDirection::Input) {
        visual_port.x = 0.0F;
        visual_port.y = definition.size.y * static_cast<float>(new_input_count) / static_cast<float>(new_input_count + 1U);
        visual_port.orientation = SymbolPortOrientation::Left;
    } else {
        visual_port.x = definition.size.x;
        visual_port.y = definition.size.y * static_cast<float>(new_output_count) / static_cast<float>(new_output_count + 1U);
        visual_port.orientation = SymbolPortOrientation::Right;
    }
    definition.symbol.ports.push_back(std::move(visual_port));
    error = {};
    return true;
}

bool rename_custom_component_port(CustomComponentDefinition& definition, std::string_view id,
                                  std::string name, PersistenceError& error) {
    if (name.empty() || name.size() > kMaximumDefinitionText)
        return fail(error, 0, "port display name must contain 1..256 bytes");
    const auto found = std::find_if(definition.ports.begin(), definition.ports.end(),
        [&](const CustomPort& port) { return port.id == id; });
    if (found == definition.ports.end()) return fail(error, 0, "port ID was not found");
    if (std::any_of(definition.ports.begin(), definition.ports.end(), [&](const CustomPort& port) {
        return port.id != id && port.direction == found->direction && port.name == name;
    })) return fail(error, 0, "another port already uses this display name");
    found->name = std::move(name);
    const auto visual = std::find_if(definition.symbol.ports.begin(), definition.symbol.ports.end(), [&](const SymbolPortVisual& item) { return item.port_id == id; });
    if (visual != definition.symbol.ports.end()) visual->label = found->name;
    error = {};
    return true;
}

bool remove_custom_component_port(CustomComponentDefinition& definition, std::string_view id,
                                  PersistenceError& error) {
    const auto found = std::find_if(definition.ports.begin(), definition.ports.end(),
        [&](const CustomPort& port) { return port.id == id; });
    if (found == definition.ports.end()) return fail(error, 0, "port ID was not found");
    if (!definition.implementation.remove_component(found->terminal_component))
        return fail(error, 0, "could not remove the internal port terminal component");
    definition.ports.erase(found);
    std::erase_if(definition.symbol.ports, [&](const SymbolPortVisual& item) { return item.port_id == id; });
    error = {};
    return true;
}

bool apply_component_definition_transactionally(ComponentLibrary& library, CircuitEditor& root,
                                                   CustomComponentDefinition definition,
                                                   PersistenceError& error) {
    if (!validate_definition_shape(definition, error)) return false;

    ComponentLibrary candidate_library = library;
    const bool replacing = candidate_library.find(definition.id) != nullptr;
    if (replacing) {
        if (!candidate_library.replace(definition, error)) return false;
    } else if (!candidate_library.add(definition, error)) {
        return false;
    }

    // Validate the edited definition's complete dependency graph before publishing it.
    std::set<std::string> visiting;
    std::set<std::string> visited;
    HierarchyError hierarchy_error;
    if (!validate_definition_refs(definition, candidate_library, HierarchyLimits{},
                                  visiting, visited, 1, hierarchy_error)) {
        return fail(error, 0, hierarchy_error.describe());
    }

    CircuitEditor candidate_root = root;
    if (replacing) {
        if (!candidate_root.update_custom_component_layout(definition.id, layouts(definition), definition.size))
            return fail(error, 0, "definition update would remove or change the direction of a connected port");
    }
    if (!validate_component_hierarchy(candidate_root, candidate_library, HierarchyLimits{}, hierarchy_error))
        return fail(error, 0, hierarchy_error.describe());

    // No operation below this point can fail: publish the library and root model together.
    library = std::move(candidate_library);
    root.replace_with(std::move(candidate_root));
    error = {};
    return true;
}

std::vector<CustomPortLayout> custom_component_port_layouts(const CustomComponentDefinition& definition) {
    return layouts(definition);
}

std::string make_definition_id(std::string_view name) {
    std::string result = "user.";
    bool previous_separator = false;
    for (const char raw_ch : name) {
        const auto ch = static_cast<unsigned char>(raw_ch);
        if (std::isalnum(ch) != 0) {
            result.push_back(static_cast<char>(std::tolower(ch)));
            previous_separator = false;
        } else if (!previous_separator && !result.empty()) {
            result.push_back('-');
            previous_separator = true;
        }
    }
    while (!result.empty() && result.back() == '-') result.pop_back();
    if (result.size() <= 5) result += "component";
    return result;
}

bool create_custom_component_from_selection(CircuitEditor& root, const std::vector<ComponentId>& selection,
                                            CustomComponentDefinition definition, ComponentLibrary& library,
                                            ComponentId& created_instance, PersistenceError& error) {
    if (selection.empty()) return fail(error, 0, "select at least one component");
    if (definition.id.empty() || definition.name.empty()) return fail(error, 0, "custom component ID and name are required");
    if (library.find(definition.id)) return fail(error, 0, "a component definition with this ID already exists");

    std::set<ComponentId> selected(selection.begin(), selection.end());
    if (selected.size() != selection.size()) return fail(error, 0, "selection contains duplicate component IDs");
    Vec2 origin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::uint32_t next_internal_id = 0;
    for (const auto id : selected) {
        const auto* component = root.find_component(id);
        if (!component) return fail(error, 0, "selected component " + std::to_string(id) + " does not exist");
        origin.x = std::min(origin.x, component->position.x);
        origin.y = std::min(origin.y, component->position.y);
        if (id >= next_internal_id) {
            if (id == kInvalidComponent - 1U) return fail(error, 0, "selected component IDs leave no space for interface terminals");
            next_internal_id = id + 1U;
        }
    }

    // Copy selected components into the private implementation, normalized to the selected origin.
    for (const auto id : selection) {
        const auto* component = root.find_component(id);
        if (!clone_component(*component, definition.implementation, library,
                             component->position - origin))
            return fail(error, 0, "could not copy selected component " + std::to_string(id));
    }

    std::map<PinId, std::string> input_port_for_pin;
    std::map<PinId, std::string> output_port_for_pin;
    std::size_t input_index = 0;
    std::size_t output_index = 0;
    for (const auto id : selection) {
        const auto* component = root.find_component(id);
        if (component->type == ComponentType::Input) {
            const auto port_id = "in" + std::to_string(input_index++);
            definition.ports.push_back({port_id, "Input " + std::to_string(input_index), PinDirection::Input, id});
            input_port_for_pin.emplace(component->outputs.front().id, port_id);
        } else if (component->type == ComponentType::Output) {
            const auto port_id = "out" + std::to_string(output_index++);
            definition.ports.push_back({port_id, "Output " + std::to_string(output_index), PinDirection::Output, id});
            output_port_for_pin.emplace(component->inputs.front().id, port_id);
        }
    }

    const auto initial_input_count = static_cast<std::size_t>(std::count_if(definition.ports.begin(), definition.ports.end(),
        [](const CustomPort& port) { return port.direction == PinDirection::Input; }));
    const auto initial_output_count = definition.ports.size() - initial_input_count;
    definition.size.y = std::max(definition.size.y,
        48.0F + 32.0F * static_cast<float>(std::max(initial_input_count, initial_output_count)));

    std::map<PinId, std::string> boundary_input_ports;
    std::map<PinId, std::string> boundary_output_ports;
    std::uint32_t boundary_counter = 0;
    for (const auto& wire : root.wires()) {
        const auto* out = root.find_pin(wire.output_pin);
        const auto* in = root.find_pin(wire.input_pin);
        if (!out || !in) return fail(error, 0, "source circuit contains a wire with a missing pin");
        const bool source_selected = selected.contains(out->owner);
        const bool destination_selected = selected.contains(in->owner);
        if (source_selected && destination_selected) {
            if (!clone_wire(root, wire, definition.implementation))
                return fail(error, 0, "could not copy an internal wire");
        } else if (!source_selected && destination_selected) {
            if (root.find_component(in->owner)->type == ComponentType::Input)
                return fail(error, 0, "an external wire cannot enter a selected Input terminal");
            auto found = boundary_input_ports.find(in->id);
            if (found == boundary_input_ports.end()) {
                if (next_internal_id == kInvalidComponent) return fail(error, 0, "not enough component IDs for input terminals");
                const auto terminal_id = next_internal_id++;
                const auto port_id = "in_boundary_" + std::to_string(boundary_counter++);
                const auto terminal_position = Vec2{-150.0F, 36.0F + 28.0F * static_cast<float>(boundary_counter)};
                if (!definition.implementation.add_component_with_id(terminal_id, ComponentType::Input, terminal_position))
                    return fail(error, 0, "could not create an input boundary terminal");
                definition.ports.push_back({port_id, "Input " + std::to_string(input_index + boundary_counter), PinDirection::Input, terminal_id});
                found = boundary_input_ports.emplace(in->id, port_id).first;
                const auto* terminal = definition.implementation.find_component(terminal_id);
                const auto* selected_component = definition.implementation.find_component(in->owner);
                const auto* terminal_out = terminal && !terminal->outputs.empty() ? &terminal->outputs.front() : nullptr;
                const auto* target_pin = selected_component ? find_pin_by_key(*selected_component, PinDirection::Input, in->key) : nullptr;
                if (!terminal_out || !target_pin || !definition.implementation.connect(terminal_out->id, target_pin->id))
                    return fail(error, 0, "could not connect an input boundary terminal");
            }
        } else if (source_selected && !destination_selected) {
            auto found = boundary_output_ports.find(out->id);
            if (found == boundary_output_ports.end()) {
                if (next_internal_id == kInvalidComponent) return fail(error, 0, "not enough component IDs for output terminals");
                const auto terminal_id = next_internal_id++;
                const auto port_id = "out_boundary_" + std::to_string(boundary_counter++);
                const auto terminal_position = Vec2{300.0F, 36.0F + 28.0F * static_cast<float>(boundary_counter)};
                if (!definition.implementation.add_component_with_id(terminal_id, ComponentType::Output, terminal_position))
                    return fail(error, 0, "could not create an output boundary terminal");
                definition.ports.push_back({port_id, "Output " + std::to_string(output_index + boundary_counter), PinDirection::Output, terminal_id});
                found = boundary_output_ports.emplace(out->id, port_id).first;
                const auto* terminal = definition.implementation.find_component(terminal_id);
                const auto* selected_component = definition.implementation.find_component(out->owner);
                const auto* source_pin = selected_component ? find_pin_by_key(*selected_component, PinDirection::Output, out->key) : nullptr;
                if (!terminal || terminal->inputs.empty() || !source_pin ||
                    !definition.implementation.connect(source_pin->id, terminal->inputs.front().id))
                    return fail(error, 0, "could not connect an output boundary terminal");
            }
        }
    }

    const auto final_input_count = static_cast<std::size_t>(std::count_if(definition.ports.begin(), definition.ports.end(),
        [](const CustomPort& port) { return port.direction == PinDirection::Input; }));
    const auto final_output_count = definition.ports.size() - final_input_count;
    definition.size.y = std::max(definition.size.y,
        48.0F + 32.0F * static_cast<float>(std::max(final_input_count, final_output_count)));

    // Every Input/Output in the implementation must be represented by exactly one port.
    PersistenceError shape_error;
    if (!validate_definition_shape(definition, shape_error)) return fail(error, 0, shape_error.describe());

    CircuitEditor candidate;
    ComponentId max_root_id = 0;
    for (const auto& component : root.components()) max_root_id = std::max(max_root_id, component.id);
    if (max_root_id >= kInvalidComponent - 1U) return fail(error, 0, "no component ID remains for the new instance");
    const ComponentId instance_id = root.components().empty() ? 0U : max_root_id + 1U;

    for (const auto& component : root.components()) {
        if (selected.contains(component.id)) continue;
        if (!clone_component(component, candidate, library, component.position))
            return fail(error, 0, "could not preserve unselected component " + std::to_string(component.id));
    }
    if (!candidate.add_custom_component_with_id(instance_id, definition.id, origin,
                                                 layouts(definition), definition.size))
        return fail(error, 0, "could not create replacement custom component instance");

    auto get_instance_pin = [&](PinDirection direction, std::string_view port_id) -> const Pin* {
        const auto* instance = candidate.find_component(instance_id);
        return instance ? find_pin_by_key(*instance, direction, port_id) : nullptr;
    };
    for (const auto& wire : root.wires()) {
        const auto* out = root.find_pin(wire.output_pin);
        const auto* in = root.find_pin(wire.input_pin);
        if (!out || !in) return fail(error, 0, "source circuit contains an invalid wire");
        const bool source_selected = selected.contains(out->owner);
        const bool destination_selected = selected.contains(in->owner);
        if (source_selected && destination_selected) continue;
        if (!source_selected && !destination_selected) {
            if (!clone_wire(root, wire, candidate)) return fail(error, 0, "could not preserve an external wire");
            continue;
        }
        if (!source_selected && destination_selected) {
            std::string port_id;
            if (const auto direct = input_port_for_pin.find(in->id); direct != input_port_for_pin.end())
                return fail(error, 0, "external connection maps to an Input source instead of an input port");
            const auto boundary = boundary_input_ports.find(in->id);
            if (boundary == boundary_input_ports.end()) return fail(error, 0, "missing external input port mapping");
            port_id = boundary->second;
            const auto* source_component = candidate.find_component(out->owner);
            const auto* source_pin = source_component ? find_pin_by_key(*source_component, PinDirection::Output, out->key) : nullptr;
            const auto* target_pin = get_instance_pin(PinDirection::Input, port_id);
            if (!source_pin || !target_pin || !candidate.connect(source_pin->id, target_pin->id))
                return fail(error, 0, "could not reconnect an external input wire");
        } else if (source_selected && !destination_selected) {
            const auto boundary = boundary_output_ports.find(out->id);
            if (boundary == boundary_output_ports.end()) return fail(error, 0, "missing external output port mapping");
            const auto* target_component = candidate.find_component(in->owner);
            const auto* target_pin = target_component ? find_pin_by_key(*target_component, PinDirection::Input, in->key) : nullptr;
            const auto* source_pin = get_instance_pin(PinDirection::Output, boundary->second);
            if (!source_pin || !target_pin || !candidate.connect(source_pin->id, target_pin->id))
                return fail(error, 0, "could not reconnect an external output wire");
        }
    }

    if (!library.add(definition, error)) return false;
    root.replace_with(std::move(candidate));
    root.select_component(instance_id);
    created_instance = instance_id;
    error = {};
    return true;
}

} // namespace hcs::editor
