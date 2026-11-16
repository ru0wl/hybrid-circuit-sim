#include "hcs/editor/model.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace hcs::editor {
namespace {
float squared_distance(Vec2 a, Vec2 b) noexcept {
    const auto dx = a.x - b.x;
    const auto dy = a.y - b.y;
    return dx * dx + dy * dy;
}

float point_segment_distance_squared(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const auto ab = b - a;
    const auto ap = p - a;
    const auto length_squared = ab.x * ab.x + ab.y * ab.y;
    if (length_squared <= std::numeric_limits<float>::epsilon()) return squared_distance(p, a);
    const auto t = std::clamp((ap.x * ab.x + ap.y * ab.y) / length_squared, 0.0F, 1.0F);
    const Vec2 closest{a.x + ab.x * t, a.y + ab.y * t};
    return squared_distance(p, closest);
}

Vec2 transform_local(Vec2 point, Vec2 size, const Component& component) noexcept {
    const Vec2 center{size.x * 0.5F, size.y * 0.5F};
    point = point - center;
    if (component.flip_horizontal) point.x = -point.x;
    if (component.flip_vertical) point.y = -point.y;
    switch (component.rotation_quadrants % 4U) {
        case 1: point = {-point.y, point.x}; break;
        case 2: point = {-point.x, -point.y}; break;
        case 3: point = {point.y, -point.x}; break;
        default: break;
    }
    const auto bounds = CircuitEditor::component_bounds_size(component);
    return point + Vec2{bounds.x * 0.5F, bounds.y * 0.5F};
}
} // namespace

Pin& CircuitEditor::append_pin(Component& component, PinDirection direction, Vec2 local_position) {
    Pin pin;
    pin.id = next_pin_id_++;
    pin.direction = direction;
    pin.owner = component.id;
    pin.local_position = local_position;
    switch (direction) {
        case PinDirection::Input: pin.key = "in"; break;
        case PinDirection::Output: pin.key = "out"; break;
    }
    auto& collection = direction == PinDirection::Input ? component.inputs : component.outputs;
    collection.push_back(pin);
    return collection.back();
}

ComponentId CircuitEditor::add_component(ComponentType type, Vec2 position) {
    if (next_component_id_ == kInvalidComponent) return kInvalidComponent;
    const auto id = next_component_id_;
    if (!add_component_with_id(id, type, position)) return kInvalidComponent;
    return id;
}

bool CircuitEditor::add_component_with_id(ComponentId id, ComponentType type, Vec2 position,
                                          bool input_value, bool dff_initial_state) {
    if (id == kInvalidComponent || find_component(id) != nullptr ||
        !std::isfinite(position.x) || !std::isfinite(position.y)) return false;

    Component component;
    component.id = id;
    component.type = type;
    component.position = position;
    component.input_value = input_value;
    component.dff_initial_state = dff_initial_state;

    switch (type) {
        case ComponentType::Input:
            component.size = {112.0F, 62.0F};
            append_pin(component, PinDirection::Output, {112.0F, 31.0F});
            break;
        case ComponentType::Output:
            component.size = {112.0F, 62.0F};
            append_pin(component, PinDirection::Input, {0.0F, 31.0F});
            break;
        case ComponentType::And:
        case ComponentType::Or:
        case ComponentType::Xor:
            component.size = {116.0F, 82.0F};
            append_pin(component, PinDirection::Input, {0.0F, 27.0F});
            append_pin(component, PinDirection::Input, {0.0F, 55.0F});
            append_pin(component, PinDirection::Output, {116.0F, 41.0F});
            break;
        case ComponentType::Not:
            component.size = {112.0F, 68.0F};
            append_pin(component, PinDirection::Input, {0.0F, 34.0F});
            append_pin(component, PinDirection::Output, {112.0F, 34.0F});
            break;
        case ComponentType::DFlipFlop:
            component.size = {128.0F, 92.0F};
            append_pin(component, PinDirection::Input, {0.0F, 29.0F});
            append_pin(component, PinDirection::Input, {0.0F, 67.0F});
            append_pin(component, PinDirection::Output, {128.0F, 48.0F});
            break;
        case ComponentType::Custom:
            return false;
    }
    if (component.type == ComponentType::Output || component.type == ComponentType::Not) {
        if (!component.inputs.empty()) component.inputs[0].key = "in";
    } else if (component.type == ComponentType::And || component.type == ComponentType::Or || component.type == ComponentType::Xor) {
        component.inputs[0].key = "in0";
        component.inputs[1].key = "in1";
    } else if (component.type == ComponentType::DFlipFlop) {
        component.inputs[0].key = "d";
        component.inputs[1].key = "clk";
        component.outputs[0].key = "q";
    }

    components_.push_back(std::move(component));
    if (id >= next_component_id_) next_component_id_ = id + 1;
    mark_structure_changed();
    if (type == ComponentType::Input && input_value) mark_input_changed();
    return true;
}


ComponentId CircuitEditor::add_custom_component(std::string definition_id, Vec2 position,
                                                   const std::vector<CustomPortLayout>& ports, Vec2 size) {
    if (next_component_id_ == kInvalidComponent) return kInvalidComponent;
    const auto id = next_component_id_;
    if (!add_custom_component_with_id(id, std::move(definition_id), position, ports, size)) return kInvalidComponent;
    return id;
}

bool CircuitEditor::add_custom_component_with_id(ComponentId id, std::string definition_id,
                                                  Vec2 position, const std::vector<CustomPortLayout>& ports,
                                                  Vec2 requested_size) {
    if (id == kInvalidComponent || find_component(id) != nullptr || definition_id.empty() ||
        definition_id.size() > 128 ||
        !std::isfinite(position.x) || !std::isfinite(position.y)) return false;
    const bool has_requested_size = requested_size.x != 0.0F || requested_size.y != 0.0F;
    if (has_requested_size && (!std::isfinite(requested_size.x) || !std::isfinite(requested_size.y) ||
        requested_size.x < 48.0F || requested_size.y < 48.0F ||
        requested_size.x > 10'000'000.0F || requested_size.y > 10'000'000.0F)) return false;
    Component component;
    component.id = id;
    component.type = ComponentType::Custom;
    component.position = position;
    component.definition_id = std::move(definition_id);
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    std::vector<std::string> seen_port_ids;
    for (const auto& port : ports) {
        if (port.id.empty() || port.id.size() > 128 || port.name.empty() || port.name.size() > 256 ||
            std::find(seen_port_ids.begin(), seen_port_ids.end(), port.id) != seen_port_ids.end() ||
            (port.direction != PinDirection::Input && port.direction != PinDirection::Output)) return false;
        seen_port_ids.push_back(port.id);
        if (port.direction == PinDirection::Input) ++input_count; else ++output_count;
    }
    const float height = std::max(72.0F, 42.0F + 24.0F * static_cast<float>(std::max(input_count, output_count)));
    component.size = requested_size.x > 0.0F && requested_size.y > 0.0F
        ? requested_size : Vec2{150.0F, height};
    std::size_t input_index = 0;
    std::size_t output_index = 0;
    for (const auto& port : ports) {
        Pin pin;
        pin.id = next_pin_id_++;
        pin.owner = id;
        pin.direction = port.direction;
        pin.key = port.id;
        if (port.direction == PinDirection::Input) {
            const auto slot = static_cast<float>(input_index++ + 1);
            pin.local_position = port.explicit_position ? port.local_position :
                Vec2{0.0F, std::min(component.size.y - 12.0F, slot * component.size.y / static_cast<float>(input_count + 1))};
            component.inputs.push_back(std::move(pin));
        } else {
            const auto slot = static_cast<float>(output_index++ + 1);
            pin.local_position = port.explicit_position ? port.local_position :
                Vec2{component.size.x, std::min(component.size.y - 12.0F, slot * component.size.y / static_cast<float>(output_count + 1))};
            component.outputs.push_back(std::move(pin));
        }
    }
    components_.push_back(std::move(component));
    if (id >= next_component_id_) next_component_id_ = id + 1;
    mark_structure_changed();
    return true;
}

bool CircuitEditor::update_custom_component_layout(std::string_view definition_id,
                                                     const std::vector<CustomPortLayout>& ports,
                                                     Vec2 size) {
    if (definition_id.empty() || !std::isfinite(size.x) || !std::isfinite(size.y) ||
        size.x < 48.0F || size.y < 48.0F || size.x > 10'000'000.0F || size.y > 10'000'000.0F)
        return false;

    std::vector<std::string> ids;
    std::size_t input_count = 0;
    std::size_t output_count = 0;
    for (const auto& port : ports) {
        if (port.id.empty() || port.id.size() > 128 || port.name.empty() || port.name.size() > 256 ||
            std::find(ids.begin(), ids.end(), port.id) != ids.end() ||
            (port.direction != PinDirection::Input && port.direction != PinDirection::Output)) return false;
        ids.push_back(port.id);
        if (port.direction == PinDirection::Input) ++input_count;
        else ++output_count;
    }

    CircuitEditor candidate = *this;
    bool changed = false;
    bool logical_changed = false;
    for (auto& component : candidate.components_) {
        if (component.type != ComponentType::Custom || component.definition_id != definition_id) continue;
        changed = true;
        const auto has_wire = [&](const Pin& pin) {
            return std::any_of(candidate.wires_.begin(), candidate.wires_.end(), [&](const Wire& wire) {
                return wire.output_pin == pin.id || wire.input_pin == pin.id;
            });
        };
        for (const auto& old_pin : component.inputs) {
            const auto replacement = std::find_if(ports.begin(), ports.end(), [&](const CustomPortLayout& port) {
                return port.id == old_pin.key && port.direction == PinDirection::Input;
            });
            if (replacement == ports.end() && has_wire(old_pin)) return false;
        }
        for (const auto& old_pin : component.outputs) {
            const auto replacement = std::find_if(ports.begin(), ports.end(), [&](const CustomPortLayout& port) {
                return port.id == old_pin.key && port.direction == PinDirection::Output;
            });
            if (replacement == ports.end() && has_wire(old_pin)) return false;
        }
        for (const auto& old_pin : component.inputs) {
            if (std::none_of(ports.begin(), ports.end(), [&](const CustomPortLayout& port) { return port.id == old_pin.key && port.direction == PinDirection::Input; })) logical_changed = true;
        }
        for (const auto& old_pin : component.outputs) {
            if (std::none_of(ports.begin(), ports.end(), [&](const CustomPortLayout& port) { return port.id == old_pin.key && port.direction == PinDirection::Output; })) logical_changed = true;
        }

        std::vector<Pin> new_inputs;
        std::vector<Pin> new_outputs;
        new_inputs.reserve(input_count);
        new_outputs.reserve(output_count);
        std::size_t input_index = 0;
        std::size_t output_index = 0;
        for (const auto& port : ports) {
            auto& old_pins = port.direction == PinDirection::Input ? component.inputs : component.outputs;
            const auto old = std::find_if(old_pins.begin(), old_pins.end(), [&](const Pin& pin) {
                return pin.key == port.id;
            });
            Pin pin;
            if (old != old_pins.end()) {
                pin = *old;
            } else {
                if (candidate.next_pin_id_ == kInvalidPin) return false;
                pin.id = candidate.next_pin_id_++;
                pin.owner = component.id;
                pin.direction = port.direction;
                pin.key = port.id;
                pin.simulation_signal = kInvalidSignal;
                logical_changed = true;
            }
            if (port.direction == PinDirection::Input) {
                const float slot = static_cast<float>(++input_index);
                pin.local_position = port.explicit_position ? port.local_position : Vec2{0.0F, std::min(size.y - 12.0F,
                    slot * size.y / static_cast<float>(input_count + 1))};
                new_inputs.push_back(std::move(pin));
            } else {
                const float slot = static_cast<float>(++output_index);
                pin.local_position = port.explicit_position ? port.local_position : Vec2{size.x, std::min(size.y - 12.0F,
                    slot * size.y / static_cast<float>(output_count + 1))};
                new_outputs.push_back(std::move(pin));
            }
        }
        component.inputs = std::move(new_inputs);
        component.outputs = std::move(new_outputs);
        component.size = size;
    }
    if (!changed) return true;
    components_ = std::move(candidate.components_);
    wires_ = std::move(candidate.wires_);
    next_pin_id_ = candidate.next_pin_id_;
    if (logical_changed) {
        for (auto& component : components_) {
            for (auto& pin : component.inputs) pin.simulation_signal = kInvalidSignal;
            for (auto& pin : component.outputs) pin.simulation_signal = kInvalidSignal;
        }
        mark_structure_changed();
    } else {
        mark_document_changed();
    }
    return true;
}

bool CircuitEditor::remove_component(ComponentId id) {
    const auto found = std::find_if(components_.begin(), components_.end(), [id](const Component& c) { return c.id == id; });
    if (found == components_.end()) return false;
    const auto owns_pin = [id](const Wire& wire, const CircuitEditor& editor) {
        const auto* out = editor.find_pin(wire.output_pin);
        const auto* in = editor.find_pin(wire.input_pin);
        return (out && out->owner == id) || (in && in->owner == id);
    };
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(), [&](const Wire& wire) { return owns_pin(wire, *this); }), wires_.end());
    components_.erase(found);
    if (selection_.component == id) selection_.component.reset();
    selection_.components.erase(std::remove(selection_.components.begin(), selection_.components.end(), id),
                                selection_.components.end());
    if (selection_.component && !find_component(*selection_.component)) selection_.component.reset();
    if (selection_.wire && !find_wire(*selection_.wire)) selection_.wire.reset();
    mark_structure_changed();
    return true;
}

bool CircuitEditor::move_component(ComponentId id, Vec2 position) {
    auto* component = find_component(id);
    if (!component || !std::isfinite(position.x) || !std::isfinite(position.y)) return false;
    if (component->position.x != position.x || component->position.y != position.y) {
        component->position = position;
        mark_document_changed();
    }
    return true;
}

bool CircuitEditor::set_component_transform(ComponentId id, std::uint8_t rotation_quadrants,
                                            bool flip_horizontal, bool flip_vertical) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::Custom) return false;
    rotation_quadrants = static_cast<std::uint8_t>(rotation_quadrants % 4U);
    if (component->rotation_quadrants != rotation_quadrants ||
        component->flip_horizontal != flip_horizontal || component->flip_vertical != flip_vertical) {
        component->rotation_quadrants = rotation_quadrants;
        component->flip_horizontal = flip_horizontal;
        component->flip_vertical = flip_vertical;
        mark_document_changed();
    }
    return true;
}

bool CircuitEditor::rotate_component(ComponentId id, int quarter_turns) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::Custom) return false;
    const int current = static_cast<int>(component->rotation_quadrants);
    const int next = ((current + quarter_turns) % 4 + 4) % 4;
    return set_component_transform(id, static_cast<std::uint8_t>(next), component->flip_horizontal, component->flip_vertical);
}

bool CircuitEditor::flip_component(ComponentId id, bool horizontal) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::Custom) return false;
    return set_component_transform(id, component->rotation_quadrants,
        horizontal ? !component->flip_horizontal : component->flip_horizontal,
        horizontal ? component->flip_vertical : !component->flip_vertical);
}

Vec2 CircuitEditor::component_bounds_size(const Component& component) noexcept {
    return component.rotation_quadrants % 2U == 0 ? component.size : Vec2{component.size.y, component.size.x};
}

bool CircuitEditor::set_input_value(ComponentId id, bool value) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::Input) return false;
    if (component->input_value != value) {
        component->input_value = value;
        mark_input_changed();
    }
    return true;
}

bool CircuitEditor::set_dff_initial_state(ComponentId id, bool value) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::DFlipFlop) return false;
    if (component->dff_initial_state != value) {
        component->dff_initial_state = value;
        mark_structure_changed();
    }
    return true;
}

bool CircuitEditor::toggle_input(ComponentId id) {
    auto* component = find_component(id);
    if (!component || component->type != ComponentType::Input) return false;
    component->input_value = !component->input_value;
    mark_input_changed();
    return true;
}

std::optional<WireId> CircuitEditor::connect(PinId output_pin, PinId input_pin) {
    const auto* output = find_pin(output_pin);
    const auto* input = find_pin(input_pin);
    if (!output || !input || output->direction != PinDirection::Output || input->direction != PinDirection::Input) return std::nullopt;
    const auto existing = std::find_if(wires_.begin(), wires_.end(), [input_pin](const Wire& wire) { return wire.input_pin == input_pin; });
    if (existing != wires_.end()) return std::nullopt;
    const auto id = next_wire_id_++;
    wires_.push_back(Wire{id, output_pin, input_pin});
    mark_structure_changed();
    return id;
}

bool CircuitEditor::remove_wire(WireId id) {
    const auto found = std::find_if(wires_.begin(), wires_.end(), [id](const Wire& wire) { return wire.id == id; });
    if (found == wires_.end()) return false;
    wires_.erase(found);
    if (selection_.wire == id) selection_.wire.reset();
    mark_structure_changed();
    return true;
}

void CircuitEditor::clear() {
    components_.clear();
    wires_.clear();
    selection_ = {};
    next_component_id_ = 0;
    next_pin_id_ = 0;
    next_wire_id_ = 0;
    mark_structure_changed();
}

void CircuitEditor::replace_with(CircuitEditor&& replacement) noexcept {
    if (this == &replacement) return;
    components_ = std::move(replacement.components_);
    wires_ = std::move(replacement.wires_);
    next_component_id_ = replacement.next_component_id_;
    next_pin_id_ = replacement.next_pin_id_;
    next_wire_id_ = replacement.next_wire_id_;
    selection_ = {};
    for (auto& component : components_) {
        for (auto& pin : component.inputs) pin.simulation_signal = kInvalidSignal;
        for (auto& pin : component.outputs) pin.simulation_signal = kInvalidSignal;
    }
    mark_structure_changed();
    mark_input_changed();
}

Component* CircuitEditor::find_component(ComponentId id) noexcept {
    const auto found = std::find_if(components_.begin(), components_.end(), [id](const Component& c) { return c.id == id; });
    return found == components_.end() ? nullptr : &*found;
}

const Component* CircuitEditor::find_component(ComponentId id) const noexcept {
    const auto found = std::find_if(components_.begin(), components_.end(), [id](const Component& c) { return c.id == id; });
    return found == components_.end() ? nullptr : &*found;
}

Pin* CircuitEditor::find_pin(PinId id) noexcept {
    for (auto& component : components_) {
        for (auto& pin : component.inputs) if (pin.id == id) return &pin;
        for (auto& pin : component.outputs) if (pin.id == id) return &pin;
    }
    return nullptr;
}

const Pin* CircuitEditor::find_pin(PinId id) const noexcept {
    for (const auto& component : components_) {
        for (const auto& pin : component.inputs) if (pin.id == id) return &pin;
        for (const auto& pin : component.outputs) if (pin.id == id) return &pin;
    }
    return nullptr;
}

Wire* CircuitEditor::find_wire(WireId id) noexcept {
    const auto found = std::find_if(wires_.begin(), wires_.end(), [id](const Wire& wire) { return wire.id == id; });
    return found == wires_.end() ? nullptr : &*found;
}

const Wire* CircuitEditor::find_wire(WireId id) const noexcept {
    const auto found = std::find_if(wires_.begin(), wires_.end(), [id](const Wire& wire) { return wire.id == id; });
    return found == wires_.end() ? nullptr : &*found;
}

Vec2 CircuitEditor::pin_world_position(PinId id) const noexcept {
    const auto* pin = find_pin(id);
    const auto* owner = pin ? find_component(pin->owner) : nullptr;
    return owner ? owner->position + transform_local(pin->local_position, owner->size, *owner) : Vec2{};
}

std::optional<ComponentId> CircuitEditor::component_at(Vec2 world) const noexcept {
    for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
        const auto& c = *it;
        const auto bounds = component_bounds_size(c);
        if (world.x >= c.position.x && world.y >= c.position.y &&
            world.x <= c.position.x + bounds.x && world.y <= c.position.y + bounds.y) return c.id;
    }
    return std::nullopt;
}

std::optional<PinId> CircuitEditor::pin_at(Vec2 world, float radius) const noexcept {
    const auto radius_squared = radius * radius;
    float best = radius_squared;
    std::optional<PinId> result;
    for (const auto& component : components_) {
        const auto inspect = [&](const std::vector<Pin>& pins) {
            for (const auto& pin : pins) {
                const auto distance = squared_distance(world, component.position + transform_local(pin.local_position, component.size, component));
                if (distance <= best) { best = distance; result = pin.id; }
            }
        };
        inspect(component.inputs);
        inspect(component.outputs);
    }
    return result;
}

std::optional<WireId> CircuitEditor::wire_at(Vec2 world, float tolerance) const noexcept {
    const auto tolerance_squared = tolerance * tolerance;
    for (auto it = wires_.rbegin(); it != wires_.rend(); ++it) {
        const auto a = pin_world_position(it->output_pin);
        const auto b = pin_world_position(it->input_pin);
        if (point_segment_distance_squared(world, a, b) <= tolerance_squared) return it->id;
    }
    return std::nullopt;
}

void CircuitEditor::select_component(std::optional<ComponentId> id) noexcept {
    selection_.component = id;
    selection_.wire.reset();
    selection_.components.clear();
    if (id && find_component(*id)) selection_.components.push_back(*id);
}

void CircuitEditor::toggle_component_selection(ComponentId id) noexcept {
    if (!find_component(id)) return;
    selection_.wire.reset();
    const auto found = std::find(selection_.components.begin(), selection_.components.end(), id);
    if (found == selection_.components.end()) selection_.components.push_back(id);
    else selection_.components.erase(found);
    selection_.component = selection_.components.empty()
        ? std::optional<ComponentId>{} : std::optional<ComponentId>{selection_.components.back()};
}

bool CircuitEditor::is_component_selected(ComponentId id) const noexcept {
    return std::find(selection_.components.begin(), selection_.components.end(), id) != selection_.components.end();
}

void CircuitEditor::select_wire(std::optional<WireId> id) noexcept {
    selection_.wire = id;
    selection_.component.reset();
    selection_.components.clear();
}

void CircuitEditor::clear_selection() noexcept { selection_ = {}; }

void CircuitEditor::assign_simulation_signal(PinId pin, SignalId signal) noexcept {
    if (auto* found = find_pin(pin)) found->simulation_signal = signal;
}

void CircuitEditor::mark_structure_changed() noexcept {
    ++structural_revision_;
    if (structural_revision_ == 0) ++structural_revision_;
    mark_document_changed();
}

void CircuitEditor::mark_input_changed() noexcept {
    ++input_revision_;
    if (input_revision_ == 0) ++input_revision_;
    mark_document_changed();
}

void CircuitEditor::mark_document_changed() noexcept {
    ++document_revision_;
    if (document_revision_ == 0) ++document_revision_;
}

const char* component_name(ComponentType type) noexcept {
    switch (type) {
        case ComponentType::Input: return "INPUT";
        case ComponentType::Output: return "OUTPUT";
        case ComponentType::And: return "AND";
        case ComponentType::Or: return "OR";
        case ComponentType::Not: return "NOT";
        case ComponentType::Xor: return "XOR";
        case ComponentType::DFlipFlop: return "DFF";
        case ComponentType::Custom: return "CUSTOM";
    }
    return "UNKNOWN";
}

} // namespace hcs::editor
