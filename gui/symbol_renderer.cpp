#include "symbol_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>
#include <numeric>

namespace hcs::gui {
namespace {
constexpr float kPi = 3.14159265358979323846F;

SDL_Color parse_color(const std::string& text, SDL_Color fallback) {
    if (text == "none" || (text.size() != 7 && text.size() != 9) || text.empty() || text.front() != '#') return fallback;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int values[4]{0, 0, 0, 255};
    const std::size_t count = text.size() == 9 ? 4 : 3;
    for (std::size_t i = 0; i < count; ++i) {
        const int hi = hex(text[1 + i * 2]);
        const int lo = hex(text[2 + i * 2]);
        if (hi < 0 || lo < 0) return fallback;
        values[i] = hi * 16 + lo;
    }
    return {static_cast<Uint8>(values[0]), static_cast<Uint8>(values[1]),
            static_cast<Uint8>(values[2]), static_cast<Uint8>(values[3])};
}

editor::Vec2 project(editor::Vec2 point, editor::Vec2 size,
                     const editor::SymbolTransform& transform,
                     editor::Vec2 origin, float zoom) {
    const auto transformed = editor::transform_symbol_point(point, size, transform);
    return {origin.x + transformed.x * zoom, origin.y + transformed.y * zoom};
}

void set_color(SDL_Renderer* renderer, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

SDL_Vertex make_vertex(editor::Vec2 point, SDL_Color color) {
    return {{point.x, point.y},
            {static_cast<float>(color.r) / 255.0F,
             static_cast<float>(color.g) / 255.0F,
             static_cast<float>(color.b) / 255.0F,
             static_cast<float>(color.a) / 255.0F},
            {0.0F, 0.0F}};
}

float cross(editor::Vec2 a, editor::Vec2 b, editor::Vec2 c) {
    return (b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
}

bool point_in_triangle(editor::Vec2 p, editor::Vec2 a, editor::Vec2 b, editor::Vec2 c,
                       float orientation) {
    constexpr float epsilon = 0.0001F;
    return cross(a,b,p)*orientation >= -epsilon &&
           cross(b,c,p)*orientation >= -epsilon &&
           cross(c,a,p)*orientation >= -epsilon;
}

// Ear clipping supports both convex and concave simple contours. Self-intersecting
// contours are rejected by the symbol validator's caller rather than guessed here.
bool triangulate_contour(const std::vector<editor::Vec2>& points, std::vector<int>& indices) {
    indices.clear();
    if (points.size() < 3 || points.size() > 1024) return false;
    float area = 0.0F;
    for (std::size_t i=0; i<points.size(); ++i) {
        const auto& a=points[i]; const auto& b=points[(i+1)%points.size()];
        area += a.x*b.y-b.x*a.y;
    }
    if (std::abs(area) < 0.0001F) return false;
    const float orientation = area > 0.0F ? 1.0F : -1.0F;
    std::vector<int> remaining(points.size());
    std::iota(remaining.begin(),remaining.end(),0);
    std::size_t guard = points.size()*points.size();
    while (remaining.size()>3 && guard-- > 0) {
        bool clipped=false;
        for (std::size_t i=0;i<remaining.size();++i) {
            const int previous=remaining[(i+remaining.size()-1)%remaining.size()];
            const int current=remaining[i];
            const int next=remaining[(i+1)%remaining.size()];
            if (cross(points[static_cast<std::size_t>(previous)],points[static_cast<std::size_t>(current)],
                      points[static_cast<std::size_t>(next)])*orientation <= 0.0001F) continue;
            bool contains=false;
            for (const int candidate:remaining) {
                if (candidate==previous || candidate==current || candidate==next) continue;
                if (point_in_triangle(points[static_cast<std::size_t>(candidate)],
                                      points[static_cast<std::size_t>(previous)],
                                      points[static_cast<std::size_t>(current)],
                                      points[static_cast<std::size_t>(next)],orientation)) {
                    contains=true; break;
                }
            }
            if (contains) continue;
            indices.insert(indices.end(),{previous,current,next});
            remaining.erase(remaining.begin()+static_cast<std::ptrdiff_t>(i));
            clipped=true; break;
        }
        if (!clipped) { indices.clear(); return false; }
    }
    if (remaining.size()==3)
        indices.insert(indices.end(),{remaining[0],remaining[1],remaining[2]});
    return !indices.empty();
}

void fill_contour(SDL_Renderer* renderer, const std::vector<editor::SymbolPoint>& points,
                  editor::Vec2 size, const editor::SymbolTransform& transform,
                  editor::Vec2 origin, float zoom, SDL_Color color) {
    if (points.size()<3) return;
    std::vector<editor::Vec2> projected; projected.reserve(points.size());
    for (const auto& point:points)
        projected.push_back(project({point.x,point.y},size,transform,origin,zoom));
    std::vector<int> indices;
    if (!triangulate_contour(projected,indices)) return;
    std::vector<SDL_Vertex> vertices; vertices.reserve(projected.size());
    for (const auto& point:projected) vertices.push_back(make_vertex(point,color));
    (void)SDL_RenderGeometry(renderer,nullptr,vertices.data(),static_cast<int>(vertices.size()),
                             indices.data(),static_cast<int>(indices.size()));
}

void draw_segment(SDL_Renderer* renderer, editor::Vec2 a, editor::Vec2 b,
                  float thickness, SDL_Color color) {
    const float dx=b.x-a.x, dy=b.y-a.y;
    const float length=std::sqrt(dx*dx+dy*dy);
    if (length<0.001F) return;
    const float half=std::max(0.5F,thickness*0.5F);
    const float nx=-dy/length*half, ny=dx/length*half;
    const std::vector<editor::Vec2> quad{{a.x+nx,a.y+ny},{b.x+nx,b.y+ny},
                                        {b.x-nx,b.y-ny},{a.x-nx,a.y-ny}};
    std::vector<SDL_Vertex> vertices; vertices.reserve(4);
    for(const auto& point:quad) vertices.push_back(make_vertex(point,color));
    constexpr int indices[]{0,1,2,0,2,3};
    (void)SDL_RenderGeometry(renderer,nullptr,vertices.data(),4,indices,6);
}

void draw_polyline(SDL_Renderer* renderer, const std::vector<editor::SymbolPoint>& points,
                   bool closed, editor::Vec2 size, const editor::SymbolTransform& transform,
                   editor::Vec2 origin, float zoom, SDL_Color color, float stroke_width=1.0F) {
    if (points.size()<2) return;
    const float thickness=std::clamp(stroke_width*zoom,0.75F,64.0F);
    for(std::size_t i=1;i<points.size();++i) {
        const auto a=project({points[i-1].x,points[i-1].y},size,transform,origin,zoom);
        const auto b=project({points[i].x,points[i].y},size,transform,origin,zoom);
        draw_segment(renderer,a,b,thickness,color);
    }
    if(closed) {
        const auto a=project({points.back().x,points.back().y},size,transform,origin,zoom);
        const auto b=project({points.front().x,points.front().y},size,transform,origin,zoom);
        draw_segment(renderer,a,b,thickness,color);
    }
}

std::vector<editor::SymbolPoint> rounded_rectangle(float x, float y, float w, float h, float radius) {
    std::vector<editor::SymbolPoint> points;
    radius = std::clamp(radius, 0.0F, std::min(w, h) * 0.5F);
    if (radius <= 0.5F) return {{x,y},{x+w,y},{x+w,y+h},{x,y+h}};
    constexpr int segments = 7;
    const std::pair<float,float> centers[] = {{x+w-radius,y+radius},{x+w-radius,y+h-radius},
                                               {x+radius,y+h-radius},{x+radius,y+radius}};
    const float starts[] = {-90.0F, 0.0F, 90.0F, 180.0F};
    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= segments; ++step) {
            const float angle = (starts[corner] + 90.0F * static_cast<float>(step) / static_cast<float>(segments)) * kPi / 180.0F;
            points.push_back({centers[corner].first + std::cos(angle)*radius,
                              centers[corner].second + std::sin(angle)*radius});
        }
    }
    return points;
}

void render_shape(SDL_Renderer* renderer, const editor::SymbolShape& shape,
                  editor::Vec2 size, editor::Vec2 origin, float zoom,
                  const editor::SymbolTransform& transform) {
    if (!shape.visible) return;
    const auto stroke=parse_color(shape.stroke,SDL_Color{208,215,226,255});
    const auto fill=parse_color(shape.fill,SDL_Color{32,38,48,255});
    const bool has_stroke=shape.stroke!="none" && shape.stroke_width>0.0F;
    const bool has_fill=shape.fill!="none";
    std::vector<editor::SymbolPoint> points;
    bool closed=false;
    switch(shape.type) {
        case editor::SymbolShapeType::Rectangle:
            points={{shape.x,shape.y},{shape.x+shape.width,shape.y},
                    {shape.x+shape.width,shape.y+shape.height},{shape.x,shape.y+shape.height}};
            closed=true; break;
        case editor::SymbolShapeType::RoundedRectangle:
            points=rounded_rectangle(shape.x,shape.y,shape.width,shape.height,shape.radius);
            closed=true; break;
        case editor::SymbolShapeType::Circle:
        case editor::SymbolShapeType::Ellipse: {
            constexpr int segments=64;
            const float rx=(shape.type==editor::SymbolShapeType::Circle?std::min(shape.width,shape.height):shape.width)*0.5F;
            const float ry=(shape.type==editor::SymbolShapeType::Circle?std::min(shape.width,shape.height):shape.height)*0.5F;
            const float cx=shape.x+shape.width*0.5F, cy=shape.y+shape.height*0.5F;
            points.reserve(segments);
            for(int i=0;i<segments;++i) {
                const float angle=2.0F*kPi*static_cast<float>(i)/static_cast<float>(segments);
                points.push_back({cx+std::cos(angle)*rx,cy+std::sin(angle)*ry});
            }
            closed=true; break;
        }
        case editor::SymbolShapeType::Line: {
            if (shape.points.size() >= 2) {
                if (has_stroke) draw_polyline(renderer, shape.points, false, size, transform,
                                               origin, zoom, stroke, shape.stroke_width);
            } else {
                const auto a=project({shape.x,shape.y},size,transform,origin,zoom);
                const auto b=project({shape.x+shape.width,shape.y+shape.height},size,transform,origin,zoom);
                if(has_stroke) draw_segment(renderer,a,b,std::max(0.75F,shape.stroke_width*zoom),stroke);
            }
            return;
        }
        case editor::SymbolShapeType::Arc: {
            const float cx=shape.x+shape.width*0.5F, cy=shape.y+shape.height*0.5F;
            const float rx=shape.width*0.5F, ry=shape.height*0.5F;
            const int count=std::clamp(static_cast<int>(std::ceil(std::abs(shape.end_angle-shape.start_angle)/5.0F)),2,144);
            for(int i=0;i<=count;++i) {
                const float t=static_cast<float>(i)/static_cast<float>(count);
                const float angle=(shape.start_angle+(shape.end_angle-shape.start_angle)*t)*kPi/180.0F;
                points.push_back({cx+std::cos(angle)*rx,cy+std::sin(angle)*ry});
            }
            if(has_stroke) draw_polyline(renderer,points,false,size,transform,origin,zoom,stroke,shape.stroke_width);
            return;
        }
        case editor::SymbolShapeType::Polyline:
        case editor::SymbolShapeType::Path:
            points=shape.points; break;
        case editor::SymbolShapeType::Polygon:
            points=shape.points; closed=true; break;
    }
    if(closed && has_fill) fill_contour(renderer,points,size,transform,origin,zoom,fill);
    if(has_stroke) draw_polyline(renderer,points,closed,size,transform,origin,zoom,stroke,shape.stroke_width);
}

editor::Vec2 orientation_vector(const editor::SymbolPortVisual& port) {
    switch (port.orientation) {
        case editor::SymbolPortOrientation::Left: return {-1.0F,0.0F};
        case editor::SymbolPortOrientation::Right: return {1.0F,0.0F};
        case editor::SymbolPortOrientation::Top: return {0.0F,-1.0F};
        case editor::SymbolPortOrientation::Bottom: return {0.0F,1.0F};
        case editor::SymbolPortOrientation::Angle: {
            const float angle = port.angle_degrees*kPi/180.0F;
            return {std::cos(angle),std::sin(angle)};
        }
    }
    return {-1.0F,0.0F};
}

void draw_dot(SDL_Renderer* renderer, editor::Vec2 point, float zoom, SDL_Color color) {
    const float r = std::clamp(3.5F*zoom,2.0F,5.0F);
    SDL_FRect rect{point.x-r,point.y-r,r*2.0F,r*2.0F};
    set_color(renderer,color); (void)SDL_RenderFillRect(renderer,&rect);
}
} // namespace

void SymbolRenderer::draw(SDL_Renderer* renderer,
                          const editor::CustomComponentDefinition& definition,
                          editor::Vec2 screen_origin, float zoom,
                          const editor::SymbolTransform& transform,
                          bool show_port_labels) {
    const auto& symbol = definition.symbol;
    if (symbol.objects.empty()) {
        SDL_FRect body{screen_origin.x,screen_origin.y,definition.size.x*zoom,definition.size.y*zoom};
        set_color(renderer,SDL_Color{44,56,78,255}); (void)SDL_RenderFillRect(renderer,&body);
        set_color(renderer,SDL_Color{126,154,202,255}); (void)SDL_RenderRect(renderer,&body);
    }
    for (const auto& object : symbol.objects) {
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T,editor::SymbolShape>) {
                render_shape(renderer,item,symbol.size,screen_origin,zoom,transform);
            } else if constexpr (std::is_same_v<T,editor::SymbolPath>) {
                if (!item.visible) return;
                if (item.closed && item.fill != "none")
                    fill_contour(renderer,item.points,symbol.size,transform,screen_origin,zoom,
                                 parse_color(item.fill,SDL_Color{32,38,48,255}));
                if (item.stroke != "none" && item.stroke_width > 0.0F)
                    draw_polyline(renderer,item.points,item.closed,symbol.size,transform,screen_origin,zoom,
                                  parse_color(item.stroke,SDL_Color{208,215,226,255}),item.stroke_width);
            } else {
                if (!item.visible) return;
                auto pos = project({item.x,item.y},symbol.size,transform,screen_origin,zoom);
                const float scale = std::clamp(zoom*item.size/12.0F,0.25F,8.0F);
                const auto role = item.font_role == editor::SymbolFontRole::Code ? FontRole::Code : FontRole::Ui;
                const auto extent = FontManager::measure(item.value.c_str(),scale,role);
                if (item.alignment == editor::SymbolTextAlignment::Center) pos.x -= extent.x*0.5F;
                else if (item.alignment == editor::SymbolTextAlignment::Right) pos.x -= extent.x;
                FontManager::draw(renderer,pos.x,pos.y,item.value.c_str(),scale,
                                  parse_color(item.color,SDL_Color{208,215,226,255}),role);
            }
        },object);
    }
    const bool has_title = std::any_of(symbol.objects.begin(), symbol.objects.end(), [](const editor::SymbolObject& object) {
        const auto* text = std::get_if<editor::SymbolText>(&object);
        return text && text->visible && !text->value.empty();
    });
    if (!has_title && zoom >= 0.45F) {
        const float scale = std::clamp(zoom * 0.8F, 0.45F, 1.0F);
        const auto extent = FontManager::measure(definition.name.c_str(), scale, FontRole::Ui);
        FontManager::draw(renderer, screen_origin.x + (editor::transformed_symbol_size(symbol.size, transform).x * zoom - extent.x) * 0.5F,
                          screen_origin.y + (editor::transformed_symbol_size(symbol.size, transform).y * zoom - extent.y) * 0.5F,
                          definition.name.c_str(), scale, SDL_Color{222,231,241,255}, FontRole::Ui);
    }
    for (const auto& port : symbol.ports) {
        if (!port.visible) continue;
        const auto endpoint = project({port.x,port.y},symbol.size,transform,screen_origin,zoom);
        const auto vector = orientation_vector(port);
        const auto next = project({port.x+vector.x*12.0F,port.y+vector.y*12.0F},symbol.size,transform,screen_origin,zoom);
        const editor::Vec2 direction{next.x-endpoint.x,next.y-endpoint.y};
        const auto logical = std::find_if(definition.ports.begin(),definition.ports.end(),[&](const auto& candidate) {
            return candidate.id == port.port_id;
        });
        const bool is_input = logical != definition.ports.end() && logical->direction == editor::PinDirection::Input;
        const SDL_Color port_color = is_input ? SDL_Color{104,190,238,255} : SDL_Color{255,177,96,255};
        set_color(renderer,port_color);
        (void)SDL_RenderLine(renderer,endpoint.x,endpoint.y,next.x,next.y);
        draw_dot(renderer,endpoint,zoom,port_color);
        // A small outward chevron makes the visual orientation apparent while the
        // port's logical direction remains independently defined by the interface.
        const float length=std::hypot(direction.x,direction.y);
        if(length>0.001F) {
            const float ux=direction.x/length, uy=direction.y/length;
            const float head=std::clamp(5.0F*zoom,2.5F,6.0F);
            const float wing=std::clamp(3.0F*zoom,1.5F,4.0F);
            const editor::Vec2 tip{next.x,next.y};
            const editor::Vec2 back{tip.x-ux*head,tip.y-uy*head};
            const editor::Vec2 side{-uy*wing,ux*wing};
            (void)SDL_RenderLine(renderer,tip.x,tip.y,back.x+side.x,back.y+side.y);
            (void)SDL_RenderLine(renderer,tip.x,tip.y,back.x-side.x,back.y-side.y);
        }
        if (show_port_labels && port.show_label && !port.label.empty()) {
            const float scale = std::clamp(zoom*0.72F,0.50F,1.05F);
            const auto size = FontManager::measure(port.label.c_str(),scale,FontRole::Ui);
            float x = endpoint.x + direction.x*6.0F;
            float y = endpoint.y + direction.y*6.0F - size.y*0.5F;
            if (std::abs(direction.x) > std::abs(direction.y)) {
                if (direction.x < 0.0F) x -= size.x;
            } else {
                x -= size.x*0.5F;
                if (direction.y < 0.0F) y -= size.y*0.5F;
                else y += size.y*0.5F;
            }
            FontManager::draw(renderer,x,y,port.label.c_str(),scale,port_color,FontRole::Ui);
        }
    }
}

bool SymbolRenderer::hit_test(const editor::CustomComponentDefinition& definition,
                              editor::Vec2 world_position, editor::Vec2 point,
                              const editor::SymbolTransform& transform, float tolerance) {
    const auto local = editor::inverse_transform_symbol_point(
        point - world_position, definition.symbol.size, transform);
    if (editor::symbol_port_at(definition.symbol, local, tolerance)) return true;
    return editor::symbol_hit_test(definition.symbol, local, tolerance);
}

} // namespace hcs::gui
