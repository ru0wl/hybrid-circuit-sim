#pragma once

#include "hcs/circuit.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hcs::editor {

using ComponentId = std::uint32_t;
using PinId = std::uint32_t;
using WireId = std::uint32_t;

constexpr ComponentId kInvalidComponent = UINT32_MAX;
constexpr PinId kInvalidPin = UINT32_MAX;
constexpr WireId kInvalidWire = UINT32_MAX;

struct Vec2 {
    float x{};
    float y{};

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
    friend constexpr Vec2 operator*(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
};

enum class ComponentType : std::uint8_t {
    Input,
    Output,
    And,
    Or,
    Not,
    Xor,
    DFlipFlop,
    Custom,
};

enum class PinDirection : std::uint8_t { Input, Output };

enum class SignalState : std::uint8_t { Zero, One, Unknown, HighImpedance, Error };

struct Pin {
    PinId id{kInvalidPin};
    PinDirection direction{PinDirection::Input};
    ComponentId owner{kInvalidComponent};
    SignalId simulation_signal{kInvalidSignal};
    Vec2 local_position{};
    // Stable symbolic key (built-in pin name or custom definition port ID).
    std::string key;
};

struct Component {
    ComponentId id{kInvalidComponent};
    ComponentType type{ComponentType::Input};
    Vec2 position{};
    Vec2 size{};
    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    bool input_value{false};
    bool dff_initial_state{false};
    std::string definition_id;
    std::uint8_t rotation_quadrants{0};
    bool flip_horizontal{false};
    bool flip_vertical{false};
};

struct CustomPortLayout {
    std::string id;
    std::string name;
    PinDirection direction{PinDirection::Input};
    Vec2 local_position{};
    bool explicit_position{false};
};

struct Wire {
    WireId id{kInvalidWire};
    PinId output_pin{kInvalidPin};
    PinId input_pin{kInvalidPin};
};

struct Selection {
    std::optional<ComponentId> component;
    std::optional<WireId> wire;
    std::vector<ComponentId> components;
};

class CircuitEditor final {
public:
    [[nodiscard]] ComponentId add_component(ComponentType type, Vec2 position);
    [[nodiscard]] bool add_component_with_id(ComponentId id, ComponentType type, Vec2 position,
                                             bool input_value = false, bool dff_initial_state = false);
    [[nodiscard]] ComponentId add_custom_component(std::string definition_id, Vec2 position,
                                                   const std::vector<CustomPortLayout>& ports, Vec2 size = {});
    [[nodiscard]] bool add_custom_component_with_id(ComponentId id, std::string definition_id,
                                                    Vec2 position, const std::vector<CustomPortLayout>& ports,
                                                    Vec2 size = {});
    // Rebuilds every instance of one definition by stable port ID. Connected removed
    // or direction-changed pins reject the operation without modifying the editor.
    [[nodiscard]] bool update_custom_component_layout(std::string_view definition_id,
                                                       const std::vector<CustomPortLayout>& ports,
                                                       Vec2 size);
    [[nodiscard]] bool remove_component(ComponentId id);
    [[nodiscard]] bool move_component(ComponentId id, Vec2 position);
    [[nodiscard]] bool set_component_transform(ComponentId id, std::uint8_t rotation_quadrants,
                                               bool flip_horizontal, bool flip_vertical);
    [[nodiscard]] bool rotate_component(ComponentId id, int quarter_turns = 1);
    [[nodiscard]] bool flip_component(ComponentId id, bool horizontal);
    [[nodiscard]] static Vec2 component_bounds_size(const Component& component) noexcept;
    [[nodiscard]] bool set_input_value(ComponentId id, bool value);
    [[nodiscard]] bool set_dff_initial_state(ComponentId id, bool value);
    [[nodiscard]] bool toggle_input(ComponentId id);

    [[nodiscard]] std::optional<WireId> connect(PinId output_pin, PinId input_pin);
    [[nodiscard]] bool remove_wire(WireId id);
    void clear();
    // Replace document contents while guaranteeing that a synchronized runtime sees
    // a new revision, even when the replacement has the same graph shape.
    void replace_with(CircuitEditor&& replacement) noexcept;

    [[nodiscard]] Component* find_component(ComponentId id) noexcept;
    [[nodiscard]] const Component* find_component(ComponentId id) const noexcept;
    [[nodiscard]] Pin* find_pin(PinId id) noexcept;
    [[nodiscard]] const Pin* find_pin(PinId id) const noexcept;
    [[nodiscard]] Wire* find_wire(WireId id) noexcept;
    [[nodiscard]] const Wire* find_wire(WireId id) const noexcept;

    [[nodiscard]] const std::vector<Component>& components() const noexcept { return components_; }
    [[nodiscard]] const std::vector<Wire>& wires() const noexcept { return wires_; }
    [[nodiscard]] std::uint64_t structural_revision() const noexcept { return structural_revision_; }
    [[nodiscard]] std::uint64_t input_revision() const noexcept { return input_revision_; }
    [[nodiscard]] std::uint64_t document_revision() const noexcept { return document_revision_; }

    [[nodiscard]] Vec2 pin_world_position(PinId id) const noexcept;
    [[nodiscard]] std::optional<ComponentId> component_at(Vec2 world) const noexcept;
    [[nodiscard]] std::optional<PinId> pin_at(Vec2 world, float radius) const noexcept;
    [[nodiscard]] std::optional<WireId> wire_at(Vec2 world, float tolerance) const noexcept;

    void select_component(std::optional<ComponentId> id) noexcept;
    void toggle_component_selection(ComponentId id) noexcept;
    [[nodiscard]] bool is_component_selected(ComponentId id) const noexcept;
    [[nodiscard]] const std::vector<ComponentId>& selected_components() const noexcept { return selection_.components; }
    void select_wire(std::optional<WireId> id) noexcept;
    void clear_selection() noexcept;
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }

    // Runtime integration assigns mappings; these IDs are references into the current
    // synchronized simulator and are not signal values.
    void assign_simulation_signal(PinId pin, SignalId signal) noexcept;

private:
    Pin& append_pin(Component& component, PinDirection direction, Vec2 local_position);
    void mark_structure_changed() noexcept;
    void mark_input_changed() noexcept;
    void mark_document_changed() noexcept;

    std::vector<Component> components_;
    std::vector<Wire> wires_;
    ComponentId next_component_id_{0};
    PinId next_pin_id_{0};
    WireId next_wire_id_{0};
    std::uint64_t structural_revision_{1};
    std::uint64_t input_revision_{1};
    std::uint64_t document_revision_{1};
    Selection selection_{};
};

[[nodiscard]] const char* component_name(ComponentType type) noexcept;

} // namespace hcs::editor
