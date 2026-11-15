#pragma once

#include "hcs/editor/camera.hpp"
#include "hcs/editor/model.hpp"
#include "hcs/editor/runtime.hpp"

#include <SDL3/SDL.h>

#include <optional>

namespace hcs::gui {

struct WirePreview {
    editor::Vec2 start{};
    editor::Vec2 end{};
};

class CircuitRenderer final {
public:
    void render(SDL_Renderer* renderer,
                const editor::CircuitEditor& circuit,
                const editor::CircuitRuntime& runtime,
                const editor::Camera2D& camera,
                bool show_grid,
                std::optional<WirePreview> preview = std::nullopt) const;

    static void fill_rect(SDL_Renderer* renderer, float x, float y, float width, float height,
                          SDL_Color color);
    static void outline_rect(SDL_Renderer* renderer, float x, float y, float width, float height,
                             SDL_Color color);
    static void line(SDL_Renderer* renderer, editor::Vec2 a, editor::Vec2 b, SDL_Color color);
    static void text(SDL_Renderer* renderer, float x, float y, const char* value,
                     float scale, SDL_Color color);
};

} // namespace hcs::gui
