#pragma once

#include "hcs/editor/model.hpp"

namespace hcs::editor {

struct Rect {
    float x{};
    float y{};
    float width{};
    float height{};
};

class Camera2D final {
public:
    void set_viewport(Rect viewport) noexcept { viewport_ = viewport; }
    [[nodiscard]] Rect viewport() const noexcept { return viewport_; }
    [[nodiscard]] Vec2 position() const noexcept { return position_; }
    void set_position(Vec2 position) noexcept { position_ = position; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }

    [[nodiscard]] Vec2 screen_to_world(Vec2 screen) const noexcept;
    [[nodiscard]] Vec2 world_to_screen(Vec2 world) const noexcept;
    void pan_screen(Vec2 delta) noexcept;
    void zoom_at(Vec2 screen_anchor, float wheel_delta) noexcept;

private:
    Rect viewport_{0.0F, 72.0F, 1280.0F, 648.0F};
    Vec2 position_{0.0F, 0.0F};
    float zoom_{1.0F};
};

} // namespace hcs::editor
