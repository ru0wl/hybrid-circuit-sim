#pragma once

#include "hcs/editor/custom_components.hpp"
#include "font_manager.hpp"

#include <SDL3/SDL.h>

namespace hcs::gui {

class SymbolRenderer final {
public:
    static void draw(SDL_Renderer* renderer,
                     const editor::CustomComponentDefinition& definition,
                     editor::Vec2 screen_origin, float zoom,
                     const editor::SymbolTransform& transform = {},
                     bool show_port_labels = true);
    [[nodiscard]] static bool hit_test(const editor::CustomComponentDefinition& definition,
                                       editor::Vec2 world_position, editor::Vec2 point,
                                       const editor::SymbolTransform& transform = {},
                                       float tolerance = 5.0F);
};

} // namespace hcs::gui
