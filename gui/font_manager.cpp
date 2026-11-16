#include "font_manager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>

#ifdef HCS_HAVE_SDL3_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

namespace hcs::gui {
namespace {

struct FontState {
    SDL_Renderer* renderer{nullptr};
    float display_scale{1.0F};
#ifdef HCS_HAVE_SDL3_TTF
    std::string ui_path;
    std::string code_path;
    std::unordered_map<int, TTF_Font*> ui_fonts;
    std::unordered_map<int, TTF_Font*> code_fonts;
    bool ttf_initialized{false};
#endif
    std::string ui_name{"bitmap fallback"};
    std::string code_name{"bitmap fallback"};
    bool initialized{false};
};

FontState& state() {
    static FontState value;
    return value;
}

#ifdef HCS_HAVE_SDL3_TTF
template <std::size_t N>
std::string find_font_path(const std::array<const char*, N>& candidates) {
    for (const char* candidate : candidates) {
        if (!std::filesystem::exists(candidate)) continue;
        TTF_Font* probe = TTF_OpenFont(candidate, 14.0F);
        if (!probe) continue;
        TTF_CloseFont(probe);
        return candidate;
    }
    return {};
}

TTF_Font* font_for_role(FontRole role, int point_size) {
    auto& current = state();
    auto& cache = role == FontRole::Ui ? current.ui_fonts : current.code_fonts;
    const auto found = cache.find(point_size);
    if (found != cache.end()) return found->second;

    const std::string& path = role == FontRole::Ui ? current.ui_path : current.code_path;
    if (path.empty()) return nullptr;
    TTF_Font* font = TTF_OpenFont(path.c_str(), static_cast<float>(point_size));
    if (!font) return nullptr;
    cache.emplace(point_size, font);
    return font;
}

TTF_Font* resolved_font(FontRole role, float scale) {
    const float base_size = role == FontRole::Ui ? 12.5F : 12.0F;
    const int point_size = std::clamp(
        static_cast<int>(std::lround(base_size * scale * state().display_scale)), 7, 42);
    TTF_Font* font = font_for_role(role, point_size);
    if (!font) font = font_for_role(role == FontRole::Ui ? FontRole::Code : FontRole::Ui, point_size);
    return font;
}
#endif

void draw_bitmap(SDL_Renderer* renderer, float x, float y, const char* value,
                 float scale, SDL_Color color) {
    // Keep the fallback pixel grid aligned to whole pixels to avoid fractional
    // rectangles being resampled by the renderer.
    static constexpr std::array<std::array<unsigned char, 7>, 96> glyphs{{
        {{0,0,0,0,0,0,0}}, {{4,4,4,4,4,0,4}}, {{10,10,10,0,0,0,0}}, {{10,10,31,10,31,10,10}},
        {{4,15,20,14,5,30,4}}, {{24,25,2,4,8,19,3}}, {{12,18,20,8,21,18,13}}, {{4,4,8,0,0,0,0}},
        {{2,4,8,8,8,4,2}}, {{8,4,2,2,2,4,8}}, {{0,4,21,14,21,4,0}}, {{0,4,4,31,4,4,0}},
        {{0,0,0,0,0,4,8}}, {{0,0,0,31,0,0,0}}, {{0,0,0,0,0,12,12}}, {{1,2,2,4,8,8,16}},
        {{14,17,19,21,25,17,14}}, {{4,12,4,4,4,4,14}}, {{14,17,1,2,4,8,31}}, {{30,1,1,14,1,1,30}},
        {{2,6,10,18,31,2,2}}, {{31,16,16,30,1,1,30}}, {{6,8,16,30,17,17,14}}, {{31,1,2,4,8,8,8}},
        {{14,17,17,14,17,17,14}}, {{14,17,17,15,1,2,12}}, {{0,12,12,0,12,12,0}}, {{0,12,12,0,12,4,8}},
        {{2,4,8,16,8,4,2}}, {{0,0,31,0,31,0,0}}, {{8,4,2,1,2,4,8}}, {{14,17,1,2,4,0,4}},
        {{14,17,23,21,23,16,14}}, {{14,17,17,31,17,17,17}}, {{30,17,17,30,17,17,30}}, {{14,17,16,16,16,17,14}},
        {{28,18,17,17,17,18,28}}, {{31,16,16,30,16,16,31}}, {{31,16,16,30,16,16,16}}, {{14,17,16,23,17,17,15}},
        {{17,17,17,31,17,17,17}}, {{14,4,4,4,4,4,14}}, {{7,2,2,2,2,18,12}}, {{17,18,20,24,20,18,17}},
        {{16,16,16,16,16,16,31}}, {{17,27,21,21,17,17,17}}, {{17,25,21,19,17,17,17}}, {{14,17,17,17,17,17,14}},
        {{30,17,17,30,16,16,16}}, {{14,17,17,17,21,18,13}}, {{30,17,17,30,20,18,17}}, {{15,16,16,14,1,1,30}},
        {{31,4,4,4,4,4,4}}, {{17,17,17,17,17,17,14}}, {{17,17,17,17,17,10,4}}, {{17,17,17,21,21,21,10}},
        {{17,17,10,4,10,17,17}}, {{17,17,10,4,4,4,4}}, {{31,1,2,4,8,16,31}}, {{14,8,8,8,8,8,14}},
        {{16,8,8,4,2,2,1}}, {{14,2,2,2,2,2,14}}, {{4,10,17,0,0,0,0}}, {{0,0,0,0,0,0,31}},
        {{8,4,2,0,0,0,0}}, {{0,0,14,1,15,17,15}}, {{16,16,22,25,17,17,30}}, {{0,0,14,16,16,17,14}},
        {{1,1,13,19,17,17,15}}, {{0,0,14,17,31,16,14}}, {{6,9,8,28,8,8,8}}, {{0,0,15,17,15,1,14}},
        {{16,16,22,25,17,17,17}}, {{4,0,12,4,4,4,14}}, {{2,0,6,2,2,18,12}}, {{16,16,18,20,24,20,18}},
        {{12,4,4,4,4,4,14}}, {{0,0,26,21,21,21,21}}, {{0,0,22,25,17,17,17}}, {{0,0,14,17,17,17,14}},
        {{0,0,30,17,30,16,16}}, {{0,0,15,17,15,1,1}}, {{0,0,22,25,16,16,16}}, {{0,0,15,16,14,1,30}},
        {{8,8,28,8,8,9,6}}, {{0,0,17,17,17,19,13}}, {{0,0,17,17,17,10,4}}, {{0,0,17,17,21,21,10}},
        {{0,0,17,10,4,10,17}}, {{0,0,17,17,15,1,14}}, {{0,0,31,2,4,8,31}}, {{2,4,4,8,4,4,2}},
        {{4,4,4,4,4,4,4}}, {{8,4,4,2,4,4,8}}, {{0,4,21,10,0,0,0}}, {{0,0,0,0,0,0,0}}
    }};
    if (!renderer || !value || scale <= 0.0F) return;
    const float pixel_scale = std::max(1.0F, std::round(scale));
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    float cursor = std::round(x);
    const float top = std::round(y);
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        const auto ch = *p >= 32 && *p < 128 ? static_cast<std::size_t>(*p - 32) : static_cast<std::size_t>('?' - 32);
        for (std::size_t row = 0; row < 7; ++row) {
            for (std::size_t col = 0; col < 5; ++col) {
                if ((glyphs[ch][row] & static_cast<unsigned char>(1U << (4U - col))) == 0) continue;
                const SDL_FRect pixel{cursor + static_cast<float>(col) * pixel_scale,
                                      top + static_cast<float>(row) * pixel_scale,
                                      pixel_scale, pixel_scale};
                (void)SDL_RenderFillRect(renderer, &pixel);
            }
        }
        cursor += 6.0F * pixel_scale;
    }
}

} // namespace

bool FontManager::initialize(SDL_Renderer* renderer, float display_scale) {
    auto& current = state();
    if (current.initialized) return current.renderer == renderer;
    current.renderer = renderer;
    set_display_scale(display_scale);
#ifdef HCS_HAVE_SDL3_TTF
    if (TTF_Init()) {
        current.ttf_initialized = true;
        current.ui_path = find_font_path(std::array<const char*, 7>{
            "/usr/share/fonts/TTF/Inter-Regular.ttf",
            "/usr/share/fonts/TTF/InterVariable.ttf",
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
            "/usr/share/fonts/opentype/inter/Inter-Regular.otf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        });
        current.code_path = find_font_path(std::array<const char*, 7>{
            "/usr/share/fonts/TTF/JetBrainsMono-Regular.ttf",
            "/usr/share/fonts/TTF/Iosevka-Regular.ttf",
            "/usr/share/fonts/TTF/IBMPlexMono-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
            "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
            "/usr/share/fonts/truetype/jetbrains-mono/JetBrainsMono-Regular.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        });
        if (current.ui_path.empty()) current.ui_path = current.code_path;
        if (current.code_path.empty()) current.code_path = current.ui_path;
        if (!current.ui_path.empty()) current.ui_name = std::filesystem::path(current.ui_path).filename().string();
        if (!current.code_path.empty()) current.code_name = std::filesystem::path(current.code_path).filename().string();
    }
#endif
    current.initialized = true;
    return true;
}

void FontManager::shutdown() noexcept {
    auto& current = state();
#ifdef HCS_HAVE_SDL3_TTF
    for (auto& [size, font] : current.ui_fonts) {
        (void)size;
        if (font) TTF_CloseFont(font);
    }
    for (auto& [size, font] : current.code_fonts) {
        (void)size;
        if (font) TTF_CloseFont(font);
    }
    current.ui_fonts.clear();
    current.code_fonts.clear();
    current.ui_path.clear();
    current.code_path.clear();
    if (current.ttf_initialized) TTF_Quit();
    current.ttf_initialized = false;
#endif
    current.renderer = nullptr;
    current.initialized = false;
    current.ui_name = "bitmap fallback";
    current.code_name = "bitmap fallback";
}

void FontManager::set_display_scale(float scale) noexcept {
    state().display_scale = std::clamp(std::isfinite(scale) ? scale : 1.0F, 0.75F, 4.0F);
}

bool FontManager::using_ttf() noexcept {
#ifdef HCS_HAVE_SDL3_TTF
    return !state().ui_path.empty() || !state().code_path.empty();
#else
    return false;
#endif
}

const char* FontManager::ui_font_name() noexcept { return state().ui_name.c_str(); }
const char* FontManager::code_font_name() noexcept { return state().code_name.c_str(); }

SDL_FPoint FontManager::measure(const char* value, float scale, FontRole role) {
    if (!value || scale <= 0.0F) return SDL_FPoint{0.0F, 0.0F};
#ifdef HCS_HAVE_SDL3_TTF
    if (TTF_Font* font = resolved_font(role, scale)) {
        int width = 0;
        int height = 0;
        if (TTF_GetStringSize(font, value, 0, &width, &height)) {
            return SDL_FPoint{static_cast<float>(width), static_cast<float>(height)};
        }
    }
#else
    (void)role;
#endif
    const float pixel_scale = std::max(1.0F, std::round(scale * state().display_scale));
    const auto byte_count = static_cast<float>(std::strlen(value));
    return SDL_FPoint{byte_count * 6.0F * pixel_scale, 7.0F * pixel_scale};
}

void FontManager::draw(SDL_Renderer* renderer, float x, float y, const char* value,
                       float scale, SDL_Color color, FontRole role) {
    if (!renderer || !value || scale <= 0.0F) return;
#ifdef HCS_HAVE_SDL3_TTF
    if (TTF_Font* font = resolved_font(role, scale)) {
        SDL_Surface* surface = TTF_RenderText_Blended(font, value, 0, color);
        if (surface) {
            SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
            if (texture) {
                // Render at the glyph's native raster size. Scaling a small fixed-size
                // font texture at every call made labels soft, especially at large zoom.
                const SDL_FRect destination{std::round(x), std::round(y),
                                             static_cast<float>(surface->w),
                                             static_cast<float>(surface->h)};
                (void)SDL_RenderTexture(renderer, texture, nullptr, &destination);
                SDL_DestroyTexture(texture);
            }
            SDL_DestroySurface(surface);
            return;
        }
    }
#else
    (void)role;
#endif
    const float fallback_scale = std::max(1.0F, std::round(scale * state().display_scale));
    draw_bitmap(renderer, x, y, value, fallback_scale, color);
}

} // namespace hcs::gui
