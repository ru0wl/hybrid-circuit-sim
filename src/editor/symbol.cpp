#include "hcs/editor/symbol.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>

namespace hcs::editor {
namespace {
constexpr std::size_t kMaximumSymbolBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumObjects = 4096;
constexpr std::size_t kMaximumPoints = 1024;
constexpr std::size_t kMaximumTextBytes = 16384;
constexpr float kMaximumCoordinate = 1'000'000.0F;

bool finite(float value) noexcept { return std::isfinite(value); }

struct SymbolBounds { float min_x{}, min_y{}, max_x{}, max_y{}; };

SymbolBounds text_bounds(const SymbolText& item) noexcept {
    const float width = std::max(item.size, static_cast<float>(item.value.size()) * item.size * 0.55F);
    float left = item.x;
    if (item.alignment == SymbolTextAlignment::Center) left -= width * 0.5F;
    else if (item.alignment == SymbolTextAlignment::Right) left -= width;
    return {left, item.y, left + width, item.y + item.size * 1.4F};
}
bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}
bool valid_color(std::string_view color, bool allow_none = false) {
    if (allow_none && color == "none") return true;
    if (color.size() != 7 && color.size() != 9) return false;
    if (color.front() != '#') return false;
    return std::all_of(color.begin() + 1, color.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
std::string float_text(float value) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return out.str();
}
bool parse_float(std::string_view source, float& output) {
    if (source.empty()) return false;
    float value{};
    const auto result = std::from_chars(source.data(), source.data() + source.size(), value,
                                        std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != source.data() + source.size() || !finite(value)) return false;
    output = value;
    return true;
}
bool parse_bool(std::string_view source, bool& value) {
    if (source == "1" || source == "true") { value = true; return true; }
    if (source == "0" || source == "false") { value = false; return true; }
    return false;
}
bool fail(SymbolError& error, std::size_t line, std::string message) {
    error.line = line;
    error.message = std::move(message);
    return false;
}
bool parse_shape_type(std::string_view name, SymbolShapeType& type) {
    constexpr std::pair<std::string_view, SymbolShapeType> values[] = {
        {"rect", SymbolShapeType::Rectangle}, {"rounded_rect", SymbolShapeType::RoundedRectangle},
        {"circle", SymbolShapeType::Circle}, {"ellipse", SymbolShapeType::Ellipse},
        {"line", SymbolShapeType::Line}, {"polyline", SymbolShapeType::Polyline},
        {"polygon", SymbolShapeType::Polygon}, {"arc", SymbolShapeType::Arc}, {"path", SymbolShapeType::Path}
    };
    for (const auto& [key, value] : values) if (key == name) { type = value; return true; }
    return false;
}
bool parse_orientation(std::string_view name, SymbolPortOrientation& orientation) {
    if (name == "left") orientation = SymbolPortOrientation::Left;
    else if (name == "right") orientation = SymbolPortOrientation::Right;
    else if (name == "top") orientation = SymbolPortOrientation::Top;
    else if (name == "bottom") orientation = SymbolPortOrientation::Bottom;
    else if (name == "angle") orientation = SymbolPortOrientation::Angle;
    else return false;
    return true;
}
std::string points_text(const std::vector<SymbolPoint>& points) {
    std::string result;
    for (const auto& point : points) {
        if (!result.empty()) result.push_back(';');
        result += float_text(point.x) + "," + float_text(point.y);
    }
    return result;
}
bool parse_points(std::string_view text, std::vector<SymbolPoint>& points) {
    points.clear();
    if (text.empty() || text.back() == ';') return false;
    while (!text.empty()) {
        const auto separator = text.find(';');
        const auto token = text.substr(0, separator);
        const auto comma = token.find(',');
        if (comma == std::string_view::npos || comma == 0 || comma + 1 == token.size()) return false;
        SymbolPoint point;
        if (!parse_float(token.substr(0, comma), point.x) || !parse_float(token.substr(comma + 1), point.y)) return false;
        points.push_back(point);
        if (points.size() > kMaximumPoints) return false;
        if (separator == std::string_view::npos) break;
        text.remove_prefix(separator + 1);
    }
    return points.size() >= 2;
}
std::map<std::string, std::string> parse_fields(std::istringstream& stream, bool& ok) {
    std::map<std::string, std::string> fields;
    while (true) {
        stream >> std::ws;
        if (stream.eof()) break;
        std::string key;
        char ch{};
        while (stream.get(ch) && ch != '=' && !std::isspace(static_cast<unsigned char>(ch))) key.push_back(ch);
        if (ch != '=' || key.empty()) { ok = false; return {}; }
        std::string value;
        stream >> std::ws;
        if (stream.peek() == '"') {
            if (!(stream >> std::quoted(value))) { ok = false; return {}; }
        } else {
            while (stream.get(ch) && !std::isspace(static_cast<unsigned char>(ch))) value.push_back(ch);
        }
        if (!fields.emplace(std::move(key), std::move(value)).second) {
            ok = false;
            return {};
        }
    }
    return fields;
}

bool take_float(const std::map<std::string, std::string>& fields, std::string_view key, float& value) {
    const auto it = fields.find(std::string(key));
    return it != fields.end() && parse_float(it->second, value);
}
bool take_string(const std::map<std::string, std::string>& fields, std::string_view key, std::string& value) {
    const auto it = fields.find(std::string(key));
    if (it == fields.end()) return false;
    value = it->second;
    return true;
}
bool take_bool(const std::map<std::string, std::string>& fields, std::string_view key, bool& value) {
    const auto it = fields.find(std::string(key));
    return it != fields.end() && parse_bool(it->second, value);
}
float point_segment_distance(Vec2 p, Vec2 a, Vec2 b) noexcept {
    const auto ab = b - a;
    const auto ap = p - a;
    const float len = ab.x * ab.x + ab.y * ab.y;
    if (len <= 0.000001F) return std::hypot(ap.x, ap.y);
    const float t = std::clamp((ap.x * ab.x + ap.y * ab.y) / len, 0.0F, 1.0F);
    return std::hypot(p.x - (a.x + ab.x * t), p.y - (a.y + ab.y * t));
}
bool point_in_polygon(Vec2 point, const std::vector<SymbolPoint>& points) noexcept {
    if (points.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto& a = points[i];
        const auto& b = points[j];
        const bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) /
                ((b.y - a.y) == 0.0F ? 0.000001F : (b.y - a.y)) + a.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

std::vector<SymbolPoint>* editable_points(SymbolObject& object) noexcept {
    return std::visit([](auto& item) -> std::vector<SymbolPoint>* {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, SymbolText>) return nullptr;
        else return &item.points;
    }, object);
}
const std::vector<SymbolPoint>* editable_points(const SymbolObject& object) noexcept {
    return std::visit([](const auto& item) -> const std::vector<SymbolPoint>* {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, SymbolText>) return nullptr;
        else return &item.points;
    }, object);
}
bool object_is_locked(const SymbolObject& object) noexcept {
    return std::visit([](const auto& item) { return item.locked; }, object);
}
bool object_is_closed(const SymbolObject& object) noexcept {
    return std::visit([](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, SymbolPath>) return item.closed;
        if constexpr (std::is_same_v<T, SymbolShape>) return item.type == SymbolShapeType::Polygon;
        return false;
    }, object);
}
std::size_t minimum_vertices(const SymbolObject& object) noexcept {
    return std::visit([](const auto& item) -> std::size_t {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, SymbolPath>) return item.closed ? 3U : 2U;
        if constexpr (std::is_same_v<T, SymbolShape>) {
            if (item.type == SymbolShapeType::Polygon) return 3U;
            if (item.type == SymbolShapeType::Polyline || item.type == SymbolShapeType::Path) return 2U;
        }
        return 0U;
    }, object);
}
bool valid_shape_type(SymbolShapeType type) noexcept {
    switch (type) {
        case SymbolShapeType::Rectangle: case SymbolShapeType::RoundedRectangle:
        case SymbolShapeType::Circle: case SymbolShapeType::Ellipse: case SymbolShapeType::Line:
        case SymbolShapeType::Polyline: case SymbolShapeType::Polygon: case SymbolShapeType::Arc:
        case SymbolShapeType::Path: return true;
    }
    return false;
}
bool valid_orientation(SymbolPortOrientation orientation) noexcept {
    switch (orientation) {
        case SymbolPortOrientation::Left: case SymbolPortOrientation::Right:
        case SymbolPortOrientation::Top: case SymbolPortOrientation::Bottom:
        case SymbolPortOrientation::Angle: return true;
    }
    return false;
}
} // namespace

std::string SymbolError::describe() const {
    return (line ? "line " + std::to_string(line) + ": " : std::string{}) + message;
}

std::string_view symbol_shape_name(SymbolShapeType type) noexcept {
    switch (type) {
        case SymbolShapeType::Rectangle: return "rect";
        case SymbolShapeType::RoundedRectangle: return "rounded_rect";
        case SymbolShapeType::Circle: return "circle";
        case SymbolShapeType::Ellipse: return "ellipse";
        case SymbolShapeType::Line: return "line";
        case SymbolShapeType::Polyline: return "polyline";
        case SymbolShapeType::Polygon: return "polygon";
        case SymbolShapeType::Arc: return "arc";
        case SymbolShapeType::Path: return "path";
    }
    return "rect";
}
std::string_view symbol_orientation_name(SymbolPortOrientation orientation) noexcept {
    switch (orientation) {
        case SymbolPortOrientation::Left: return "left";
        case SymbolPortOrientation::Right: return "right";
        case SymbolPortOrientation::Top: return "top";
        case SymbolPortOrientation::Bottom: return "bottom";
        case SymbolPortOrientation::Angle: return "angle";
    }
    return "left";
}

SymbolDefinition make_default_symbol(Vec2 size, const std::vector<std::pair<std::string, std::string>>& ports) {
    SymbolDefinition symbol;
    symbol.size = size;
    SymbolShape body;
    body.id = "body";
    body.type = SymbolShapeType::RoundedRectangle;
    body.x = 8.0F; body.y = 8.0F;
    body.width = std::max(32.0F, size.x - 16.0F);
    body.height = std::max(32.0F, size.y - 16.0F);
    symbol.objects.emplace_back(std::move(body));
    std::size_t input_count = 0, output_count = 0;
    for (const auto& port : ports) (port.second == "input" ? input_count : output_count)++;
    std::size_t in_index = 0, out_index = 0;
    for (const auto& [id, direction] : ports) {
        SymbolPortVisual visual;
        visual.port_id = id;
        visual.label = id;
        if (direction == "input") {
            const float slot = static_cast<float>(++in_index);
            visual.x = 0.0F;
            visual.y = size.y * slot / static_cast<float>(input_count + 1);
            visual.orientation = SymbolPortOrientation::Left;
        } else {
            const float slot = static_cast<float>(++out_index);
            visual.x = size.x;
            visual.y = size.y * slot / static_cast<float>(output_count + 1);
            visual.orientation = SymbolPortOrientation::Right;
        }
        symbol.ports.push_back(std::move(visual));
    }
    return symbol;
}

SymbolDefinition make_symbol_preset(std::string_view preset, Vec2 size,
                                    const std::vector<std::pair<std::string, std::string>>& ports) {
    auto symbol = make_default_symbol(size, ports);
    auto* body = std::get_if<SymbolShape>(&symbol.objects.front());
    if (!body) return symbol;
    if (preset == "circle") {
        body->type = SymbolShapeType::Ellipse;
        const float diameter = std::min(size.x, size.y) - 16.0F;
        body->width = diameter; body->height = diameter;
        body->x = (size.x - diameter) * 0.5F; body->y = (size.y - diameter) * 0.5F;
    } else if (preset == "triangle") {
        body->type = SymbolShapeType::Polygon;
        body->points = {{size.x * 0.5F, 8.0F}, {size.x - 8.0F, size.y - 8.0F}, {8.0F, size.y - 8.0F}};
        body->fill = "#202630";
    } else if (preset == "and" || preset == "or" || preset == "xor" || preset == "not" ||
               preset == "dff" || preset == "mux") {
        // Presets are only an editable starting contour; they carry no logic semantics.
        body->type = SymbolShapeType::Polygon;
        if (preset == "and") body->points = {{8,8},{size.x*0.55F,8},{size.x-8,size.y*0.5F},{size.x*0.55F,size.y-8},{8,size.y-8}};
        else if (preset == "or" || preset == "xor") body->points = {{8,8},{size.x*0.45F,8},{size.x-8,size.y*0.5F},{size.x*0.45F,size.y-8},{8,size.y-8},{size.x*0.28F,size.y*0.5F}};
        else if (preset == "not") body->points = {{8,8},{size.x-18,size.y*0.5F},{8,size.y-8}};
        else if (preset == "mux") body->points = {{20,8},{size.x-20,24},{size.x-20,size.y-24},{20,size.y-8}};
        else body->type = SymbolShapeType::RoundedRectangle;
    }
    return symbol;
}

bool validate_symbol(const SymbolDefinition& symbol, SymbolError& error) {
    if (symbol.version != 1) return fail(error, 0, "unsupported symbol version");
    if (!finite(symbol.size.x) || !finite(symbol.size.y) || symbol.size.x < 48.0F || symbol.size.y < 48.0F ||
        symbol.size.x > 4096.0F || symbol.size.y > 4096.0F) return fail(error, 0, "symbol dimensions must be finite and between 48 and 4096");
    if (symbol.objects.size() > kMaximumObjects) return fail(error, 0, "symbol object count exceeds 4096");
    if (symbol.ports.size() > 256) return fail(error, 0, "symbol port count exceeds 256");
    std::set<std::string> ids;
    auto valid_point = [](float x, float y) {
        return finite(x) && finite(y) && std::abs(x) <= kMaximumCoordinate && std::abs(y) <= kMaximumCoordinate;
    };
    auto inside_symbol = [&](float x, float y) {
        return valid_point(x, y) && x >= 0.0F && y >= 0.0F && x <= symbol.size.x && y <= symbol.size.y;
    };
    for (const auto& object : symbol.objects) {
        const bool ok = std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if (!valid_id(item.id) || !ids.insert(item.id).second) return false;
            if constexpr (std::is_same_v<T, SymbolShape>) {
                if (!valid_shape_type(item.type) || !finite(item.x) || !finite(item.y) || !finite(item.width) || !finite(item.height) ||
                    !finite(item.radius) || !finite(item.stroke_width) || !finite(item.start_angle) || !finite(item.end_angle) ||
                    std::abs(item.start_angle) > 3600.0F || std::abs(item.end_angle) > 3600.0F ||
                    (item.type != SymbolShapeType::Line && (item.width < 0.0F || item.height < 0.0F)) ||
                    item.radius < 0.0F || item.stroke_width < 0.0F || item.stroke_width > 64.0F ||
                    !inside_symbol(item.x, item.y) || !inside_symbol(item.x + item.width, item.y + item.height) ||
                    !valid_color(item.fill, true) || !valid_color(item.stroke, true) || item.points.size() > kMaximumPoints) return false;
                if ((item.type == SymbolShapeType::Polyline || item.type == SymbolShapeType::Polygon || item.type == SymbolShapeType::Path) && item.points.size() < 2) return false;
                if (item.type == SymbolShapeType::Polygon && item.points.size() < 3) return false;
                if (item.type == SymbolShapeType::Line && !item.points.empty() && item.points.size() != 2) return false;
                for (const auto& point : item.points) if (!inside_symbol(point.x, point.y)) return false;
                return true;
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                if (!finite(item.stroke_width) || item.stroke_width < 0.0F || item.stroke_width > 64.0F ||
                    !valid_color(item.fill, true) || !valid_color(item.stroke, true) || item.points.size() < (item.closed ? 3U : 2U) || item.points.size() > kMaximumPoints) return false;
                for (const auto& point : item.points) if (!inside_symbol(point.x, point.y)) return false;
                return true;
            } else {
                const bool valid_alignment = item.alignment == SymbolTextAlignment::Left ||
                    item.alignment == SymbolTextAlignment::Center || item.alignment == SymbolTextAlignment::Right;
                const bool valid_role = item.font_role == SymbolFontRole::Ui || item.font_role == SymbolFontRole::Code;
                if (!valid_alignment || !valid_role || item.value.size() > kMaximumTextBytes ||
                    !finite(item.x) || !finite(item.y) || !finite(item.size) || item.size < 4.0F ||
                    item.size > 128.0F || !valid_color(item.color)) return false;
                const auto bounds = text_bounds(item);
                return inside_symbol(bounds.min_x, bounds.min_y) && inside_symbol(bounds.max_x, bounds.max_y);
            }
        }, object);
        if (!ok) return fail(error, 0, "invalid object properties or duplicate object ID");
    }
    std::set<std::string> port_ids;
    for (const auto& port : symbol.ports) {
        if (!valid_id(port.port_id) || !port_ids.insert(port.port_id).second || !valid_orientation(port.orientation) || !valid_point(port.x, port.y) ||
            port.x < 0.0F || port.y < 0.0F || port.x > symbol.size.x || port.y > symbol.size.y ||
            port.label.size() > 256 || !std::all_of(port.label.begin(), port.label.end(), [](unsigned char c) { return c >= 0x20U; }) ||
            !finite(port.angle_degrees) || std::abs(port.angle_degrees) > 3600.0F)
            return fail(error, 0, "invalid or duplicate visual port; coordinates must be within symbol bounds and angles within +/-3600 degrees");
    }
    error = {};
    return true;
}

bool serialize_symbol(const SymbolDefinition& symbol, std::string& text, SymbolError& error) {
    if (!validate_symbol(symbol, error)) return false;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "HCSS 1\nsize width=" << float_text(symbol.size.x) << " height=" << float_text(symbol.size.y) << '\n';
    for (const auto& object : symbol.objects) {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, SymbolShape>) {
                out << "shape " << symbol_shape_name(item.type) << " id=" << item.id
                    << " x=" << float_text(item.x) << " y=" << float_text(item.y)
                    << " width=" << float_text(item.width) << " height=" << float_text(item.height)
                    << " radius=" << float_text(item.radius) << " start=" << float_text(item.start_angle)
                    << " end=" << float_text(item.end_angle) << " points=" << quoted(points_text(item.points))
                    << " fill=" << item.fill << " stroke=" << item.stroke << " stroke_width=" << float_text(item.stroke_width)
                    << " visible=" << (item.visible ? 1 : 0) << " locked=" << (item.locked ? 1 : 0) << '\n';
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                out << "path id=" << item.id << " points=" << quoted(points_text(item.points))
                    << " closed=" << (item.closed ? 1 : 0) << " fill=" << item.fill << " stroke=" << item.stroke
                    << " stroke_width=" << float_text(item.stroke_width) << " visible=" << (item.visible ? 1 : 0)
                    << " locked=" << (item.locked ? 1 : 0) << '\n';
            } else {
                out << "text id=" << item.id << " x=" << float_text(item.x) << " y=" << float_text(item.y)
                    << " size=" << float_text(item.size) << " color=" << item.color
                    << " align=" << (item.alignment == SymbolTextAlignment::Left ? "left" : item.alignment == SymbolTextAlignment::Right ? "right" : "center")
                    << " role=" << (item.font_role == SymbolFontRole::Code ? "code" : "ui")
                    << " visible=" << (item.visible ? 1 : 0) << " locked=" << (item.locked ? 1 : 0)
                    << " value=" << quoted(item.value) << '\n';
            }
        }, object);
    }
    for (const auto& port : symbol.ports) {
        out << "port id=" << port.port_id << " x=" << float_text(port.x) << " y=" << float_text(port.y)
            << " orientation=" << symbol_orientation_name(port.orientation)
            << " angle=" << float_text(port.angle_degrees) << " label=" << quoted(port.label)
            << " show_label=" << (port.show_label ? 1 : 0) << " visible=" << (port.visible ? 1 : 0) << '\n';
    }
    text = out.str();
    error = {};
    return true;
}

bool parse_symbol(std::string_view text, SymbolDefinition& destination, SymbolError& error) {
    if (text.size() > kMaximumSymbolBytes) return fail(error, 0, "symbol text exceeds 4 MiB");
    SymbolDefinition candidate;
    bool header = false, size_seen = false;
    std::size_t line_number = 0;
    while (!text.empty()) {
        ++line_number;
        const auto newline = text.find('\n');
        auto line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        if (line.empty() || line.starts_with('#')) continue;
        std::istringstream input{std::string(line)};
        std::string keyword;
        input >> keyword;
        if (!header) {
            unsigned version{};
            if (keyword != "HCSS" || !(input >> version) || version != 1) return fail(error, line_number, "expected symbol header 'HCSS 1'");
            std::string extra; if (input >> extra) return fail(error, line_number, "unexpected symbol header field");
            header = true; continue;
        }
        if (keyword == "size") {
            std::string width, height, extra;
            if (size_seen || !(input >> width >> height) || input >> extra || !width.starts_with("width=") || !height.starts_with("height=") ||
                !parse_float(std::string_view(width).substr(6), candidate.size.x) || !parse_float(std::string_view(height).substr(7), candidate.size.y))
                return fail(error, line_number, "expected one size declaration: size width=<number> height=<number>");
            size_seen = true; continue;
        }
        if (candidate.objects.size() + candidate.ports.size() >= kMaximumObjects) return fail(error, line_number, "symbol object count exceeds 4096");
        if (keyword == "shape") {
            std::string type_name;
            if (!(input >> type_name)) return fail(error, line_number, "shape is missing its type");
            SymbolShape shape;
            if (!parse_shape_type(type_name, shape.type)) return fail(error, line_number, "unsupported shape type '" + type_name + "'");
            bool fields_ok = true;
            auto fields = parse_fields(input, fields_ok);
            std::string point_data;
            const auto points_field = fields.find("points");
            if (!fields_ok || !take_string(fields, "id", shape.id) || !take_float(fields, "x", shape.x) ||
                !take_float(fields, "y", shape.y) || !take_float(fields, "width", shape.width) ||
                !take_float(fields, "height", shape.height) || !take_float(fields, "radius", shape.radius) ||
                !take_float(fields, "start", shape.start_angle) || !take_float(fields, "end", shape.end_angle) ||
                points_field == fields.end() || !take_string(fields, "fill", shape.fill) || !take_string(fields, "stroke", shape.stroke) ||
                !take_float(fields, "stroke_width", shape.stroke_width) || !take_bool(fields, "visible", shape.visible) ||
                !take_bool(fields, "locked", shape.locked)) return fail(error, line_number, "malformed shape record or missing field");
            point_data = points_field->second;
            if (!point_data.empty() && !parse_points(point_data, shape.points)) return fail(error, line_number, "invalid shape point list");
            static const std::set<std::string> allowed{"id","x","y","width","height","radius","start","end","points","fill","stroke","stroke_width","visible","locked"};
            for (const auto& [key, value] : fields) { (void)value; if (!allowed.contains(key)) return fail(error, line_number, "unknown shape field '" + key + "'"); }
            candidate.objects.emplace_back(std::move(shape));
            SymbolError current_error;
            if (!validate_symbol(candidate, current_error))
                return fail(error, line_number, current_error.message);
        } else if (keyword == "path") {
            SymbolPath path;
            bool fields_ok = true;
            auto fields = parse_fields(input, fields_ok);
            std::string points;
            if (!fields_ok || !take_string(fields, "id", path.id) || !take_string(fields, "points", points) ||
                !parse_points(points, path.points) || !take_bool(fields, "closed", path.closed) ||
                !take_string(fields, "fill", path.fill) || !take_string(fields, "stroke", path.stroke) ||
                !take_float(fields, "stroke_width", path.stroke_width) || !take_bool(fields, "visible", path.visible) ||
                !take_bool(fields, "locked", path.locked)) return fail(error, line_number, "malformed path record or missing field");
            static const std::set<std::string> allowed{"id","points","closed","fill","stroke","stroke_width","visible","locked"};
            for (const auto& [key, value] : fields) { (void)value; if (!allowed.contains(key)) return fail(error, line_number, "unknown path field '" + key + "'"); }
            candidate.objects.emplace_back(std::move(path));
            SymbolError current_error;
            if (!validate_symbol(candidate, current_error))
                return fail(error, line_number, current_error.message);
        } else if (keyword == "text") {
            SymbolText item;
            bool fields_ok = true;
            auto fields = parse_fields(input, fields_ok);
            std::string alignment, role;
            if (!fields_ok || !take_string(fields, "id", item.id) || !take_float(fields, "x", item.x) ||
                !take_float(fields, "y", item.y) || !take_float(fields, "size", item.size) ||
                !take_string(fields, "color", item.color) || !take_string(fields, "align", alignment) ||
                !take_string(fields, "role", role) || !take_bool(fields, "visible", item.visible) ||
                !take_bool(fields, "locked", item.locked) || !take_string(fields, "value", item.value))
                return fail(error, line_number, "malformed text object or missing field");
            if (alignment == "left") item.alignment = SymbolTextAlignment::Left;
            else if (alignment == "center") item.alignment = SymbolTextAlignment::Center;
            else if (alignment == "right") item.alignment = SymbolTextAlignment::Right;
            else return fail(error, line_number, "text align must be left, center or right");
            if (role == "ui") item.font_role = SymbolFontRole::Ui;
            else if (role == "code") item.font_role = SymbolFontRole::Code;
            else return fail(error, line_number, "text role must be ui or code");
            static const std::set<std::string> allowed{"id","x","y","size","color","align","role","visible","locked","value"};
            for (const auto& [key, value] : fields) { (void)value; if (!allowed.contains(key)) return fail(error, line_number, "unknown text field '" + key + "'"); }
            candidate.objects.emplace_back(std::move(item));
            SymbolError current_error;
            if (!validate_symbol(candidate, current_error))
                return fail(error, line_number, current_error.message);
        } else if (keyword == "port") {
            SymbolPortVisual port;
            std::string orientation;
            bool fields_ok = true;
            auto fields = parse_fields(input, fields_ok);
            if (!fields_ok || !take_string(fields, "id", port.port_id) || !take_float(fields, "x", port.x) ||
                !take_float(fields, "y", port.y) || !take_string(fields, "orientation", orientation) ||
                !parse_orientation(orientation, port.orientation) || !take_float(fields, "angle", port.angle_degrees) ||
                !take_string(fields, "label", port.label) || !take_bool(fields, "show_label", port.show_label) ||
                !take_bool(fields, "visible", port.visible)) return fail(error, line_number, "malformed port record or missing field");
            static const std::set<std::string> allowed{"id","x","y","orientation","angle","label","show_label","visible"};
            for (const auto& [key, value] : fields) { (void)value; if (!allowed.contains(key)) return fail(error, line_number, "unknown port field '" + key + "'"); }
            candidate.ports.push_back(std::move(port));
            SymbolError current_error;
            if (!validate_symbol(candidate, current_error))
                return fail(error, line_number, current_error.message);
        } else {
            return fail(error, line_number, "unknown symbol statement '" + keyword + "'");
        }
    }
    if (!header || !size_seen) return fail(error, 1, "symbol is missing a header or size declaration");
    if (!validate_symbol(candidate, error)) { if (error.line == 0) error.line = line_number; return false; }
    destination = std::move(candidate);
    error = {};
    return true;
}

Vec2 transform_symbol_point(Vec2 point, Vec2 size, const SymbolTransform& transform) noexcept {
    const Vec2 center{size.x * 0.5F, size.y * 0.5F};
    point = point - center;
    if (transform.flip_horizontal) point.x = -point.x;
    if (transform.flip_vertical) point.y = -point.y;
    switch (transform.rotation_quadrants % 4U) {
        case 1: point = {-point.y, point.x}; break;
        case 2: point = {-point.x, -point.y}; break;
        case 3: point = {point.y, -point.x}; break;
        default: break;
    }
    const auto output_size = transformed_symbol_size(size, transform);
    return point + Vec2{output_size.x * 0.5F, output_size.y * 0.5F};
}
Vec2 transformed_symbol_size(Vec2 size, const SymbolTransform& transform) noexcept {
    return transform.rotation_quadrants % 2U == 0 ? size : Vec2{size.y, size.x};
}
Vec2 inverse_transform_symbol_point(Vec2 point, Vec2 size, const SymbolTransform& transform) noexcept {
    const auto output = transformed_symbol_size(size, transform);
    point = point - Vec2{output.x * 0.5F, output.y * 0.5F};
    switch (transform.rotation_quadrants % 4U) {
        case 1: point = {point.y, -point.x}; break;
        case 2: point = {-point.x, -point.y}; break;
        case 3: point = {-point.y, point.x}; break;
        default: break;
    }
    if (transform.flip_vertical) point.y = -point.y;
    if (transform.flip_horizontal) point.x = -point.x;
    return point + Vec2{size.x * 0.5F, size.y * 0.5F};
}

std::optional<std::string> symbol_object_at(const SymbolDefinition& symbol, Vec2 point, float tolerance) {
    if (!finite(point.x) || !finite(point.y) || !finite(tolerance) || tolerance < 0.0F) return std::nullopt;
    for (auto it = symbol.objects.rbegin(); it != symbol.objects.rend(); ++it) {
        const bool hit = std::visit([&](const auto& item) {
            if (!item.visible) return false;
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, SymbolText>) {
                const auto bounds = text_bounds(item);
                return point.x >= bounds.min_x - tolerance && point.x <= bounds.max_x + tolerance &&
                       point.y >= bounds.min_y - tolerance && point.y <= bounds.max_y + tolerance;
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                const float limit = tolerance + item.stroke_width * 0.5F;
                for (std::size_t i = 1; i < item.points.size(); ++i)
                    if (point_segment_distance(point, {item.points[i-1].x,item.points[i-1].y},
                                               {item.points[i].x,item.points[i].y}) <= limit) return true;
                if (item.closed && item.points.size() > 2) {
                    if (point_segment_distance(point, {item.points.back().x,item.points.back().y},
                                               {item.points.front().x,item.points.front().y}) <= limit) return true;
                    return item.fill != "none" && point_in_polygon(point, item.points);
                }
                return false;
            } else {
                const float limit = tolerance + item.stroke_width * 0.5F;
                switch (item.type) {
                    case SymbolShapeType::Line:
                        if (item.points.size() >= 2) {
                            for (std::size_t i = 1; i < item.points.size(); ++i)
                                if (point_segment_distance(point, {item.points[i-1].x,item.points[i-1].y},
                                                           {item.points[i].x,item.points[i].y}) <= limit) return true;
                            return false;
                        }
                        return point_segment_distance(point, {item.x,item.y},
                            {item.x + item.width,item.y + item.height}) <= limit;
                    case SymbolShapeType::Polyline:
                    case SymbolShapeType::Path:
                    case SymbolShapeType::Polygon: {
                        for (std::size_t i = 1; i < item.points.size(); ++i)
                            if (point_segment_distance(point, {item.points[i-1].x,item.points[i-1].y},
                                                       {item.points[i].x,item.points[i].y}) <= limit) return true;
                        if (item.type == SymbolShapeType::Polygon && item.points.size() > 2) {
                            if (point_segment_distance(point, {item.points.back().x,item.points.back().y},
                                                       {item.points.front().x,item.points.front().y}) <= limit) return true;
                            return item.fill != "none" && point_in_polygon(point, item.points);
                        }
                        return false;
                    }
                    case SymbolShapeType::Ellipse:
                    case SymbolShapeType::Circle: {
                        if (item.width <= 0.0F || item.height <= 0.0F) return false;
                        const float cx = item.x + item.width * 0.5F;
                        const float cy = item.y + item.height * 0.5F;
                        const float diameter = std::min(item.width, item.height);
                        const float rx = (item.type == SymbolShapeType::Circle ? diameter : item.width) * 0.5F;
                        const float ry = (item.type == SymbolShapeType::Circle ? diameter : item.height) * 0.5F;
                        const float nx = (point.x - cx) / std::max(rx, 0.0001F);
                        const float ny = (point.y - cy) / std::max(ry, 0.0001F);
                        const float radial = std::sqrt(nx*nx + ny*ny);
                        const float tolerance_norm = limit / std::max(1.0F, std::min(rx, ry));
                        return (item.fill != "none" && radial <= 1.0F) || std::abs(radial - 1.0F) <= tolerance_norm;
                    }
                    case SymbolShapeType::Arc: {
                        const float cx = item.x + item.width * 0.5F;
                        const float cy = item.y + item.height * 0.5F;
                        const int count = std::clamp(static_cast<int>(std::ceil(std::abs(item.end_angle-item.start_angle)/4.0F)), 2, 360);
                        Vec2 previous{};
                        for (int i = 0; i <= count; ++i) {
                            const float t = static_cast<float>(i) / static_cast<float>(count);
                            const float angle = (item.start_angle + (item.end_angle-item.start_angle)*t) *
                                0.01745329251994329577F;
                            const Vec2 current{cx + std::cos(angle)*item.width*0.5F,
                                               cy + std::sin(angle)*item.height*0.5F};
                            if (i > 0 && point_segment_distance(point, previous, current) <= limit) return true;
                            previous = current;
                        }
                        return false;
                    }
                    case SymbolShapeType::Rectangle:
                    case SymbolShapeType::RoundedRectangle:
                        return point.x >= item.x - tolerance && point.x <= item.x + item.width + tolerance &&
                               point.y >= item.y - tolerance && point.y <= item.y + item.height + tolerance;
                }
                return false;
            }
        }, *it);
        if (hit) return std::visit([](const auto& item) { return item.id; }, *it);
    }
    return std::nullopt;
}
bool symbol_hit_test(const SymbolDefinition& symbol, Vec2 point, float tolerance) noexcept {
    return symbol_object_at(symbol, point, tolerance).has_value();
}
bool move_symbol_object(SymbolDefinition& symbol, std::string_view id, Vec2 delta) noexcept {
    if (!finite(delta.x) || !finite(delta.y)) return false;
    for (auto& object : symbol.objects) {
        const bool found = std::visit([&](auto& item) {
            if (item.id != id || item.locked) return false;
            using T = std::decay_t<decltype(item)>;
            float min_x=0.0F, min_y=0.0F, max_x=0.0F, max_y=0.0F;
            if constexpr (std::is_same_v<T, SymbolShape>) {
                if (!item.points.empty()) {
                    min_x=max_x=item.points.front().x; min_y=max_y=item.points.front().y;
                    for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
                } else { max_x=item.x+item.width; max_y=item.y+item.height; }
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                if(item.points.empty()) return false;
                min_x=max_x=item.points.front().x; min_y=max_y=item.points.front().y;
                for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
            } else {
                const auto bounds = text_bounds(item);
                min_x = bounds.min_x; min_y = bounds.min_y;
                max_x = bounds.max_x; max_y = bounds.max_y;
            }
            const float dx=std::clamp(delta.x,-min_x,symbol.size.x-max_x);
            const float dy=std::clamp(delta.y,-min_y,symbol.size.y-max_y);
            if constexpr (std::is_same_v<T, SymbolShape>) {
                item.x+=dx; item.y+=dy;
                for(auto& p:item.points){p.x+=dx;p.y+=dy;}
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                for(auto& p:item.points){p.x+=dx;p.y+=dy;}
            } else { item.x+=dx; item.y+=dy; }
            return true;
        }, object);
        if (found) return true;
    }
    return false;
}
std::optional<std::size_t> symbol_object_vertex_at(const SymbolDefinition& symbol, std::string_view id,
                                                          Vec2 point, float tolerance) noexcept {
    if (!finite(point.x) || !finite(point.y) || point.x < 0.0F || point.y < 0.0F ||
        point.x > symbol.size.x || point.y > symbol.size.y || !finite(tolerance) || tolerance < 0.0F) return std::nullopt;
    const auto found = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (found == symbol.objects.end()) return std::nullopt;
    const auto* points = editable_points(*found);
    if (!points || points->empty()) return std::nullopt;
    const float limit = tolerance * tolerance;
    float best = limit;
    std::optional<std::size_t> result;
    for (std::size_t i = 0; i < points->size(); ++i) {
        const float dx = point.x - (*points)[i].x;
        const float dy = point.y - (*points)[i].y;
        const float distance = dx*dx + dy*dy;
        if (distance <= best) { best = distance; result = i; }
    }
    return result;
}

bool move_symbol_object_vertex(SymbolDefinition& symbol, std::string_view id, std::size_t vertex,
                               Vec2 position) noexcept {
    if (!finite(position.x) || !finite(position.y) || position.x < 0.0F || position.y < 0.0F ||
        position.x > symbol.size.x || position.y > symbol.size.y) return false;
    const auto found = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (found == symbol.objects.end() || object_is_locked(*found)) return false;
    auto* points = editable_points(*found);
    if (!points || vertex >= points->size()) return false;
    (*points)[vertex] = {position.x, position.y};
    return true;
}

std::optional<std::size_t> insert_symbol_object_vertex(SymbolDefinition& symbol, std::string_view id,
                                                       Vec2 point, float tolerance) {
    if (!finite(point.x) || !finite(point.y) || point.x < 0.0F || point.y < 0.0F ||
        point.x > symbol.size.x || point.y > symbol.size.y || !finite(tolerance) || tolerance < 0.0F) return std::nullopt;
    const auto found = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (found == symbol.objects.end() || object_is_locked(*found)) return std::nullopt;
    auto* points = editable_points(*found);
    if (!points || points->size() < 2 || points->size() >= kMaximumPoints) return std::nullopt;
    const bool closed = object_is_closed(*found);
    const std::size_t segments = points->size() - 1 + (closed ? 1U : 0U);
    float best = tolerance;
    std::optional<std::size_t> after;
    for (std::size_t i = 0; i < segments; ++i) {
        const auto& a = (*points)[i];
        const auto& b = (*points)[(i + 1U) % points->size()];
        const float distance = point_segment_distance(point, {a.x,a.y}, {b.x,b.y});
        if (distance <= best) { best = distance; after = i; }
    }
    if (!after) return std::nullopt;
    const std::size_t index = *after + 1U;
    points->insert(points->begin() + static_cast<std::ptrdiff_t>(index), {point.x,point.y});
    return index;
}

bool remove_symbol_object_vertex(SymbolDefinition& symbol, std::string_view id, std::size_t vertex) noexcept {
    const auto found = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (found == symbol.objects.end() || object_is_locked(*found)) return false;
    auto* points = editable_points(*found);
    if (!points || vertex >= points->size() || points->size() <= minimum_vertices(*found)) return false;
    points->erase(points->begin() + static_cast<std::ptrdiff_t>(vertex));
    return true;
}

bool set_symbol_path_closed(SymbolDefinition& symbol, std::string_view id, bool closed) noexcept {
    for (auto& object : symbol.objects) {
        if (auto* path = std::get_if<SymbolPath>(&object); path && path->id == id && !path->locked) {
            if (closed && path->points.size() < 3) return false;
            path->closed = closed;
            return true;
        }
    }
    return false;
}

bool resize_symbol_object(SymbolDefinition& symbol, std::string_view id, Vec2 delta) noexcept {
    if (!finite(delta.x) || !finite(delta.y)) return false;
    for (auto& object : symbol.objects) {
        const bool found = std::visit([&](auto& item) {
            if (item.id != id || item.locked) return false;
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, SymbolShape>) {
                if (item.type == SymbolShapeType::Line && item.points.size() >= 2) {
                    float min_x=item.points.front().x,max_x=min_x,min_y=item.points.front().y,max_y=min_y;
                    for (const auto& p:item.points) { min_x=std::min(min_x,p.x); max_x=std::max(max_x,p.x); min_y=std::min(min_y,p.y); max_y=std::max(max_y,p.y); }
                    const float old_w=std::max(1.0F,max_x-min_x), old_h=std::max(1.0F,max_y-min_y);
                    const float new_w=std::clamp(old_w+delta.x,1.0F,std::max(1.0F,symbol.size.x-min_x));
                    const float new_h=std::clamp(old_h+delta.y,1.0F,std::max(1.0F,symbol.size.y-min_y));
                    for (auto& p:item.points) { p.x=min_x+(p.x-min_x)*(new_w/old_w); p.y=min_y+(p.y-min_y)*(new_h/old_h); }
                    return true;
                }
                if (item.type == SymbolShapeType::Line || item.type == SymbolShapeType::Rectangle ||
                    item.type == SymbolShapeType::RoundedRectangle || item.type == SymbolShapeType::Ellipse ||
                    item.type == SymbolShapeType::Circle || item.type == SymbolShapeType::Arc) {
                    const float available_width = symbol.size.x - item.x;
                    const float available_height = symbol.size.y - item.y;
                    if (available_width < 1.0F || available_height < 1.0F) return false;
                    if (item.type == SymbolShapeType::Circle) {
                        const float requested = std::max(item.width + delta.x, item.height + delta.y);
                        const float max_diameter = std::min(symbol.size.x, symbol.size.y);
                        const float diameter = std::clamp(requested, 1.0F, max_diameter);
                        const float center_x = item.x + item.width * 0.5F;
                        const float center_y = item.y + item.height * 0.5F;
                        item.width = item.height = diameter;
                        item.x = std::clamp(center_x - diameter * 0.5F, 0.0F, symbol.size.x - diameter);
                        item.y = std::clamp(center_y - diameter * 0.5F, 0.0F, symbol.size.y - diameter);
                    } else {
                        item.width = std::clamp(item.width + delta.x, 1.0F, available_width);
                        item.height = std::clamp(item.height + delta.y, 1.0F, available_height);
                    }
                    return true;
                }
                if (item.points.size() < 2) return false;
                float min_x=item.points.front().x,max_x=min_x,min_y=item.points.front().y,max_y=min_y;
                for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
                const float old_w=std::max(1.0F,max_x-min_x),old_h=std::max(1.0F,max_y-min_y);
                const float new_w=std::clamp(old_w+delta.x,1.0F,std::max(1.0F,symbol.size.x-min_x));
                const float new_h=std::clamp(old_h+delta.y,1.0F,std::max(1.0F,symbol.size.y-min_y));
                for(auto& p:item.points){p.x=min_x+(p.x-min_x)*(new_w/old_w);p.y=min_y+(p.y-min_y)*(new_h/old_h);}
                return true;
            } else if constexpr (std::is_same_v<T, SymbolPath>) {
                if(item.points.size()<2)return false;
                float min_x=item.points.front().x,max_x=min_x,min_y=item.points.front().y,max_y=min_y;
                for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
                const float old_w=std::max(1.0F,max_x-min_x),old_h=std::max(1.0F,max_y-min_y);
                const float new_w=std::clamp(old_w+delta.x,1.0F,std::max(1.0F,symbol.size.x-min_x));
                const float new_h=std::clamp(old_h+delta.y,1.0F,std::max(1.0F,symbol.size.y-min_y));
                for(auto& p:item.points){p.x=min_x+(p.x-min_x)*(new_w/old_w);p.y=min_y+(p.y-min_y)*(new_h/old_h);}
                return true;
            } else {
                const float glyph_width_factor = std::max(1.0F, static_cast<float>(item.value.size()) * 0.55F);
                float horizontal_room = symbol.size.x;
                if (item.alignment == SymbolTextAlignment::Left)
                    horizontal_room = (symbol.size.x - item.x) / glyph_width_factor;
                else if (item.alignment == SymbolTextAlignment::Center)
                    horizontal_room = 2.0F * std::min(item.x, symbol.size.x - item.x) / glyph_width_factor;
                else
                    horizontal_room = item.x / glyph_width_factor;
                const float vertical_room = (symbol.size.y - item.y) / 1.4F;
                const float maximum_size = std::min({128.0F, horizontal_room, vertical_room});
                if (maximum_size < 4.0F) return false;
                item.size = std::clamp(item.size + delta.y, 4.0F, maximum_size);
                return true;
            }
        }, object);
        if (found) return true;
    }
    return false;
}
bool reorder_symbol_object(SymbolDefinition& symbol, std::string_view id, int direction) noexcept {
    const auto it = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (it == symbol.objects.end()) return false;
    const auto index = static_cast<std::ptrdiff_t>(it - symbol.objects.begin());
    const auto target = std::clamp(index + (direction < 0 ? -1 : 1), std::ptrdiff_t{0},
                                   static_cast<std::ptrdiff_t>(symbol.objects.size() - 1));
    if (target == index) return false;
    std::iter_swap(symbol.objects.begin() + index, symbol.objects.begin() + target);
    return true;
}
bool align_symbol_object(SymbolDefinition& symbol, std::string_view id, SymbolAlignment alignment) noexcept {
    for (const auto& object : symbol.objects) {
        const bool matches = std::visit([&](const auto& item) { return item.id == id; }, object);
        if (!matches || object_is_locked(object)) continue;
        float min_x{}, min_y{}, max_x{}, max_y{};
        bool have_bounds = false;
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, SymbolText>) {
                const auto bounds = text_bounds(item);
                min_x = bounds.min_x; min_y = bounds.min_y;
                max_x = bounds.max_x; max_y = bounds.max_y;
                have_bounds = true;
            } else if constexpr (std::is_same_v<T, SymbolShape>) {
                if (!item.points.empty()) {
                    min_x = max_x = item.points.front().x; min_y = max_y = item.points.front().y;
                    for (const auto& point : item.points) {
                        min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
                        min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
                    }
                } else {
                    min_x = item.x; min_y = item.y; max_x = item.x + item.width; max_y = item.y + item.height;
                }
                have_bounds = true;
            } else if (!item.points.empty()) {
                min_x = max_x = item.points.front().x; min_y = max_y = item.points.front().y;
                for (const auto& point : item.points) {
                    min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
                    min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
                }
                have_bounds = true;
            }
        }, object);
        if (!have_bounds) return false;
        Vec2 delta{};
        switch (alignment) {
            case SymbolAlignment::Left: delta.x = -min_x; break;
            case SymbolAlignment::HorizontalCenter: delta.x = (symbol.size.x - (max_x - min_x)) * 0.5F - min_x; break;
            case SymbolAlignment::Right: delta.x = symbol.size.x - max_x; break;
            case SymbolAlignment::Top: delta.y = -min_y; break;
            case SymbolAlignment::VerticalCenter: delta.y = (symbol.size.y - (max_y - min_y)) * 0.5F - min_y; break;
            case SymbolAlignment::Bottom: delta.y = symbol.size.y - max_y; break;
        }
        return move_symbol_object(symbol, id, delta);
    }
    return false;
}

bool remove_symbol_object(SymbolDefinition& symbol, std::string_view id) noexcept {
    const auto it = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (it == symbol.objects.end()) return false;
    symbol.objects.erase(it);
    return true;
}

bool rotate_symbol_object(SymbolDefinition& symbol, std::string_view id, float degrees) noexcept {
    if (!finite(degrees) || std::abs(degrees) > 3600.0F) return false;
    for (auto& object : symbol.objects) {
        if (!std::visit([&](const auto& item) { return item.id == id; }, object) || object_is_locked(object)) continue;
        if (std::holds_alternative<SymbolText>(object)) return false;
        if (auto* shape = std::get_if<SymbolShape>(&object)) {
            if (shape->type == SymbolShapeType::Circle) return true;
            if (shape->type == SymbolShapeType::Arc) {
                shape->start_angle = std::remainder(shape->start_angle + degrees, 360.0F);
                shape->end_angle = std::remainder(shape->end_angle + degrees, 360.0F);
                return true;
            }
        }

        std::vector<SymbolPoint> points;
        bool closed = false;
        if (auto* shape = std::get_if<SymbolShape>(&object)) {
            closed = shape->type == SymbolShapeType::Polygon;
            switch (shape->type) {
                case SymbolShapeType::Rectangle:
                    points = {{shape->x,shape->y},{shape->x+shape->width,shape->y},
                              {shape->x+shape->width,shape->y+shape->height},{shape->x,shape->y+shape->height}};
                    closed = true; break;
                case SymbolShapeType::RoundedRectangle: {
                    // Approximate rounded corners with a small editable polygon.
                    const float radius = std::clamp(shape->radius, 0.0F, std::min(shape->width, shape->height) * 0.5F);
                    const float centers[4][2] = {{shape->x+shape->width-radius,shape->y+radius},
                        {shape->x+shape->width-radius,shape->y+shape->height-radius},
                        {shape->x+radius,shape->y+shape->height-radius},{shape->x+radius,shape->y+radius}};
                    const float starts[4] = {-90.0F,0.0F,90.0F,180.0F};
                    for (int corner=0; corner<4; ++corner) for (int step=0; step<=4; ++step) {
                        const float angle=(starts[corner]+22.5F*static_cast<float>(step))*0.01745329251994329577F;
                        points.push_back({centers[corner][0]+std::cos(angle)*radius,
                                          centers[corner][1]+std::sin(angle)*radius});
                    }
                    closed = true; break;
                }
                case SymbolShapeType::Ellipse: {
                    constexpr std::size_t segments=64;
                    const float cx=shape->x+shape->width*0.5F, cy=shape->y+shape->height*0.5F;
                    points.reserve(segments);
                    for (std::size_t i=0;i<segments;++i) {
                        const float angle=2.0F*3.14159265358979323846F*static_cast<float>(i)/static_cast<float>(segments);
                        points.push_back({cx+std::cos(angle)*shape->width*0.5F,cy+std::sin(angle)*shape->height*0.5F});
                    }
                    closed = true; break;
                }
                case SymbolShapeType::Line:
                    points = shape->points.size() >= 2 ? shape->points :
                        std::vector<SymbolPoint>{{shape->x,shape->y},{shape->x+shape->width,shape->y+shape->height}};
                    break;
                case SymbolShapeType::Polyline: case SymbolShapeType::Polygon: case SymbolShapeType::Path:
                    points = shape->points; break;
                case SymbolShapeType::Circle: case SymbolShapeType::Arc: return false;
            }
        } else if (auto* path = std::get_if<SymbolPath>(&object)) {
            points = path->points;
            closed = path->closed;
        }
        if (points.size() < 2) return false;
        float min_x=points.front().x,max_x=min_x,min_y=points.front().y,max_y=min_y;
        for (const auto& point:points) {
            min_x=std::min(min_x,point.x); max_x=std::max(max_x,point.x);
            min_y=std::min(min_y,point.y); max_y=std::max(max_y,point.y);
        }
        const float cx=(min_x+max_x)*0.5F, cy=(min_y+max_y)*0.5F;
        const float radians=degrees*0.01745329251994329577F;
        const float c=std::cos(radians), sn=std::sin(radians);
        for (auto& point:points) {
            const float x=point.x-cx, y=point.y-cy;
            point.x=cx+x*c-y*sn;
            point.y=cy+x*sn+y*c;
        }
        min_x=max_x=points.front().x; min_y=max_y=points.front().y;
        for (const auto& point:points) {
            min_x=std::min(min_x,point.x); max_x=std::max(max_x,point.x);
            min_y=std::min(min_y,point.y); max_y=std::max(max_y,point.y);
        }
        const float width=max_x-min_x, height=max_y-min_y;
        if (width > symbol.size.x || height > symbol.size.y) return false;
        const float dx=std::clamp(0.0F,-min_x,symbol.size.x-max_x);
        const float dy=std::clamp(0.0F,-min_y,symbol.size.y-max_y);
        for (auto& point:points) { point.x+=dx; point.y+=dy; }
        if (auto* shape=std::get_if<SymbolShape>(&object)) {
            shape->type = closed ? SymbolShapeType::Polygon : SymbolShapeType::Polyline;
            shape->points = std::move(points);
            shape->x=shape->y=shape->width=shape->height=0.0F;
        } else if (auto* path=std::get_if<SymbolPath>(&object)) {
            path->points=std::move(points);
        }
        return true;
    }
    return false;
}

bool duplicate_symbol_object(SymbolDefinition& symbol, std::string_view id, std::string new_id) {
    if (!valid_id(new_id) || symbol.objects.size() >= kMaximumObjects) return false;
    if (std::any_of(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == new_id; }, object);
    })) return false;
    const auto found = std::find_if(symbol.objects.begin(), symbol.objects.end(), [&](const SymbolObject& object) {
        return std::visit([&](const auto& item) { return item.id == id; }, object);
    });
    if (found == symbol.objects.end()) return false;
    SymbolObject copy = *found;
    float min_x=0.0F,min_y=0.0F,max_x=0.0F,max_y=0.0F;
    std::visit([&](auto& item) {
        using T=std::decay_t<decltype(item)>;
        item.id = new_id;
        item.locked = false;
        if constexpr (std::is_same_v<T,SymbolShape>) {
            if(!item.points.empty()) {
                min_x=max_x=item.points.front().x; min_y=max_y=item.points.front().y;
                for(const auto& point:item.points){min_x=std::min(min_x,point.x);max_x=std::max(max_x,point.x);min_y=std::min(min_y,point.y);max_y=std::max(max_y,point.y);}
            } else {min_x=item.x; min_y=item.y; max_x=item.x+item.width; max_y=item.y+item.height;}
        } else if constexpr (std::is_same_v<T,SymbolPath>) {
            min_x=max_x=item.points.front().x; min_y=max_y=item.points.front().y;
            for(const auto& point:item.points){min_x=std::min(min_x,point.x);max_x=std::max(max_x,point.x);min_y=std::min(min_y,point.y);max_y=std::max(max_y,point.y);}
        } else {
            const auto bounds = text_bounds(item);
            min_x = bounds.min_x; min_y = bounds.min_y;
            max_x = bounds.max_x; max_y = bounds.max_y;
        }
    },copy);
    const float width=max_x-min_x,height=max_y-min_y;
    const float dx=std::clamp(8.0F,-min_x,symbol.size.x-width-min_x);
    const float dy=std::clamp(8.0F,-min_y,symbol.size.y-height-min_y);
    std::visit([&](auto& item) {
        using T=std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T,SymbolShape>) {
            item.x+=dx; item.y+=dy;
            for(auto& point:item.points){point.x+=dx;point.y+=dy;}
        } else if constexpr (std::is_same_v<T,SymbolPath>) {
            for(auto& point:item.points){point.x+=dx;point.y+=dy;}
        } else { item.x+=dx; item.y+=dy; }
    },copy);
    const auto index = static_cast<std::size_t>(found - symbol.objects.begin());
    symbol.objects.insert(symbol.objects.begin() + static_cast<std::ptrdiff_t>(index + 1), std::move(copy));
    return true;
}

std::optional<std::string> symbol_port_at(const SymbolDefinition& symbol, Vec2 point, float tolerance) {
    const float tolerance_squared = tolerance * tolerance;
    float best = tolerance_squared;
    std::optional<std::string> result;
    for (const auto& port : symbol.ports) {
        if (!port.visible) continue;
        const float dx = point.x - port.x, dy = point.y - port.y;
        const float distance = dx*dx + dy*dy;
        if (distance <= best) { best = distance; result = port.port_id; }
    }
    return result;
}

bool move_symbol_port(SymbolDefinition& symbol, std::string_view port_id, Vec2 position,
                      SymbolPortOrientation orientation, float angle_degrees) noexcept {
    if (!finite(position.x) || !finite(position.y) || !finite(angle_degrees) ||
        !valid_orientation(orientation) || std::abs(angle_degrees) > 3600.0F) return false;
    if (position.x < 0.0F || position.y < 0.0F || position.x > symbol.size.x || position.y > symbol.size.y) return false;
    for (auto& port : symbol.ports) {
        if (port.port_id != port_id) continue;
        port.x = position.x; port.y = position.y;
        port.orientation = orientation; port.angle_degrees = angle_degrees;
        return true;
    }
    return false;
}

bool set_symbol_port_label(SymbolDefinition& symbol, std::string_view port_id,
                           std::string label, bool visible) {
    if (label.size() > 256 || !std::all_of(label.begin(), label.end(), [](unsigned char c) { return c >= 0x20U; })) return false;
    for (auto& port : symbol.ports) {
        if (port.port_id != port_id) continue;
        port.label = std::move(label); port.show_label = visible;
        return true;
    }
    return false;
}

bool set_symbol_port_orientation(SymbolDefinition& symbol, std::string_view port_id,
                                 SymbolPortOrientation orientation, float angle_degrees) noexcept {
    if (!finite(angle_degrees) || std::abs(angle_degrees) > 3600.0F || !valid_orientation(orientation)) return false;
    for (auto& port : symbol.ports) {
        if (port.port_id != port_id) continue;
        port.orientation = orientation;
        port.angle_degrees = angle_degrees;
        return true;
    }
    return false;
}

} // namespace hcs::editor
