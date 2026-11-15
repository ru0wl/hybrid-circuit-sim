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
} // namespace

Pin& CircuitEditor::append_pin(Component& component, PinDirection direction, Vec2 local_position) {
    Pin pin;
    pin.id = next_pin_id_++;
    pin.direction = direction;
    pin.owner = component.id;
    pin.local_position = local_position;
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
    }

    components_.push_back(std::move(component));
    if (id >= next_component_id_) next_component_id_ = id + 1;
    mark_structure_changed();
    if (type == ComponentType::Input && input_value) mark_input_changed();
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
    return owner ? owner->position + pin->local_position : Vec2{};
}

std::optional<ComponentId> CircuitEditor::component_at(Vec2 world) const noexcept {
    for (auto it = components_.rbegin(); it != components_.rend(); ++it) {
        const auto& c = *it;
        if (world.x >= c.position.x && world.y >= c.position.y &&
            world.x <= c.position.x + c.size.x && world.y <= c.position.y + c.size.y) return c.id;
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
                const auto distance = squared_distance(world, component.position + pin.local_position);
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
}

void CircuitEditor::select_wire(std::optional<WireId> id) noexcept {
    selection_.wire = id;
    selection_.component.reset();
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
    }
    return "UNKNOWN";
}

} // namespace hcs::editor
