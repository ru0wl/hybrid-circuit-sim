#pragma once

#include "hcs/editor/model.hpp"
#include "hcs/editor/persistence.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace hcs::editor {

enum class SymbolShapeType : std::uint8_t {
    Rectangle, RoundedRectangle, Circle, Ellipse, Line,
    Polyline, Polygon, Arc, Path
};

enum class SymbolPortOrientation : std::uint8_t { Left, Right, Top, Bottom, Angle };
enum class SymbolTextAlignment : std::uint8_t { Left, Center, Right };
enum class SymbolFontRole : std::uint8_t { Ui, Code };
enum class SymbolAlignment : std::uint8_t { Left, HorizontalCenter, Right, Top, VerticalCenter, Bottom };

struct SymbolPoint { float x{}; float y{}; };

struct SymbolShape {
    std::string id;
    SymbolShapeType type{SymbolShapeType::Rectangle};
    float x{8.0F};
    float y{8.0F};
    float width{134.0F};
    float height{68.0F};
    float radius{8.0F};
    float start_angle{};
    float end_angle{360.0F};
    std::vector<SymbolPoint> points;
    std::string fill{"#202630"};
    std::string stroke{"#D0D7E2"};
    float stroke_width{2.0F};
    bool visible{true};
    bool locked{false};
};

struct SymbolText {
    std::string id;
    std::string value;
    float x{};
    float y{};
    float size{12.0F};
    std::string color{"#D0D7E2"};
    SymbolTextAlignment alignment{SymbolTextAlignment::Center};
    SymbolFontRole font_role{SymbolFontRole::Ui};
    bool visible{true};
    bool locked{false};
};

struct SymbolPath {
    std::string id;
    std::vector<SymbolPoint> points;
    bool closed{false};
    std::string fill{"none"};
    std::string stroke{"#D0D7E2"};
    float stroke_width{2.0F};
    bool visible{true};
    bool locked{false};
};

using SymbolObject = std::variant<SymbolShape, SymbolPath, SymbolText>;

struct SymbolPortVisual {
    std::string port_id;
    float x{};
    float y{};
    SymbolPortOrientation orientation{SymbolPortOrientation::Left};
    float angle_degrees{};
    std::string label;
    bool show_label{true};
    bool visible{true};
};

struct SymbolDefinition {
    unsigned version{1};
    Vec2 size{150.0F, 84.0F};
    // Vector order is the painter's order (back to front).
    std::vector<SymbolObject> objects;
    std::vector<SymbolPortVisual> ports;
};

struct SymbolError {
    std::size_t line{};
    std::string message;
    [[nodiscard]] std::string describe() const;
};

struct SymbolTransform {
    std::uint8_t rotation_quadrants{};
    bool flip_horizontal{false};
    bool flip_vertical{false};
};

[[nodiscard]] SymbolDefinition make_default_symbol(Vec2 size,
                                                    const std::vector<std::pair<std::string, std::string>>& ports = {});
[[nodiscard]] SymbolDefinition make_symbol_preset(std::string_view preset, Vec2 size,
                                                   const std::vector<std::pair<std::string, std::string>>& ports = {});
[[nodiscard]] bool validate_symbol(const SymbolDefinition& symbol, SymbolError& error);
[[nodiscard]] bool serialize_symbol(const SymbolDefinition& symbol, std::string& text, SymbolError& error);
[[nodiscard]] bool parse_symbol(std::string_view text, SymbolDefinition& destination, SymbolError& error);

[[nodiscard]] std::string_view symbol_shape_name(SymbolShapeType type) noexcept;
[[nodiscard]] std::string_view symbol_orientation_name(SymbolPortOrientation orientation) noexcept;
[[nodiscard]] Vec2 transform_symbol_point(Vec2 point, Vec2 size, const SymbolTransform& transform) noexcept;
[[nodiscard]] Vec2 transformed_symbol_size(Vec2 size, const SymbolTransform& transform) noexcept;
[[nodiscard]] Vec2 inverse_transform_symbol_point(Vec2 point, Vec2 size,
                                                  const SymbolTransform& transform) noexcept;
[[nodiscard]] bool symbol_hit_test(const SymbolDefinition& symbol, Vec2 point,
                                   float tolerance = 5.0F) noexcept;
[[nodiscard]] std::optional<std::string> symbol_object_at(const SymbolDefinition& symbol,
                                                           Vec2 point, float tolerance = 5.0F);
[[nodiscard]] bool move_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                      Vec2 delta) noexcept;
[[nodiscard]] std::optional<std::size_t> symbol_object_vertex_at(
    const SymbolDefinition& symbol, std::string_view id, Vec2 point,
    float tolerance = 8.0F) noexcept;
[[nodiscard]] bool move_symbol_object_vertex(SymbolDefinition& symbol, std::string_view id,
                                             std::size_t vertex, Vec2 position) noexcept;
[[nodiscard]] std::optional<std::size_t> insert_symbol_object_vertex(
    SymbolDefinition& symbol, std::string_view id, Vec2 point,
    float tolerance = 12.0F);
[[nodiscard]] bool remove_symbol_object_vertex(SymbolDefinition& symbol, std::string_view id,
                                               std::size_t vertex) noexcept;
[[nodiscard]] bool set_symbol_path_closed(SymbolDefinition& symbol, std::string_view id,
                                          bool closed) noexcept;
[[nodiscard]] bool resize_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                        Vec2 size_delta) noexcept;
[[nodiscard]] bool reorder_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                         int direction) noexcept;
[[nodiscard]] bool align_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                       SymbolAlignment alignment) noexcept;
[[nodiscard]] bool remove_symbol_object(SymbolDefinition& symbol, std::string_view id) noexcept;
[[nodiscard]] bool rotate_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                         float degrees) noexcept;
[[nodiscard]] bool duplicate_symbol_object(SymbolDefinition& symbol, std::string_view id,
                                           std::string new_id);
[[nodiscard]] std::optional<std::string> symbol_port_at(const SymbolDefinition& symbol,
                                                        Vec2 point, float tolerance = 8.0F);
[[nodiscard]] bool move_symbol_port(SymbolDefinition& symbol, std::string_view port_id,
                                    Vec2 position, SymbolPortOrientation orientation,
                                    float angle_degrees = 0.0F) noexcept;
[[nodiscard]] bool set_symbol_port_label(SymbolDefinition& symbol, std::string_view port_id,
                                         std::string label, bool visible = true);
[[nodiscard]] bool set_symbol_port_orientation(SymbolDefinition& symbol, std::string_view port_id,
                                               SymbolPortOrientation orientation,
                                               float angle_degrees = 0.0F) noexcept;

} // namespace hcs::editor
