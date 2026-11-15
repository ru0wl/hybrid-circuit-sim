#include "hcs/editor/persistence.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace hcs::editor {
namespace {

constexpr std::size_t kMaximumFileBytes = 16U * 1024U * 1024U;
constexpr float kMaximumPosition = 10'000'000.0F;
constexpr std::size_t kMaximumNameBytes = 256;

struct Token {
    std::string value;
    bool quoted{false};
};

struct ComponentRecord {
    ComponentId id{kInvalidComponent};
    ComponentType type{ComponentType::Input};
    Vec2 position{};
    bool input_value{false};
    bool dff_initial_state{false};
    std::size_t line{0};
};

struct Endpoint {
    ComponentId component{kInvalidComponent};
    std::string pin;
};

struct WireRecord {
    Endpoint source;
    Endpoint destination;
    std::size_t line{0};
};

bool tokenize_line(std::string_view line, std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        if (i == line.size() || line[i] == '#') break;

        Token token;
        if (line[i] == '"') {
            token.quoted = true;
            ++i;
            bool closed = false;
            while (i < line.size()) {
                const char ch = line[i++];
                if (ch == '"') { closed = true; break; }
                if (ch == '\\') {
                    if (i == line.size()) { error = "unterminated escape in quoted string"; return false; }
                    const char escaped = line[i++];
                    switch (escaped) {
                        case 'n': token.value.push_back('\n'); break;
                        case 'r': token.value.push_back('\r'); break;
                        case 't': token.value.push_back('\t'); break;
                        case '"': token.value.push_back('"'); break;
                        case '\\': token.value.push_back('\\'); break;
                        default: error = "unsupported escape sequence in quoted string"; return false;
                    }
                } else {
                    token.value.push_back(ch);
                }
            }
            if (!closed) { error = "unterminated quoted string"; return false; }
            if (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '#') {
                error = "expected whitespace after quoted string";
                return false;
            }
        } else {
            const auto begin = i;
            while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '#') ++i;
            token.value.assign(line.substr(begin, i - begin));
        }
        tokens.push_back(std::move(token));
    }
    return true;
}

template <typename T>
bool parse_integer(std::string_view text, T& value) {
    if (text.empty()) return false;
    T parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) return false;
    value = parsed;
    return true;
}

bool parse_float(std::string_view text, float& value) {
    if (text.empty()) return false;
    float parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed, std::chars_format::general);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || !std::isfinite(parsed) ||
        std::abs(parsed) > kMaximumPosition) return false;
    value = parsed;
    return true;
}

bool parse_bit(std::string_view text, bool& value) {
    if (text == "0") { value = false; return true; }
    if (text == "1") { value = true; return true; }
    return false;
}

bool parse_component_type(std::string_view text, ComponentType& type) {
    if (text == "Input") type = ComponentType::Input;
    else if (text == "Output") type = ComponentType::Output;
    else if (text == "AND") type = ComponentType::And;
    else if (text == "OR") type = ComponentType::Or;
    else if (text == "NOT") type = ComponentType::Not;
    else if (text == "XOR") type = ComponentType::Xor;
    else if (text == "DFF") type = ComponentType::DFlipFlop;
    else return false;
    return true;
}

std::string_view component_type_text(ComponentType type) {
    switch (type) {
        case ComponentType::Input: return "Input";
        case ComponentType::Output: return "Output";
        case ComponentType::And: return "AND";
        case ComponentType::Or: return "OR";
        case ComponentType::Not: return "NOT";
        case ComponentType::Xor: return "XOR";
        case ComponentType::DFlipFlop: return "DFF";
    }
    return "UNKNOWN";
}

bool endpoint_pin_exists(const ComponentRecord& component, std::string_view pin, bool source) {
    if (source) {
        if (component.type == ComponentType::Output) return false;
        return pin == (component.type == ComponentType::DFlipFlop ? "q" : "out");
    }
    switch (component.type) {
        case ComponentType::Input: return false;
        case ComponentType::Output: return pin == "in";
        case ComponentType::And:
        case ComponentType::Or:
        case ComponentType::Xor: return pin == "in0" || pin == "in1";
        case ComponentType::Not: return pin == "in";
        case ComponentType::DFlipFlop: return pin == "d" || pin == "clk";
    }
    return false;
}

std::string pin_name(const Component& component, const Pin& pin) {
    if (pin.direction == PinDirection::Output) {
        return component.type == ComponentType::DFlipFlop ? "q" : "out";
    }
    switch (component.type) {
        case ComponentType::Output: return "in";
        case ComponentType::Not: return "in";
        case ComponentType::DFlipFlop:
            return !component.inputs.empty() && component.inputs.front().id == pin.id ? "d" : "clk";
        case ComponentType::And:
        case ComponentType::Or:
        case ComponentType::Xor:
            return !component.inputs.empty() && component.inputs.front().id == pin.id ? "in0" : "in1";
        case ComponentType::Input: break;
    }
    return "unknown";
}

bool parse_endpoint(std::string_view text, Endpoint& endpoint) {
    const auto dot = text.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 >= text.size() ||
        text.find('.', dot + 1) != std::string_view::npos) return false;
    std::uint32_t id{};
    if (!parse_integer(text.substr(0, dot), id) || id == kInvalidComponent) return false;
    endpoint.component = id;
    endpoint.pin.assign(text.substr(dot + 1));
    return true;
}

std::string quote_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(ch); break;
        }
    }
    result.push_back('"');
    return result;
}

std::string float_text(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return stream.str();
}

bool fail(PersistenceError& error, std::size_t line, std::string message) {
    error.line = line;
    error.message = std::move(message);
    return false;
}

bool parse_document(std::string_view text, std::string& name,
                    std::vector<ComponentRecord>& components, std::vector<WireRecord>& wires,
                    PersistenceError& error) {
    error = {};
    components.clear();
    wires.clear();
    name.clear();
    std::map<ComponentId, std::size_t> component_indices;
    std::vector<Token> tokens;
    std::size_t line_number = 0;
    bool header_seen = false;
    bool name_seen = false;

    while (!text.empty()) {
        ++line_number;
        const auto newline = text.find('\n');
        const auto line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        std::string token_error;
        if (!tokenize_line(line, tokens, token_error)) return fail(error, line_number, token_error);
        if (tokens.empty()) continue;

        if (!header_seen) {
            unsigned version{};
            if (tokens.size() != 2 || tokens[0].value != "HCS" || tokens[0].quoted || tokens[1].quoted ||
                !parse_integer(tokens[1].value, version)) {
                return fail(error, line_number, "expected file header 'HCS 1'");
            }
            if (version != kCircuitFormatVersion)
                return fail(error, line_number, "unsupported HCS format version " + std::to_string(version));
            header_seen = true;
            continue;
        }

        if (tokens[0].value == "circuit") {
            if (name_seen) return fail(error, line_number, "duplicate circuit name declaration");
            if (tokens.size() != 2 || !tokens[1].quoted)
                return fail(error, line_number, "expected: circuit \"name\"");
            if (tokens[1].value.size() > kMaximumNameBytes)
                return fail(error, line_number, "circuit name exceeds 256 bytes");
            if (tokens[1].value.find('\0') != std::string::npos)
                return fail(error, line_number, "circuit name contains a null byte");
            name = tokens[1].value;
            name_seen = true;
            continue;
        }

        if (tokens[0].value == "component") {
            if (tokens.size() < 5) return fail(error, line_number, "component requires an ID, type, x and y fields");
            ComponentRecord component;
            component.line = line_number;
            if (tokens[1].quoted || !parse_integer(tokens[1].value, component.id) || component.id == kInvalidComponent)
                return fail(error, line_number, "invalid component ID '" + tokens[1].value + "'");
            if (tokens[2].quoted || !parse_component_type(tokens[2].value, component.type))
                return fail(error, line_number, "unknown component type '" + tokens[2].value + "'");
            if (component_indices.contains(component.id))
                return fail(error, line_number, "duplicate component ID " + std::to_string(component.id));

            std::map<std::string, std::string> fields;
            for (std::size_t i = 3; i < tokens.size(); ++i) {
                if (tokens[i].quoted) return fail(error, line_number, "component fields must use key=value syntax");
                const auto equal = tokens[i].value.find('=');
                if (equal == std::string::npos || equal == 0 || equal + 1 == tokens[i].value.size())
                    return fail(error, line_number, "malformed component field '" + tokens[i].value + "'");
                const auto key = tokens[i].value.substr(0, equal);
                const auto value = tokens[i].value.substr(equal + 1);
                if (!fields.emplace(key, value).second)
                    return fail(error, line_number, "duplicate component field '" + key + "'");
            }
            for (const auto& [key, value] : fields) {
                (void)value;
                if (key != "x" && key != "y" && key != "value" && key != "initial")
                    return fail(error, line_number, "unknown component field '" + key + "'");
            }
            const auto x = fields.find("x");
            const auto y = fields.find("y");
            if (x == fields.end() || y == fields.end())
                return fail(error, line_number, "component " + std::to_string(component.id) + " is missing x or y");
            if (!parse_float(x->second, component.position.x))
                return fail(error, line_number, "component " + std::to_string(component.id) + " has invalid x position");
            if (!parse_float(y->second, component.position.y))
                return fail(error, line_number, "component " + std::to_string(component.id) + " has invalid y position");
            if (const auto value = fields.find("value"); value != fields.end()) {
                if (component.type != ComponentType::Input)
                    return fail(error, line_number, "field 'value' is only valid for Input components");
                if (!parse_bit(value->second, component.input_value))
                    return fail(error, line_number, "component " + std::to_string(component.id) + " has invalid input value (expected 0 or 1)");
            }
            if (const auto initial = fields.find("initial"); initial != fields.end()) {
                if (component.type != ComponentType::DFlipFlop)
                    return fail(error, line_number, "field 'initial' is only valid for DFF components");
                if (!parse_bit(initial->second, component.dff_initial_state))
                    return fail(error, line_number, "component " + std::to_string(component.id) + " has invalid initial state (expected 0 or 1)");
            }
            component_indices.emplace(component.id, components.size());
            components.push_back(component);
            continue;
        }

        if (tokens[0].value == "wire") {
            if (tokens.size() != 4 || tokens[2].value != "->" || tokens[0].quoted ||
                tokens[1].quoted || tokens[2].quoted || tokens[3].quoted)
                return fail(error, line_number, "expected: wire <component.pin> -> <component.pin>");
            WireRecord wire;
            wire.line = line_number;
            if (!parse_endpoint(tokens[1].value, wire.source))
                return fail(error, line_number, "invalid wire source endpoint '" + tokens[1].value + "'");
            if (!parse_endpoint(tokens[3].value, wire.destination))
                return fail(error, line_number, "invalid wire destination endpoint '" + tokens[3].value + "'");
            wires.push_back(std::move(wire));
            continue;
        }

        return fail(error, line_number, "unknown statement '" + tokens[0].value + "'");
    }

    if (!header_seen) return fail(error, 1, "missing HCS file header");
    if (!name_seen) return fail(error, 0, "missing circuit name declaration");

    std::set<std::pair<ComponentId, std::string>> driven_inputs;
    for (const auto& wire : wires) {
        const auto source = component_indices.find(wire.source.component);
        const auto destination = component_indices.find(wire.destination.component);
        if (source == component_indices.end())
            return fail(error, wire.line, "wire references missing source component " + std::to_string(wire.source.component));
        if (destination == component_indices.end())
            return fail(error, wire.line, "wire references missing destination component " + std::to_string(wire.destination.component));
        const auto& source_component = components[source->second];
        const auto& destination_component = components[destination->second];
        if (!endpoint_pin_exists(source_component, wire.source.pin, true))
            return fail(error, wire.line, "'" + std::to_string(wire.source.component) + "." + wire.source.pin + "' is not an output pin");
        if (!endpoint_pin_exists(destination_component, wire.destination.pin, false))
            return fail(error, wire.line, "'" + std::to_string(wire.destination.component) + "." + wire.destination.pin + "' is not an input pin");
        if (!driven_inputs.emplace(wire.destination.component, wire.destination.pin).second)
            return fail(error, wire.line, "multiple drivers target input '" + std::to_string(wire.destination.component) + "." + wire.destination.pin + "'");
    }
    return true;
}

} // namespace

std::string PersistenceError::describe() const {
    if (line == 0) return message;
    return "line " + std::to_string(line) + ": " + message;
}

bool serialize_circuit(const CircuitEditor& circuit, std::string_view circuit_name,
                       std::string& text, PersistenceError& error) {
    error = {};
    if (circuit_name.size() > kMaximumNameBytes)
        return fail(error, 0, "circuit name exceeds 256 bytes");
    if (circuit_name.find('\0') != std::string_view::npos)
        return fail(error, 0, "circuit name contains a null byte");

    std::vector<const Component*> components;
    components.reserve(circuit.components().size());
    for (const auto& component : circuit.components()) {
        if (!std::isfinite(component.position.x) || !std::isfinite(component.position.y) ||
            std::abs(component.position.x) > kMaximumPosition || std::abs(component.position.y) > kMaximumPosition)
            return fail(error, 0, "component " + std::to_string(component.id) + " has an invalid position");
        components.push_back(&component);
    }
    std::sort(components.begin(), components.end(), [](const Component* a, const Component* b) { return a->id < b->id; });

    struct SerializedWire { std::string value; };
    std::vector<SerializedWire> wires;
    wires.reserve(circuit.wires().size());
    for (const auto& wire : circuit.wires()) {
        const auto* source_pin = circuit.find_pin(wire.output_pin);
        const auto* destination_pin = circuit.find_pin(wire.input_pin);
        const auto* source_component = source_pin ? circuit.find_component(source_pin->owner) : nullptr;
        const auto* destination_component = destination_pin ? circuit.find_component(destination_pin->owner) : nullptr;
        if (!source_pin || !destination_pin || !source_component || !destination_component ||
            source_pin->direction != PinDirection::Output || destination_pin->direction != PinDirection::Input)
            return fail(error, 0, "wire " + std::to_string(wire.id) + " contains an invalid pin reference");
        wires.push_back({std::to_string(source_component->id) + "." + pin_name(*source_component, *source_pin) +
                         " -> " + std::to_string(destination_component->id) + "." + pin_name(*destination_component, *destination_pin)});
    }
    std::sort(wires.begin(), wires.end(), [](const SerializedWire& a, const SerializedWire& b) { return a.value < b.value; });

    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "HCS " << kCircuitFormatVersion << '\n';
    output << "circuit " << quote_string(circuit_name) << "\n\n";
    for (const auto* component : components) {
        output << "component " << component->id << ' ' << component_type_text(component->type)
               << " x=" << float_text(component->position.x)
               << " y=" << float_text(component->position.y);
        if (component->type == ComponentType::Input) output << " value=" << (component->input_value ? 1 : 0);
        if (component->type == ComponentType::DFlipFlop) output << " initial=" << (component->dff_initial_state ? 1 : 0);
        output << '\n';
    }
    if (!wires.empty()) {
        output << '\n';
        for (const auto& wire : wires) output << "wire " << wire.value << '\n';
    }
    text = output.str();
    return true;
}

bool parse_circuit(std::string_view text, CircuitEditor& destination,
                   std::string& circuit_name, PersistenceError& error) {
    if (text.size() > kMaximumFileBytes) return fail(error, 0, "circuit file exceeds the 16 MiB size limit");

    std::string parsed_name;
    std::vector<ComponentRecord> components;
    std::vector<WireRecord> wires;
    if (!parse_document(text, parsed_name, components, wires, error)) return false;

    std::sort(components.begin(), components.end(), [](const ComponentRecord& a, const ComponentRecord& b) { return a.id < b.id; });
    CircuitEditor candidate;
    for (const auto& component : components) {
        if (!candidate.add_component_with_id(component.id, component.type, component.position,
                                             component.input_value, component.dff_initial_state))
            return fail(error, component.line, "could not construct component " + std::to_string(component.id));
    }
    for (const auto& wire : wires) {
        const auto* source_component = candidate.find_component(wire.source.component);
        const auto* destination_component = candidate.find_component(wire.destination.component);
        if (!source_component || !destination_component)
            return fail(error, wire.line, "wire references a component that could not be constructed");

        PinId source_pin = kInvalidPin;
        PinId destination_pin = kInvalidPin;
        for (const auto& pin : source_component->outputs)
            if (pin_name(*source_component, pin) == wire.source.pin) source_pin = pin.id;
        for (const auto& pin : destination_component->inputs)
            if (pin_name(*destination_component, pin) == wire.destination.pin) destination_pin = pin.id;
        if (source_pin == kInvalidPin || destination_pin == kInvalidPin)
            return fail(error, wire.line, "wire endpoint pin was not found while constructing the model");
        if (!candidate.connect(source_pin, destination_pin))
            return fail(error, wire.line, "wire could not be connected (input may already have a driver)");
    }

    destination.replace_with(std::move(candidate));
    circuit_name = std::move(parsed_name);
    error = {};
    return true;
}

bool save_circuit_file(const std::filesystem::path& path, const CircuitEditor& circuit,
                       std::string_view circuit_name, PersistenceError& error) {
    if (path.empty()) return fail(error, 0, "output path is empty");
    std::string text;
    if (!serialize_circuit(circuit, circuit_name, text, error)) return false;

    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return fail(error, 0, "cannot open temporary file '" + temporary.string() + "' for writing");
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            file.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return fail(error, 0, "failed while writing temporary file '" + temporary.string() + "'");
        }
    }
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return fail(error, 0, "cannot atomically replace '" + path.string() + "': " + rename_error.message());
    }
    error = {};
    return true;
}

bool load_circuit_file(const std::filesystem::path& path, CircuitEditor& destination,
                       std::string& circuit_name, PersistenceError& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail(error, 0, "cannot open '" + path.string() + "' for reading");
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    if (length < 0) return fail(error, 0, "could not determine file size");
    if (static_cast<std::uintmax_t>(length) > kMaximumFileBytes)
        return fail(error, 0, "circuit file exceeds the 16 MiB size limit");
    file.seekg(0, std::ios::beg);
    std::string text(static_cast<std::size_t>(length), '\0');
    if (!text.empty()) file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file && !file.eof()) return fail(error, 0, "failed while reading circuit file");
    return parse_circuit(text, destination, circuit_name, error);
}

} // namespace hcs::editor
