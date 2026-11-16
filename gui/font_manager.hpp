#pragma once

#include <SDL3/SDL.h>

namespace hcs::gui {

enum class FontRole { Ui, Code };

class FontManager final {
public:
    [[nodiscard]] static bool initialize(SDL_Renderer* renderer, float display_scale = 1.0F);
    static void shutdown() noexcept;
    static void set_display_scale(float scale) noexcept;
    [[nodiscard]] static bool using_ttf() noexcept;
    [[nodiscard]] static const char* ui_font_name() noexcept;
    [[nodiscard]] static const char* code_font_name() noexcept;
    [[nodiscard]] static SDL_FPoint measure(const char* value, float scale,
                                            FontRole role = FontRole::Ui);
    static void draw(SDL_Renderer* renderer, float x, float y, const char* value,
                     float scale, SDL_Color color, FontRole role = FontRole::Ui);
};

} // namespace hcs::gui
