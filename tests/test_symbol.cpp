#include "hcs/editor/custom_components.hpp"
#include "hcs/editor/symbol.hpp"
#include "hcs/editor/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>

using namespace hcs::editor;

namespace {
void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

void test_symbol_roundtrip_and_determinism() {
    SymbolDefinition symbol;
    symbol.size = {180.0F, 130.0F};
    SymbolShape rect;
    rect.id = "body"; rect.type = SymbolShapeType::RoundedRectangle;
    rect.x = 10; rect.y = 10; rect.width = 160; rect.height = 110; rect.radius = 14;
    symbol.objects.emplace_back(rect);
    SymbolShape poly;
    poly.id = "poly"; poly.type = SymbolShapeType::Polygon;
    poly.points = {{90,20},{160,65},{90,110},{20,65}};
    poly.fill = "#303F54"; poly.stroke = "#AABBCC";
    symbol.objects.emplace_back(poly);
    SymbolPath path;
    path.id = "contour"; path.points = {{12,65},{35,45},{58,65},{35,85}};
    path.closed = true; path.fill = "none";
    symbol.objects.emplace_back(path);
    SymbolText title;
    title.id = "title"; title.value = "Half Adder";
    title.x = 90; title.y = 60; title.alignment = SymbolTextAlignment::Center;
    symbol.objects.emplace_back(title);
    symbol.ports = {
        {"a", 0, 40, SymbolPortOrientation::Left, 0, "A", true, true},
        {"b", 80, 0, SymbolPortOrientation::Top, 0, "B", true, true},
        {"sum", 180, 80, SymbolPortOrientation::Right, 0, "SUM", true, true},
        {"carry", 85, 130, SymbolPortOrientation::Bottom, 0, "CARRY", true, true},
        {"sense", 8, 8, SymbolPortOrientation::Angle, 45, "SENSE", false, true},
    };
    SymbolError error;
    std::string text;
    require(serialize_symbol(symbol, text, error), "symbol serialization");
    SymbolDefinition parsed;
    require(parse_symbol(text, parsed, error), "symbol parsing");
    std::string second;
    require(serialize_symbol(parsed, second, error), "symbol second serialization");
    require(text == second, "symbol serialization should be deterministic");
    require(parsed.objects.size() == 4 && parsed.ports.size() == 5, "symbol objects and ports survive round trip");
    const auto* parsed_text = std::get_if<SymbolText>(&parsed.objects.back());
    require(parsed_text && parsed_text->value == "Half Adder", "quoted text with spaces should survive roundtrip");
}

void test_symbol_edits_and_hit_testing() {
    SymbolDefinition symbol = make_symbol_preset("triangle", {160,120}, {{"in", "input"}, {"out", "output"}});
    require(symbol.objects.size() == 1 && symbol.ports.size() == 2, "preset creates editable objects and ports");
    require(symbol_hit_test(symbol, {80,10}, 2.0F), "triangle contour is hit-testable");
    require(move_symbol_port(symbol, "in", {80,0}, SymbolPortOrientation::Top), "port can be moved to top side");
    require(set_symbol_port_label(symbol, "in", "DATA", true), "port label can be edited");
    require(symbol.ports[0].port_id == "in" && symbol.ports[0].x == 80.0F && symbol.ports[0].label == "DATA",
            "moving and renaming visual port preserves stable port ID");
    require(move_symbol_object(symbol, "body", {4,5}), "symbol object can be moved");
    require(resize_symbol_object(symbol, "body", {10,7}), "shape can be resized");
    require(reorder_symbol_object(symbol, "body", 1) == false, "single object order remains stable");
    require(remove_symbol_object(symbol, "body"), "symbol object can be deleted");
}

void test_transform_roundtrip_and_hit_testing() {
    const Vec2 source_size{160,80};
    const SymbolTransform transform{1, true, false};
    const Vec2 source{20,30};
    const auto output = transform_symbol_point(source, source_size, transform);
    const auto restored = inverse_transform_symbol_point(output, source_size, transform);
    require(std::abs(restored.x-source.x) < 0.001F && std::abs(restored.y-source.y) < 0.001F,
            "symbol point transform must roundtrip");
    const auto output_size = transformed_symbol_size(source_size, transform);
    require(output_size.x == 80 && output_size.y == 160, "quarter rotation swaps the rendered bounds");
}

void test_validation_rejects_malformed_symbols() {
    SymbolDefinition destination = make_default_symbol({140,80});
    const auto previous = destination;
    SymbolError error;
    require(!parse_symbol("HCSS 9\nsize width=140 height=80\n", destination, error), "unsupported symbol version rejected");
    require(destination.size.x == previous.size.x, "invalid text must not replace the previous symbol");
    require(!parse_symbol("HCSS 1\nsize width=140 height=80\nshape rect id=x x=0 y=0 width=10 height=10 radius=2 start=0 end=360 points=\"\" fill=red stroke=#FFFFFF stroke_width=2 visible=1 locked=0\n", destination, error),
            "invalid color must be rejected");
    SymbolDefinition duplicate;
    duplicate.size = {80,80};
    SymbolShape a; a.id = "same"; a.x=1; a.y=1; a.width=20; a.height=20;
    SymbolShape b=a;
    duplicate.objects = {a,b};
    require(!validate_symbol(duplicate,error), "duplicate stable object IDs rejected");
}


void test_vertex_editing_and_path_closure() {
    SymbolDefinition symbol;
    symbol.size = {160, 120};
    SymbolPath path;
    path.id = "editable_path";
    path.points = {{20,20},{80,20},{120,80}};
    symbol.objects.emplace_back(path);

    const auto vertex = symbol_object_vertex_at(symbol, "editable_path", {79,21}, 4.0F);
    require(vertex && *vertex == 1, "vertex hit testing selects nearest path vertex");
    require(move_symbol_object_vertex(symbol, "editable_path", *vertex, {84,26}),
            "path vertex can be moved");
    require(symbol.objects.size() == 1 && std::get<SymbolPath>(symbol.objects.front()).points[1].x == 84,
            "moving a vertex updates the canonical path model");

    const auto inserted = insert_symbol_object_vertex(symbol, "editable_path", {50,20}, 4.0F);
    require(inserted && *inserted == 1, "vertex can be inserted on the closest segment");
    require(std::get<SymbolPath>(symbol.objects.front()).points.size() == 4,
            "insertion increases the path point count");
    require(remove_symbol_object_vertex(symbol, "editable_path", *inserted),
            "inserted vertex can be removed");
    require(std::get<SymbolPath>(symbol.objects.front()).points.size() == 3,
            "removing a vertex restores the original point count");

    require(set_symbol_path_closed(symbol, "editable_path", true), "open path can be closed");
    require(!remove_symbol_object_vertex(symbol, "editable_path", 0),
            "closed path retains at least three vertices");
    require(set_symbol_path_closed(symbol, "editable_path", false), "closed path can be reopened");
    SymbolError error;
    require(validate_symbol(symbol, error), "edited path remains serializable");
}

void test_shape_hit_testing_and_bounds() {
    SymbolDefinition symbol;
    symbol.size = {160, 120};
    SymbolShape circle;
    circle.id = "circle";
    circle.type = SymbolShapeType::Circle;
    circle.x = 30; circle.y = 20; circle.width = 80; circle.height = 80;
    circle.fill = "#223344";
    symbol.objects.emplace_back(circle);
    require(symbol_hit_test(symbol, {70,60}, 1.0F), "filled circle body is hit-testable");
    require(!symbol_hit_test(symbol, {31,21}, 1.0F), "empty corner of circle bounding box is not hit-testable");
    SymbolError error;
    require(validate_symbol(symbol, error), "circle symbol validates");

    auto invalid = symbol;
    auto* shape = std::get_if<SymbolShape>(&invalid.objects.front());
    shape->x = 100;
    require(!validate_symbol(invalid, error), "shape outside symbol dimensions is rejected");
}

void test_text_alignment_bounds_hit_testing_and_resize() {
    SymbolDefinition symbol;
    symbol.size = {180.0F, 100.0F};
    SymbolText label;
    label.id = "label";
    label.value = "HALF ADDER";
    label.x = 90.0F;
    label.y = 20.0F;
    label.size = 12.0F;
    label.alignment = SymbolTextAlignment::Center;
    symbol.objects.emplace_back(label);

    SymbolError error;
    require(validate_symbol(symbol, error), "centered text stays within symbol using aligned text bounds");
    require(symbol_object_at(symbol, {90.0F, 25.0F}, 0.0F).has_value(),
            "text hit testing uses top-left glyph bounds and alignment");
    require(!symbol_object_at(symbol, {90.0F, 12.0F}, 0.0F).has_value(),
            "text hit testing does not use the old inverted vertical bounds");

    require(align_symbol_object(symbol, "label", SymbolAlignment::Left),
            "text aligns using its rendered left edge");
    const auto& aligned = std::get<SymbolText>(symbol.objects.front());
    const float estimated_width = std::max(aligned.size, static_cast<float>(aligned.value.size()) * aligned.size * 0.55F);
    require(std::abs(aligned.x - estimated_width * 0.5F) < 0.001F,
            "center-aligned text anchor follows its aligned visible bounds");
    require(resize_symbol_object(symbol, "label", {0.0F, 500.0F}),
            "text size can be increased within available canvas bounds");
    require(validate_symbol(symbol, error), "resized text remains within the symbol canvas");

    auto invalid = symbol;
    auto& invalid_text = std::get<SymbolText>(invalid.objects.front());
    invalid_text.y = invalid.size.y - 1.0F;
    require(!validate_symbol(invalid, error), "text that would render below the symbol bounds is rejected");
}

void test_freeform_line_geometry_and_edits() {
    SymbolDefinition symbol;
    symbol.size = {160.0F, 120.0F};
    SymbolShape line;
    line.id = "diagonal";
    line.type = SymbolShapeType::Line;
    line.x = 140.0F;
    line.y = 15.0F;
    line.width = 0.0F;
    line.height = 0.0F;
    line.points = {{140.0F, 15.0F}, {20.0F, 100.0F}};
    line.fill = "none";
    symbol.objects.emplace_back(line);

    SymbolError error;
    require(validate_symbol(symbol, error), "a line with a negative slope remains within the symbol bounds");
    require(symbol_object_at(symbol, {80.0F, 57.5F}, 1.0F).has_value(),
            "hit testing follows both explicit line endpoints");
    require(move_symbol_object(symbol, "diagonal", {-5.0F, 4.0F}),
            "explicit line endpoints can be moved together");
    require(resize_symbol_object(symbol, "diagonal", {8.0F, 6.0F}),
            "explicit line endpoints can be resized");
    require(validate_symbol(symbol, error), "moved and resized line remains inside the symbol");

    std::string serialized;
    require(serialize_symbol(symbol, serialized, error), "negative-slope line serializes");
    SymbolDefinition parsed;
    require(parse_symbol(serialized, parsed, error), "negative-slope line parses");
    const auto* restored = std::get_if<SymbolShape>(&parsed.objects.front());
    require(restored && restored->points.size() == 2 && restored->points[0].x > restored->points[1].x,
            "roundtrip preserves endpoint order and negative slope");
}

void test_port_orientation_and_instance_endpoint_transform() {
    SymbolDefinition symbol = make_default_symbol({160, 100}, {{"in", "input"}, {"out", "output"}});
    require(set_symbol_port_orientation(symbol, "in", SymbolPortOrientation::Angle, 37.5F),
            "port may use an arbitrary visual angle");
    const auto found = std::find_if(symbol.ports.begin(), symbol.ports.end(), [](const auto& port) {
        return port.port_id == "in";
    });
    require(found != symbol.ports.end() && found->angle_degrees == 37.5F,
            "visual orientation is stored independently of logical port ID");

    CircuitEditor circuit;
    std::vector<CustomPortLayout> layouts{
        {"in", "Input", PinDirection::Input, {0, 30}, true},
        {"out", "Output", PinDirection::Output, {160, 70}, true},
    };
    const auto component_id = circuit.add_custom_component("test.symbol", {300, 200}, layouts, {160, 100});
    require(component_id != kInvalidComponent, "custom test instance is created");
    require(circuit.set_component_transform(component_id, 1, true, false), "component transform is applied");
    const auto* component = circuit.find_component(component_id);
    require(component && component->inputs.size() == 1 && component->outputs.size() == 1,
            "custom instance retains logical pins");
    const SymbolTransform transform{1, true, false};
    const auto expected_input = Vec2{300,200} + transform_symbol_point({0,30}, {160,100}, transform);
    const auto actual_input = circuit.pin_world_position(component->inputs.front().id);
    require(std::abs(expected_input.x-actual_input.x)<0.001F && std::abs(expected_input.y-actual_input.y)<0.001F,
            "wire endpoint follows the transformed visual input port");
}


void test_symbol_alignment_rotation_duplication_and_circle_resize() {
    SymbolDefinition symbol;
    symbol.size = {160, 120};
    SymbolShape rect;
    rect.id = "rect"; rect.x = 28; rect.y = 24; rect.width = 44; rect.height = 26;
    symbol.objects.emplace_back(rect);
    require(align_symbol_object(symbol, "rect", SymbolAlignment::HorizontalCenter),
            "shape can be aligned to horizontal canvas center");
    const auto& aligned = std::get<SymbolShape>(symbol.objects.front());
    require(std::abs((aligned.x + aligned.width * 0.5F) - 80.0F) < 0.001F,
            "horizontal center alignment produces a centered shape");
    require(align_symbol_object(symbol, "rect", SymbolAlignment::Bottom),
            "shape can be aligned to bottom edge");
    require(std::abs(aligned.y + aligned.height - 120.0F) < 0.001F,
            "bottom alignment produces an exact bottom edge");
    require(rotate_symbol_object(symbol, "rect", 45.0F), "rectangle can rotate into an editable polygon");
    const auto& rotated = std::get<SymbolShape>(symbol.objects.front());
    require(rotated.type == SymbolShapeType::Polygon && rotated.points.size() >= 4,
            "rotated rectangle becomes vector polygon geometry");
    SymbolError error;
    require(validate_symbol(symbol, error), "rotated geometry stays within symbol bounds");
    require(duplicate_symbol_object(symbol, "rect", "rect_copy"), "symbol objects can be duplicated");
    require(validate_symbol(symbol, error), "duplicated geometry remains valid");

    SymbolDefinition circle_symbol;
    circle_symbol.size = {100, 90};
    SymbolShape circle;
    circle.id = "circle"; circle.type = SymbolShapeType::Circle;
    circle.x = 70; circle.y = 60; circle.width = 20; circle.height = 20;
    circle_symbol.objects.emplace_back(circle);
    require(resize_symbol_object(circle_symbol, "circle", {100,100}),
            "circle resize is clamped instead of overflowing the symbol canvas");
    const auto& resized = std::get<SymbolShape>(circle_symbol.objects.front());
    require(resized.width == resized.height && resized.x >= 0 && resized.y >= 0 &&
            resized.x + resized.width <= circle_symbol.size.x &&
            resized.y + resized.height <= circle_symbol.size.y,
            "resized circle remains square and inside the symbol canvas");
    require(validate_symbol(circle_symbol, error), "resized circle validates");
}

void test_symbol_parser_strictness_and_port_validation() {
    SymbolDefinition destination = make_default_symbol({160, 100});
    const auto previous = destination;
    SymbolError error;
    const std::string base =
        "HCSS 1\n"
        "size width=160 height=100\n"
        "shape polygon id=p x=0 y=0 width=0 height=0 radius=0 start=0 end=360 points=\"10,10;50,10;30,50;\" fill=none stroke=#FFFFFF stroke_width=2 visible=1 locked=0\n";
    require(!parse_symbol(base, destination, error), "trailing separator in a path point list is rejected");
    require(destination.size.x == previous.size.x, "malformed symbol leaves the destination unchanged");

    auto invalid = make_default_symbol({160, 100}, {{"in", "input"}});
    invalid.ports.front().orientation = static_cast<SymbolPortOrientation>(255);
    require(!validate_symbol(invalid, error), "invalid port orientation enum is rejected");
    require(!move_symbol_port(invalid, "in", {10,10}, static_cast<SymbolPortOrientation>(255)),
            "port mutation rejects invalid orientation values");

    auto invalid_text = make_default_symbol({160, 100});
    SymbolText text;
    text.id = "text"; text.value = "Label"; text.x = 20; text.y = 40;
    text.alignment = static_cast<SymbolTextAlignment>(255);
    invalid_text.objects.emplace_back(text);
    require(!validate_symbol(invalid_text, error), "invalid text alignment enum is rejected");
}

void test_visual_ports_drive_real_instance_pins_and_do_not_rebuild_runtime() {
    CustomComponentDefinition definition;
    PersistenceError error;
    require(create_empty_component_definition("custom.visual-ports", "Visual Ports", "Custom", definition, error),
            "create component definition for visual port test");
    require(add_custom_component_port(definition, "data", "Data", PinDirection::Input, error),
            "add visual input port");
    require(add_custom_component_port(definition, "result", "Result", PinDirection::Output, error),
            "add visual output port");
    require(move_symbol_port(definition.symbol, "data", {definition.size.x * 0.5F, 0.0F}, SymbolPortOrientation::Top),
            "move input visual port to the top edge");
    require(move_symbol_port(definition.symbol, "result", {definition.size.x, definition.size.y * 0.5F}, SymbolPortOrientation::Right),
            "move output visual port to the right edge");

    ComponentLibrary library;
    require(library.add(definition, error), "publish visual port definition");
    const auto* published = library.find(definition.id);
    require(published != nullptr, "find published visual port definition");
    const auto layouts = custom_component_port_layouts(*published);
    require(layouts.size() == 2 && layouts[0].explicit_position && layouts[1].explicit_position,
            "library layout includes explicit visual port positions");

    CircuitEditor root;
    const Vec2 component_position{320.0F, 200.0F};
    const auto instance = root.add_custom_component(definition.id, component_position, layouts, definition.size);
    require(instance != kInvalidComponent, "place a visual custom instance");
    require(root.set_component_transform(instance, 1, true, false), "rotate and flip custom instance");
    const auto* component = root.find_component(instance);
    require(component && component->inputs.size() == 1 && component->outputs.size() == 1,
            "custom instance exposes logical ports");
    const SymbolTransform transform{1, true, false};
    const auto expected_input = component_position + transform_symbol_point(
        {definition.size.x * 0.5F, 0.0F}, definition.size, transform);
    const auto input_position = root.pin_world_position(component->inputs.front().id);
    require(std::abs(input_position.x - expected_input.x) < 0.001F &&
            std::abs(input_position.y - expected_input.y) < 0.001F,
            "main editor pin endpoint follows the visual symbol after rotation and flip");

    CircuitRuntime runtime(RuntimeBackend::InterpretedEvent);
    runtime.synchronize(root, library);
    (void)runtime.step();
    const auto ticks_before = runtime.tick_count();
    const auto logic_revision = library.logic_revision();
    auto visual_edit = *library.find(definition.id);
    require(move_symbol_object(visual_edit.symbol, "body", {2.0F, 1.0F}),
            "visual appearance can be edited independently");
    require(library.replace(std::move(visual_edit), error), "publish visual-only definition update");
    require(library.logic_revision() == logic_revision,
            "visual-only update does not change the logical library revision");
    runtime.synchronize(root, library);
    require(runtime.tick_count() == ticks_before,
            "visual-only update does not rebuild or reset the simulator runtime");
}

void test_documented_symbol_examples_parse_and_serialize() {
    const std::filesystem::path directory = std::filesystem::path(HCS_SOURCE_DIR) / "examples" / "symbols";
    const std::array names{"half_adder.hcssymbol", "full_adder.hcssymbol", "circular_component.hcssymbol",
                           "four_sided_ports.hcssymbol", "polygon_contour.hcssymbol"};
    for (const auto* name : names) {
        std::ifstream file(directory / name, std::ios::binary);
        require(static_cast<bool>(file), "documented HCSS example must be present");
        std::ostringstream contents;
        contents << file.rdbuf();
        SymbolDefinition symbol;
        SymbolError error;
        require(parse_symbol(contents.str(), symbol, error), "documented HCSS example must parse");
        std::string canonical;
        require(serialize_symbol(symbol, canonical, error), "documented HCSS example must serialize");
        SymbolDefinition reparsed;
        require(parse_symbol(canonical, reparsed, error), "canonical example must reparse");
        std::string second;
        require(serialize_symbol(reparsed, second, error) && canonical == second,
                "documented HCSS examples must serialize deterministically");
    }
}

void test_old_component_definition_defaults_to_rectangle() {
    // This deliberately omits the optional symbol-begin/symbol-end block used by HCSS 1.
    const std::string old_definition =
        "HCSC 1\n"
        "id custom.legacy\n"
        "name \"Legacy\"\n"
        "description \"Old definition\"\n"
        "category \"Custom\"\n"
        "size width=150 height=84\n"
        "port input a \"A\" terminal=1\n"
        "port output y \"Y\" terminal=2\n"
        "implementation\n"
        "HCS 1\n"
        "circuit \"Legacy implementation\"\n"
        "component 1 Input x=0 y=0 value=0\n"
        "component 2 Output x=100 y=0\n";
    ComponentLibrary library;
    CustomComponentDefinition parsed;
    PersistenceError error;
    require(parse_component_definition(old_definition, library, parsed, error), "legacy HCSC without symbol should load");
    require(!parsed.symbol.objects.empty() && parsed.symbol.ports.size() == 2,
            "legacy definition gets a default symbol and visual endpoints for its logical ports");
    require(parsed.symbol.ports[0].port_id == "a" && parsed.symbol.ports[1].port_id == "y",
            "legacy default symbol preserves the logical port IDs");
}

} // namespace

int main() {
    test_symbol_roundtrip_and_determinism();
    test_symbol_edits_and_hit_testing();
    test_transform_roundtrip_and_hit_testing();
    test_validation_rejects_malformed_symbols();
    test_vertex_editing_and_path_closure();
    test_shape_hit_testing_and_bounds();
    test_text_alignment_bounds_hit_testing_and_resize();
    test_freeform_line_geometry_and_edits();
    test_port_orientation_and_instance_endpoint_transform();
    test_symbol_alignment_rotation_duplication_and_circle_resize();
    test_symbol_parser_strictness_and_port_validation();
    test_visual_ports_drive_real_instance_pins_and_do_not_rebuild_runtime();
    test_documented_symbol_examples_parse_and_serialize();
    test_old_component_definition_defaults_to_rectangle();
    std::cout << "PASS: symbol model, serialization, transforms and compatibility tests\n";
}
