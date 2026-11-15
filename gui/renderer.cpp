#include "renderer.hpp"

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

std::array<std::uint8_t, 7> glyph(char c) noexcept {
    switch (c) {
        case 'A': return {14,17,17,31,17,17,17}; case 'B': return {30,17,17,30,17,17,30};
        case 'C': return {14,17,16,16,16,17,14}; case 'D': return {30,17,17,17,17,17,30};
        case 'E': return {31,16,16,30,16,16,31}; case 'F': return {31,16,16,30,16,16,16};
        case 'G': return {14,17,16,23,17,17,15}; case 'H': return {17,17,17,31,17,17,17};
        case 'I': return {14,4,4,4,4,4,14}; case 'J': return {7,2,2,2,18,18,12};
        case 'K': return {17,18,20,24,20,18,17}; case 'L': return {16,16,16,16,16,16,31};
        case 'M': return {17,27,21,21,17,17,17}; case 'N': return {17,25,21,19,17,17,17};
        case 'O': return {14,17,17,17,17,17,14}; case 'P': return {30,17,17,30,16,16,16};
        case 'Q': return {14,17,17,17,21,18,13}; case 'R': return {30,17,17,30,20,18,17};
        case 'S': return {15,16,16,14,1,1,30}; case 'T': return {31,4,4,4,4,4,4};
        case 'U': return {17,17,17,17,17,17,14}; case 'V': return {17,17,17,17,17,10,4};
        case 'W': return {17,17,17,21,21,21,10}; case 'X': return {17,17,10,4,10,17,17};
        case 'Y': return {17,17,10,4,4,4,4}; case 'Z': return {31,1,2,4,8,16,31};
        case '0': return {14,17,19,21,25,17,14}; case '1': return {4,12,4,4,4,4,14};
        case '2': return {14,17,1,2,4,8,31}; case '3': return {30,1,1,14,1,1,30};
        case '4': return {2,6,10,18,31,2,2}; case '5': return {31,16,16,30,1,1,30};
        case '6': return {14,16,16,30,17,17,14}; case '7': return {31,1,2,4,8,8,8};
        case '8': return {14,17,17,14,17,17,14}; case '9': return {14,17,17,15,1,1,14};
        case '-': return {0,0,0,31,0,0,0}; case '+': return {0,4,4,31,4,4,0};
        case ':': return {0,4,4,0,4,4,0}; case '.': return {0,0,0,0,0,12,12};
        case '/': return {1,2,2,4,8,8,16}; case '(': return {2,4,8,8,8,4,2};
        case ')': return {8,4,2,2,2,4,8}; case '=': return {0,31,0,31,0,0,0};
        case '_': return {0,0,0,0,0,0,31}; case ' ': return {0,0,0,0,0,0,0};
        default: return {31,17,21,21,21,17,31};
    }
}

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

void CircuitRenderer::text(SDL_Renderer* renderer, float x, float y, const char* value, float scale, SDL_Color color) {
    if (!value || scale <= 0.0F) return;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float cursor = x;
    for (const char* p = value; *p != '\0'; ++p) {
        const auto rows = glyph(*p >= 'a' && *p <= 'z' ? static_cast<char>(*p - 'a' + 'A') : *p);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::uint8_t col = 0; col < 5; ++col) {
                if ((rows[row] & static_cast<std::uint8_t>(1U << (4U - col))) == 0) continue;
                const SDL_FRect pixel{cursor + static_cast<float>(col) * scale,
                                      y + static_cast<float>(row) * scale, scale, scale};
                (void)SDL_RenderFillRect(renderer, &pixel);
            }
        }
        cursor += 6.0F * scale;
    }
}

void CircuitRenderer::render(SDL_Renderer* renderer,
                             const editor::CircuitEditor& circuit,
                             const editor::CircuitRuntime& runtime,
                             const editor::Camera2D& camera,
                             bool show_grid,
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
        const auto width = component.size.x * camera.zoom();
        const auto height = component.size.y * camera.zoom();
        const auto selected = circuit.selection().component == component.id;
        fill_rect(renderer, top_left.x, top_left.y, width, height, kComponentFill);
        outline_rect(renderer, top_left.x, top_left.y, width, height, selected ? kSelection : kComponentBorder);

        if (camera.zoom() >= 0.45F) {
            const auto label_scale = std::clamp(camera.zoom() * 1.35F, 0.65F, 1.65F);
            const auto label_width = static_cast<float>(std::char_traits<char>::length(editor::component_name(component.type))) * 6.0F * label_scale;
            auto label_y = top_left.y + (height - 7.0F * label_scale) * 0.5F;
            if (component.type == editor::ComponentType::Input || component.type == editor::ComponentType::Output)
                label_y = top_left.y + 7.0F * camera.zoom();
            text(renderer, top_left.x + (width - label_width) * 0.5F, label_y,
                 editor::component_name(component.type), label_scale, kText);
        }

        if (component.type == editor::ComponentType::Input) {
            const char* value = component.input_value ? "1" : "0";
            const auto scale = std::clamp(camera.zoom() * 2.0F, 1.0F, 2.5F);
            text(renderer, top_left.x + width * 0.5F - 2.5F * scale,
                 top_left.y + height * 0.55F, value, scale, component.input_value ? kLogicOne : kLogicZero);
        } else if (component.type == editor::ComponentType::Output && !component.inputs.empty()) {
            const auto state = runtime.signal_for_pin(component.inputs.front().id);
            const char* value = state == editor::SignalState::One ? "1" : state == editor::SignalState::Zero ? "0" : "?";
            const auto scale = std::clamp(camera.zoom() * 2.0F, 1.0F, 2.5F);
            text(renderer, top_left.x + width * 0.5F - 2.5F * scale,
                 top_left.y + height * 0.55F, value, scale, state_color(state));
        }

        if (camera.zoom() >= 0.65F && !component.inputs.empty()) {
            for (std::size_t i = 0; i < component.inputs.size(); ++i) {
                const char* pin_label = nullptr;
                if (component.type == editor::ComponentType::DFlipFlop)
                    pin_label = i == 0 ? "D" : "CLK";
                else if (component.type == editor::ComponentType::And ||
                         component.type == editor::ComponentType::Or ||
                         component.type == editor::ComponentType::Xor)
                    pin_label = i == 0 ? "A" : "B";
                if (pin_label) {
                    const auto pin_screen = camera.world_to_screen(component.position + component.inputs[i].local_position);
                    text(renderer, pin_screen.x + 8.0F * camera.zoom(), pin_screen.y - 4.0F * camera.zoom(),
                         pin_label, std::clamp(camera.zoom() * 0.8F, 0.65F, 1.0F), kMuted);
                }
            }
        }

        const auto draw_pins = [&](const std::vector<editor::Pin>& pins) {
            for (const auto& pin : pins) {
                const auto world = component.position + pin.local_position;
                draw_pin(renderer, camera.world_to_screen(world), runtime.signal_for_pin(pin.id), camera.zoom());
            }
        };
        draw_pins(component.inputs);
        draw_pins(component.outputs);
    }
}

} // namespace hcs::gui
