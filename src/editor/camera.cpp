#include "hcs/editor/camera.hpp"

#include <algorithm>
#include <cmath>

namespace hcs::editor {

Vec2 Camera2D::screen_to_world(Vec2 screen) const noexcept {
    const Vec2 center{viewport_.x + viewport_.width * 0.5F, viewport_.y + viewport_.height * 0.5F};
    return position_ + (screen - center) * (1.0F / zoom_);
}

Vec2 Camera2D::world_to_screen(Vec2 world) const noexcept {
    const Vec2 center{viewport_.x + viewport_.width * 0.5F, viewport_.y + viewport_.height * 0.5F};
    return center + (world - position_) * zoom_;
}

void Camera2D::pan_screen(Vec2 delta) noexcept {
    position_ = position_ - delta * (1.0F / zoom_);
}

void Camera2D::zoom_at(Vec2 screen_anchor, float wheel_delta) noexcept {
    const auto before = screen_to_world(screen_anchor);
    const auto factor = std::pow(1.12F, wheel_delta);
    zoom_ = std::clamp(zoom_ * factor, 0.18F, 3.5F);
    const auto after = screen_to_world(screen_anchor);
    position_ = position_ + (before - after);
}

} // namespace hcs::editor
