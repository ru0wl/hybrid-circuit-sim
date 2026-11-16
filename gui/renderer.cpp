#include "renderer.hpp"
#include "symbol_renderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace hcs::gui {
namespace {
constexpr SDL_Color kBackground{15, 19, 27, 255};
constexpr SDL_Color kGrid{28, 36, 47, 255};
constexpr SDL_Color kGridMajor{37, 47, 60, 255};
constexpr SDL_Color kComponentFill{34, 43, 56, 255};
constexpr SDL_Color kComponentBorder{105, 124, 147, 255};
constexpr SDL_Color kSelection{89, 178, 255, 255};
constexpr SDL_Color kText{222, 231, 241, 255};
constexpr SDL_Color kMuted{133, 151, 173, 255};
constexpr SDL_Color kLogicZero{109, 145, 174, 255};
constexpr SDL_Color kLogicOne{255, 190, 82, 255};
constexpr SDL_Color kError{235, 92, 102, 255};


SDL_Color state_color(editor::SignalState state) noexcept {
    switch (state) {
        case editor::SignalState::Zero: return kLogicZero;
        case editor::SignalState::One: return kLogicOne;
        case editor::SignalState::Unknown: return SDL_Color{149, 153, 166, 255};
        case editor::SignalState::HighImpedance: return SDL_Color{113, 190, 170, 255};
        case editor::SignalState::Error: return kError;
    }
    return kError;
}

void draw_pin(SDL_Renderer* renderer, editor::Vec2 screen, editor::SignalState state, float zoom) {
    const auto radius = std::clamp(4.2F * zoom, 2.5F, 6.0F);
    CircuitRenderer::fill_rect(renderer, screen.x - radius, screen.y - radius,
                               radius * 2.0F, radius * 2.0F, state_color(state));
    CircuitRenderer::outline_rect(renderer, screen.x - radius, screen.y - radius,
                                  radius * 2.0F, radius * 2.0F, SDL_Color{10, 14, 20, 255});
}

void draw_grid(SDL_Renderer* renderer, const editor::Camera2D& camera) {
    const auto viewport = camera.viewport();
    const auto top_left = camera.screen_to_world({viewport.x, viewport.y});
    const auto bottom_right = camera.screen_to_world({viewport.x + viewport.width, viewport.y + viewport.height});
    constexpr float spacing = 40.0F;
    const auto first_x = std::floor(top_left.x / spacing) * spacing;
    const auto first_y = std::floor(top_left.y / spacing) * spacing;
    for (float x = first_x; x <= bottom_right.x + spacing; x += spacing) {
        const auto sx = camera.world_to_screen({x, 0.0F}).x;
        const auto major = std::fmod(std::abs(x / spacing), 5.0F) < 0.001F;
        CircuitRenderer::line(renderer, {sx, viewport.y}, {sx, viewport.y + viewport.height}, major ? kGridMajor : kGrid);
    }
    for (float y = first_y; y <= bottom_right.y + spacing; y += spacing) {
        const auto sy = camera.world_to_screen({0.0F, y}).y;
        const auto major = std::fmod(std::abs(y / spacing), 5.0F) < 0.001F;
        CircuitRenderer::line(renderer, {viewport.x, sy}, {viewport.x + viewport.width, sy}, major ? kGridMajor : kGrid);
    }
}
} // namespace

void CircuitRenderer::fill_rect(SDL_Renderer* renderer, float x, float y, float width, float height, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{x, y, width, height};
    (void)SDL_RenderFillRect(renderer, &rect);
}

void CircuitRenderer::outline_rect(SDL_Renderer* renderer, float x, float y, float width, float height, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    const SDL_FRect rect{x, y, width, height};
    (void)SDL_RenderRect(renderer, &rect);
}

void CircuitRenderer::line(SDL_Renderer* renderer, editor::Vec2 a, editor::Vec2 b, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    (void)SDL_RenderLine(renderer, a.x, a.y, b.x, b.y);
}

SDL_FPoint CircuitRenderer::text_size(const char* value, float scale, FontRole role) {
    return FontManager::measure(value, scale, role);
}

void CircuitRenderer::text(SDL_Renderer* renderer, float x, float y, const char* value,
                           float scale, SDL_Color color, FontRole role) {
    FontManager::draw(renderer, x, y, value, scale, color, role);
}

void CircuitRenderer::render(SDL_Renderer* renderer,
                             const editor::CircuitEditor& circuit,
                             const editor::CircuitRuntime& runtime,
                             const editor::Camera2D& camera,
                             bool show_grid,
                             const editor::ComponentLibrary& library,
                             std::optional<WirePreview> preview) const {
    SDL_SetRenderDrawColor(renderer, kBackground.r, kBackground.g, kBackground.b, kBackground.a);
    (void)SDL_RenderClear(renderer);
    if (show_grid) draw_grid(renderer, camera);

    for (const auto& wire : circuit.wires()) {
        const auto start = camera.world_to_screen(circuit.pin_world_position(wire.output_pin));
        const auto end = camera.world_to_screen(circuit.pin_world_position(wire.input_pin));
        if (circuit.selection().wire == wire.id) {
            const editor::Vec2 offset{0.0F, 2.0F};
            line(renderer, start + offset, end + offset, kSelection);
            line(renderer, start - offset, end - offset, kSelection);
        }
        const auto color = state_color(runtime.signal_for_pin(wire.output_pin));
        line(renderer, start, end, color);
    }
    if (preview) {
        line(renderer, camera.world_to_screen(preview->start), camera.world_to_screen(preview->end), kSelection);
    }

    for (const auto& component : circuit.components()) {
        const auto top_left = camera.world_to_screen(component.position);
        const auto bounds = editor::CircuitEditor::component_bounds_size(component);
        const auto width = bounds.x * camera.zoom();
        const auto height = bounds.y * camera.zoom();
        const auto selected = circuit.is_component_selected(component.id);
        const auto custom = component.type == editor::ComponentType::Custom;
        const auto* definition = custom ? library.find(component.definition_id) : nullptr;
        if (custom && definition) {
            const editor::SymbolTransform transform{component.rotation_quadrants,
                component.flip_horizontal, component.flip_vertical};
            SymbolRenderer::draw(renderer, *definition, top_left, camera.zoom(), transform, true);
            if (selected) outline_rect(renderer, top_left.x - 2.0F, top_left.y - 2.0F,
                                       width + 4.0F, height + 4.0F, kSelection);
        } else {
            fill_rect(renderer, top_left.x, top_left.y, width, height, kComponentFill);
            outline_rect(renderer, top_left.x, top_left.y, width, height,
                         selected ? kSelection : kComponentBorder);
            if (camera.zoom() >= 0.45F) {
                const auto label_scale = std::clamp(camera.zoom() * 1.35F, 0.65F, 1.65F);
                const std::string label = editor::component_name(component.type);
                const auto label_size = text_size(label.c_str(), label_scale);
                auto label_y = top_left.y + (height - label_size.y) * 0.5F;
                if (component.type == editor::ComponentType::Input || component.type == editor::ComponentType::Output)
                    label_y = top_left.y + 7.0F * camera.zoom();
                text(renderer, top_left.x + (width - label_size.x) * 0.5F, label_y,
                     label.c_str(), label_scale, kText);
            }
        }

        if (component.type == editor::ComponentType::Input) {
            const char* value = component.input_value ? "1" : "0";
            const auto scale = std::clamp(camera.zoom() * 2.0F, 1.0F, 2.5F);
            const auto value_size = text_size(value, scale);
            text(renderer, top_left.x + (width - value_size.x) * 0.5F,
                 top_left.y + (height - value_size.y) * 0.62F,
                 value, scale, component.input_value ? kLogicOne : kLogicZero);
        } else if (component.type == editor::ComponentType::Output && !component.inputs.empty()) {
            const auto state = runtime.signal_for_pin(component.inputs.front().id);
            const char* value = state == editor::SignalState::One ? "1" : state == editor::SignalState::Zero ? "0" : "?";
            const auto scale = std::clamp(camera.zoom() * 2.0F, 1.0F, 2.5F);
            const auto value_size = text_size(value, scale);
            text(renderer, top_left.x + (width - value_size.x) * 0.5F,
                 top_left.y + (height - value_size.y) * 0.62F,
                 value, scale, state_color(state));
        }

        if (!custom && camera.zoom() >= 0.65F && !component.inputs.empty()) {
            for (std::size_t i = 0; i < component.inputs.size(); ++i) {
                const char* pin_label = nullptr;
                if (component.type == editor::ComponentType::DFlipFlop)
                    pin_label = i == 0 ? "D" : "CLK";
                else if (component.type == editor::ComponentType::And ||
                         component.type == editor::ComponentType::Or ||
                         component.type == editor::ComponentType::Xor)
                    pin_label = i == 0 ? "A" : "B";
                if (pin_label) {
                    const auto pin_screen = camera.world_to_screen(circuit.pin_world_position(component.inputs[i].id));
                    const auto label_scale = std::clamp(camera.zoom() * 0.8F, 0.65F, 1.0F);
                    const auto label_size = text_size(pin_label, label_scale);
                    text(renderer, pin_screen.x + 8.0F * camera.zoom(),
                         pin_screen.y - label_size.y * 0.5F, pin_label, label_scale, kMuted);
                }
            }
        }

        const auto draw_pins = [&](const std::vector<editor::Pin>& pins) {
            for (const auto& pin : pins) {
                draw_pin(renderer, camera.world_to_screen(circuit.pin_world_position(pin.id)),
                         runtime.signal_for_pin(pin.id), camera.zoom());
            }
        };
        draw_pins(component.inputs);
        draw_pins(component.outputs);
    }
}

} // namespace hcs::gui
