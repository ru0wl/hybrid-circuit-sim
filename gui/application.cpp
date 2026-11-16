#include "renderer.hpp"
#include "symbol_renderer.hpp"

#include "hcs/editor/simulation_controller.hpp"
#include "hcs/editor/persistence.hpp"
#include "hcs/editor/custom_components.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <set>
#include <vector>
#include <filesystem>
#include <cmath>
#include <type_traits>
#include <variant>

namespace {
using hcs::editor::Camera2D;
using hcs::editor::CircuitEditor;
using hcs::editor::CircuitRuntime;
using hcs::editor::ComponentId;
using hcs::editor::ComponentType;
using hcs::editor::CustomPortLayout;
using hcs::editor::PinDirection;
using hcs::editor::PinId;
using hcs::editor::Rect;
using hcs::editor::RuntimeBackend;
using hcs::editor::SimulationController;
using hcs::editor::SimulationRate;
using hcs::editor::Vec2;
using hcs::gui::CircuitRenderer;

constexpr int kInitialWidth = 1440;
constexpr int kInitialHeight = 900;
constexpr float kToolbarHeight = 72.0F;
constexpr float kStatusHeight = 46.0F;
constexpr SDL_Color kToolbar{22, 29, 39, 255};
constexpr SDL_Color kButton{43, 55, 70, 255};
constexpr SDL_Color kButtonActive{54, 98, 139, 255};
constexpr SDL_Color kButtonBorder{82, 104, 128, 255};
constexpr SDL_Color kText{224, 233, 244, 255};
constexpr SDL_Color kMuted{143, 160, 181, 255};
constexpr SDL_Color kLogicOne{255, 190, 82, 255};
constexpr SDL_Color kSelection{89, 178, 255, 255};

struct Button {
    Rect rect;
    std::string label;
    enum class Action : std::uint8_t {
        PlaceInput, PlaceOutput, PlaceAnd, PlaceOr, PlaceNot, PlaceXor, PlaceDff,
        RunPause, Step, Reset, NextBackend, NextRate, ToggleGrid, Demo,
        ToggleSimd, ToggleParallel, CycleWorkers, GpuInfo,
        NewCircuit, OpenCircuit, SaveCircuit, SaveCircuitAs, TextEdit, ApplyText, ReloadText, BackToGraph,
        ToggleLibrary, CreateCustomFromSelection, NewEmptyCustom, EditDefinition,
        SaveDefinition, CancelDefinitionEdit,
        OpenSymbolDesigner, SymbolBack, SymbolTextMode, SymbolTextApply, SymbolTextReload,
        SymbolSelect, SymbolRect, SymbolRoundedRect, SymbolCircle, SymbolEllipse, SymbolLine, SymbolPolygon,
        SymbolPath, SymbolArc, SymbolAddText, SymbolAddInput, SymbolAddOutput, SymbolPreset,
        SymbolSnap, SymbolFill, SymbolStroke, SymbolZoomIn, SymbolZoomOut,
        SymbolStrokeDown, SymbolStrokeUp, SymbolTextSizeDown, SymbolTextSizeUp,
    } action;
};

class Application final {
public:
    bool initialize() {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        window_ = SDL_CreateWindow("HCS - Circuit Editor", kInitialWidth, kInitialHeight,
                                   SDL_WINDOW_RESIZABLE);
        if (!window_) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
            SDL_Quit();
            return false;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(window_);
            window_ = nullptr;
            SDL_Quit();
            return false;
        }
        (void)hcs::gui::FontManager::initialize(renderer_, SDL_GetWindowDisplayScale(window_));
        std::vector<std::string> library_diagnostics;
        if (!library_.load_directory(hcs::editor::ComponentLibrary::default_directory(), library_diagnostics)) {
            for (const auto& diagnostic : library_diagnostics)
                std::fprintf(stderr, "Component library warning: %s\n", diagnostic.c_str());
        }
        SDL_SetWindowMinimumSize(window_, 960, 600);
        update_viewport();
        load_demo();
        if (!hcs::gui::FontManager::using_ttf()) {
            set_status("SDL3_ttf or a supported system font was not found; using the pixel-aligned ASCII fallback.");
        }
        update_window_title();
        return true;
    }

    ~Application() {
        hcs::gui::FontManager::shutdown();
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int run() {
        using Clock = std::chrono::steady_clock;
        auto previous = Clock::now();
        while (running_) {
            const auto now = Clock::now();
            const auto delta = std::chrono::duration<double>(now - previous).count();
            previous = now;

            process_events();
            try {
                runtime_.synchronize(circuit_, library_);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Circuit synchronization failed: %s\n", error.what());
                controller_.pause();
            }
            controller_.update(delta);
            update_window_title();
            render();
            SDL_Delay(1);
        }
        return 0;
    }

private:
    enum class PathMode : std::uint8_t {
        None, Open, SaveAs, DefinitionName, NewDefinitionName,
        EditDefinitionName, EditDefinitionDescription, EditDefinitionCategory, PortName,
        SymbolObjectText, SymbolPortLabel
    };
    enum class DeferredAction : std::uint8_t { None, NewCircuit, OpenCircuit, Demo, Quit, CancelDefinitionEdit };
    enum class SymbolTool : std::uint8_t { Select, Rectangle, RoundedRectangle, Circle, Ellipse, Line, Polygon, Path, Arc, Text };
    enum class SymbolDrag : std::uint8_t { None, Object, Resize, Port, Vertex, Draw, Pan };
    struct SymbolCanvas { Rect rect{}; Vec2 origin{}; float zoom{1.0F}; };

    void update_viewport() {
        int width = kInitialWidth;
        int height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        camera_.set_viewport(Rect{0.0F, kToolbarHeight, static_cast<float>(width),
                                  std::max(1.0F, static_cast<float>(height) - kToolbarHeight - kStatusHeight)});
    }

    static PinId input_pin(const CircuitEditor& circuit, ComponentId component, std::size_t index = 0) {
        const auto* found = circuit.find_component(component);
        return found && index < found->inputs.size() ? found->inputs[index].id : hcs::editor::kInvalidPin;
    }
    static PinId output_pin(const CircuitEditor& circuit, ComponentId component, std::size_t index = 0) {
        const auto* found = circuit.find_component(component);
        return found && index < found->outputs.size() ? found->outputs[index].id : hcs::editor::kInvalidPin;
    }

    void load_demo() {
        circuit_.clear();
        const auto inv_in = circuit_.add_component(ComponentType::Input, {-520.0F, -230.0F});
        const auto inv = circuit_.add_component(ComponentType::Not, {-300.0F, -222.0F});
        const auto inv_out = circuit_.add_component(ComponentType::Output, {-80.0F, -230.0F});
        (void)circuit_.connect(output_pin(circuit_, inv_in), input_pin(circuit_, inv));
        (void)circuit_.connect(output_pin(circuit_, inv), input_pin(circuit_, inv_out));

        const auto xor_a = circuit_.add_component(ComponentType::Input, {-520.0F, 30.0F});
        const auto xor_b = circuit_.add_component(ComponentType::Input, {-520.0F, 165.0F});
        const auto xor_gate = circuit_.add_component(ComponentType::Xor, {-300.0F, 80.0F});
        const auto xor_out = circuit_.add_component(ComponentType::Output, {-80.0F, 90.0F});
        (void)circuit_.connect(output_pin(circuit_, xor_a), input_pin(circuit_, xor_gate, 0));
        (void)circuit_.connect(output_pin(circuit_, xor_b), input_pin(circuit_, xor_gate, 1));
        (void)circuit_.connect(output_pin(circuit_, xor_gate), input_pin(circuit_, xor_out));

        circuit_.select_component(std::nullopt);
        placement_.reset();
        wire_start_.reset();
        runtime_.synchronize(circuit_, library_);
        controller_.reset(circuit_, library_);
        controller_.notify_input_changed();
        document_name_ = "HCS Demo";
        current_path_.clear();
        text_buffer_dirty_ = false;
        saved_document_revision_ = circuit_.document_revision();
        text_buffer_.clear();
        update_window_title();
    }

    bool dirty() const noexcept {
        return circuit_.document_revision() != saved_document_revision_ ||
               (editing_definition_.has_value() && definition_metadata_dirty_);
    }

    std::string current_filename() const {
        if (editing_definition_) return editing_definition_->name;
        return current_path_.empty() ? document_name_ : current_path_.filename().string();
    }

    void update_window_title() {
        if (!window_) return;
        std::string title = editing_definition_
            ? "HCS - Component Definition: " + current_filename()
            : "HCS - " + current_filename();
        if (dirty() || text_buffer_dirty_) title += "*";
        if (title != last_window_title_) {
            SDL_SetWindowTitle(window_, title.c_str());
            last_window_title_ = std::move(title);
        }
    }

    void set_status(std::string message) {
        status_message_ = std::move(message);
        update_window_title();
    }

    void execute_action(DeferredAction action) {
        switch (action) {
            case DeferredAction::NewCircuit:
                circuit_.clear();
                circuit_.clear_selection();
                placement_.reset();
                wire_start_.reset();
                current_path_.clear();
                document_name_ = "Untitled";
                text_buffer_.clear();
                text_buffer_dirty_ = false;
                runtime_.synchronize(circuit_, library_);
                controller_.reset(circuit_, library_);
                controller_.pause();
                saved_document_revision_ = circuit_.document_revision();
                set_status("Created a new circuit.");
                break;
            case DeferredAction::OpenCircuit:
                begin_path_entry(PathMode::Open);
                break;
            case DeferredAction::Demo:
                load_demo();
                set_status("Loaded the HCS demo circuit.");
                break;
            case DeferredAction::Quit:
                running_ = false;
                break;
            case DeferredAction::CancelDefinitionEdit:
                cancel_definition_edit();
                break;
            case DeferredAction::None:
                break;
        }
    }

    void request_action(DeferredAction action) {
        if (action == DeferredAction::None) return;
        if (editing_definition_ && action != DeferredAction::CancelDefinitionEdit) {
            set_status("Save or cancel the component definition edit before switching circuits.");
            return;
        }
        if (dirty() || text_buffer_dirty_) {
            confirm_pending_ = action;
            controller_.pause();
            return;
        }
        execute_action(action);
    }

    void cancel_confirmation() {
        confirm_pending_ = DeferredAction::None;
        after_save_action_ = DeferredAction::None;
    }

    void confirm_discard_and_continue() {
        const auto action = confirm_pending_;
        confirm_pending_ = DeferredAction::None;
        after_save_action_ = DeferredAction::None;
        text_buffer_dirty_ = false;
        text_error_.clear();
        execute_action(action);
    }

    void confirm_save_and_continue() {
        const auto action = confirm_pending_;
        if (editing_definition_) {
            if (text_mode_ && text_buffer_dirty_ && !apply_text()) return;
            if (!save_definition_edit()) return;
            confirm_pending_ = DeferredAction::None;
            after_save_action_ = DeferredAction::None;
            if (action == DeferredAction::CancelDefinitionEdit) return;
            execute_action(action);
            return;
        }
        if (text_buffer_dirty_) {
            const bool was_text_mode = text_mode_;
            if (!apply_text()) return;
            if (was_text_mode) text_mode_ = true;
        }
        confirm_pending_ = DeferredAction::None;
        if (current_path_.empty()) {
            after_save_action_ = action;
            begin_path_entry(PathMode::SaveAs);
            return;
        }
        if (save_to_path(current_path_)) execute_action(action);
        else confirm_pending_ = action;
    }

    void begin_path_entry(PathMode mode) {
        path_mode_ = mode;
        path_buffer_.clear();
        if (mode == PathMode::SaveAs && !current_path_.empty()) path_buffer_ = current_path_.string();
        else if (editing_definition_) {
            if (mode == PathMode::EditDefinitionName) path_buffer_ = editing_definition_->name;
            else if (mode == PathMode::EditDefinitionDescription) path_buffer_ = editing_definition_->description;
            else if (mode == PathMode::EditDefinitionCategory) path_buffer_ = editing_definition_->category;
            else if (mode == PathMode::PortName) {
                const auto found = std::find_if(editing_definition_->ports.begin(), editing_definition_->ports.end(),
                    [&](const hcs::editor::CustomPort& port) { return port.id == pending_port_id_; });
                if (found != editing_definition_->ports.end()) path_buffer_ = found->name;
            } else if (mode == PathMode::SymbolObjectText && selected_symbol_object_id_) {
                for (const auto& object : editing_definition_->symbol.objects)
                    if (const auto* value = std::get_if<hcs::editor::SymbolText>(&object); value && value->id == *selected_symbol_object_id_) path_buffer_ = value->value;
            } else if (mode == PathMode::SymbolPortLabel && selected_symbol_port_id_) {
                const auto found = std::find_if(editing_definition_->symbol.ports.begin(), editing_definition_->symbol.ports.end(),
                    [&](const auto& port) { return port.port_id == *selected_symbol_port_id_; });
                if (found != editing_definition_->symbol.ports.end()) path_buffer_ = found->label;
            }
        }
        status_message_.clear();
        SDL_StartTextInput(window_);
    }

    void cancel_path_entry() {
        path_mode_ = PathMode::None;
        SDL_StopTextInput(window_);
        path_buffer_.clear();
        pending_port_id_.clear();
        after_save_action_ = DeferredAction::None;
    }

    void erase_path_character() {
        if (path_buffer_.empty()) return;
        std::size_t position = path_buffer_.size() - 1;
        while (position > 0 && (static_cast<unsigned char>(path_buffer_[position]) & 0xC0U) == 0x80U) --position;
        path_buffer_.erase(position);
    }

    void begin_definition_edit(hcs::editor::CustomComponentDefinition definition) {
        if (editing_definition_) return;
        root_before_definition_edit_ = circuit_;
        root_document_name_before_definition_edit_ = document_name_;
        root_current_path_before_definition_edit_ = current_path_;
        root_saved_revision_before_definition_edit_ = saved_document_revision_;
        root_text_buffer_before_definition_edit_ = text_buffer_;
        root_text_buffer_dirty_before_definition_edit_ = text_buffer_dirty_;

        editing_definition_ = std::move(definition);
        next_input_port_serial_ = 1;
        next_output_port_serial_ = 1;
        for (const auto& port : editing_definition_->ports) {
            const std::string_view prefix = port.direction == hcs::editor::PinDirection::Input ? "input" : "output";
            if (!port.id.starts_with(prefix)) continue;
            const std::string_view suffix(port.id.data() + prefix.size(), port.id.size() - prefix.size());
            if (suffix.empty()) continue;
            std::size_t parsed = 0;
            bool numeric = true;
            for (const char ch : suffix) {
                if (ch < '0' || ch > '9') { numeric = false; break; }
                const auto digit = static_cast<std::size_t>(ch - '0');
                if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
                    numeric = false;
                    break;
                }
                parsed = parsed * 10U + digit;
            }
            if (!numeric || parsed == std::numeric_limits<std::size_t>::max()) continue;
            auto& next = port.direction == hcs::editor::PinDirection::Input
                ? next_input_port_serial_ : next_output_port_serial_;
            next = std::max(next, parsed + 1U);
        }
        symbol_designer_mode_ = false;
        symbol_text_mode_ = false;
        symbol_text_buffer_dirty_ = false;
        symbol_text_error_.clear();
        symbol_zoom_factor_ = 1.0F;
        symbol_pan_offset_ = {};
        circuit_ = editing_definition_->implementation;
        document_name_ = editing_definition_->name;
        current_path_.clear();
        saved_document_revision_ = circuit_.document_revision();
        definition_metadata_dirty_ = false;
        text_mode_ = false;
        text_buffer_.clear();
        text_buffer_dirty_ = false;
        text_error_.clear();
        placement_.reset();
        placement_custom_.reset();
        wire_start_.reset();
        library_panel_ = false;
        runtime_.invalidate();
        try {
            runtime_.synchronize(circuit_, library_);
            controller_.reset(circuit_, library_);
            controller_.pause();
        } catch (const std::exception& exception) {
            set_status("Definition editor runtime warning: " + std::string(exception.what()));
        }
        set_status("Editing definition. I/O add ports, P rename selected port, Delete removes a port/component; N/D/C edit metadata.");
    }

    void begin_definition_edit(std::string_view id) {
        const auto* definition = library_.find(id);
        if (!definition) {
            set_status("Custom component definition is missing: " + std::string(id));
            return;
        }
        begin_definition_edit(*definition);
    }

    void create_empty_definition(const std::string& name) {
        auto id = hcs::editor::make_definition_id(name);
        const auto base_id = id;
        for (std::uint32_t suffix = 2; library_.find(id); ++suffix)
            id = base_id + "-" + std::to_string(suffix);
        hcs::editor::CustomComponentDefinition definition;
        hcs::editor::PersistenceError error;
        if (!hcs::editor::create_empty_component_definition(id, name, "Custom", definition, error)) {
            set_status("Create empty component failed: " + error.describe());
            return;
        }
        begin_definition_edit(std::move(definition));
    }

    bool add_definition_port(hcs::editor::PinDirection direction) {
        if (!editing_definition_) return false;
        editing_definition_->implementation = circuit_;
        const std::string id_prefix = direction == hcs::editor::PinDirection::Input ? "input" : "output";
        const std::string name_prefix = direction == hcs::editor::PinDirection::Input ? "Input " : "Output ";
        auto& next_serial = direction == hcs::editor::PinDirection::Input
            ? next_input_port_serial_ : next_output_port_serial_;
        std::size_t ordinal = next_serial;
        std::string id;
        std::string name;
        const auto occupied = [&](std::size_t candidate) {
            const auto candidate_id = id_prefix + std::to_string(candidate);
            const auto candidate_name = name_prefix + std::to_string(candidate);
            return std::any_of(editing_definition_->ports.begin(), editing_definition_->ports.end(),
                [&](const hcs::editor::CustomPort& port) {
                    return port.id == candidate_id ||
                           (port.direction == direction && port.name == candidate_name);
                });
        };
        while (occupied(ordinal)) {
            if (ordinal == std::numeric_limits<std::size_t>::max()) {
                set_status("No generated port ID remains in this editing session.");
                return false;
            }
            ++ordinal;
        }
        if (ordinal == std::numeric_limits<std::size_t>::max()) {
            set_status("No generated port ID remains in this editing session.");
            return false;
        }
        id = id_prefix + std::to_string(ordinal);
        name = name_prefix + std::to_string(ordinal);
        hcs::editor::PersistenceError error;
        if (!hcs::editor::add_custom_component_port(*editing_definition_, id, name, direction, error)) {
            set_status("Add port failed: " + error.describe());
            return false;
        }
        next_serial = ordinal + 1U;
        circuit_ = editing_definition_->implementation;
        definition_metadata_dirty_ = true;
        const auto terminal = editing_definition_->ports.back().terminal_component;
        circuit_.select_component(terminal);
        runtime_.invalidate();
        try { runtime_.synchronize(circuit_, library_); }
        catch (const std::exception& exception) {
            set_status("Port added; runtime rebuild warning: " + std::string(exception.what()));
            return true;
        }
        set_status("Added " + name + ". Connect its pin to the internal circuit.");
        return true;
    }

    void begin_selected_port_rename() {
        if (!editing_definition_) return;
        const auto& selected = circuit_.selected_components();
        if (selected.empty()) { set_status("Select an external Input/Output terminal first."); return; }
        const auto id = selected.front();
        const auto found = std::find_if(editing_definition_->ports.begin(), editing_definition_->ports.end(),
            [&](const hcs::editor::CustomPort& port) { return port.terminal_component == id; });
        if (found == editing_definition_->ports.end()) { set_status("The selected component is not an external port terminal."); return; }
        pending_port_id_ = found->id;
        begin_path_entry(PathMode::PortName);
    }

    void edit_selected_definition() {
        if (editing_definition_) return;
        const auto& selected = circuit_.selected_components();
        if (selected.empty()) { set_status("Select a custom component instance first."); return; }
        const auto* component = circuit_.find_component(selected.front());
        if (!component || component->type != hcs::editor::ComponentType::Custom) {
            set_status("The selected component is not a custom component instance.");
            return;
        }
        begin_definition_edit(component->definition_id);
    }

    bool save_definition_edit() {
        if (!editing_definition_ || !root_before_definition_edit_) return false;
        if (symbol_text_mode_ && symbol_text_buffer_dirty_ && !apply_symbol_text()) return false;
        if (symbol_text_buffer_dirty_) { set_status("Apply or reload the pending symbol text before saving."); return false; }
        auto candidate_definition = *editing_definition_;
        candidate_definition.implementation = circuit_;
        const bool logic_changed = [&] {
            auto probe = library_;
            hcs::editor::PersistenceError probe_error;
            if (!probe.replace(candidate_definition, probe_error)) return true;
            return probe.logic_revision() != library_.logic_revision();
        }();
        auto candidate_library = library_;
        auto candidate_root = *root_before_definition_edit_;
        hcs::editor::PersistenceError error;
        if (!hcs::editor::apply_component_definition_transactionally(
                candidate_library, candidate_root, std::move(candidate_definition), error)) {
            set_status("Definition update rejected: " + error.describe());
            return false;
        }
        if (!candidate_library.save_definition(hcs::editor::ComponentLibrary::default_directory(),
                                               editing_definition_->id, error)) {
            set_status("Definition is valid but could not be saved: " + error.describe());
            return false;
        }

        library_ = std::move(candidate_library);
        circuit_ = std::move(candidate_root);
        document_name_ = root_document_name_before_definition_edit_;
        current_path_ = root_current_path_before_definition_edit_;
        saved_document_revision_ = circuit_.document_revision();
        text_buffer_ = root_text_buffer_before_definition_edit_;
        text_buffer_dirty_ = root_text_buffer_dirty_before_definition_edit_;
        text_mode_ = false;
        symbol_designer_mode_ = false;
        symbol_text_mode_ = false;
        editing_definition_.reset();
        root_before_definition_edit_.reset();
        definition_metadata_dirty_ = false;
        pending_port_id_.clear();
        symbol_designer_mode_ = false;
        symbol_text_mode_ = false;
        symbol_text_buffer_dirty_ = false;
        if (logic_changed) runtime_.invalidate();
        try {
            runtime_.synchronize(circuit_, library_);
            if (logic_changed) {
                controller_.reset(circuit_, library_);
                controller_.pause();
            }
        } catch (const std::exception& exception) {
            set_status("Definition saved, but runtime synchronization failed: " + std::string(exception.what()));
            return false;
        }
        set_status("Saved custom component definition. Connected instances were migrated by stable port ID.");
        return true;
    }

    void cancel_definition_edit() {
        if (!editing_definition_ || !root_before_definition_edit_) return;
        circuit_ = std::move(*root_before_definition_edit_);
        root_before_definition_edit_.reset();
        document_name_ = root_document_name_before_definition_edit_;
        current_path_ = root_current_path_before_definition_edit_;
        saved_document_revision_ = root_saved_revision_before_definition_edit_;
        text_buffer_ = root_text_buffer_before_definition_edit_;
        text_buffer_dirty_ = root_text_buffer_dirty_before_definition_edit_;
        text_mode_ = false;
        symbol_designer_mode_ = false;
        symbol_text_mode_ = false;
        editing_definition_.reset();
        definition_metadata_dirty_ = false;
        pending_port_id_.clear();
        runtime_.invalidate();
        try {
            runtime_.synchronize(circuit_, library_);
            controller_.reset(circuit_, library_);
            controller_.pause();
        } catch (const std::exception& exception) {
            set_status("Definition edit cancelled; root runtime rebuild failed: " + std::string(exception.what()));
            return;
        }
        set_status("Cancelled custom component definition editing.");
    }

    void remove_definition_selection() {
        if (!editing_definition_) return;
        const auto selected = circuit_.selected_components();
        editing_definition_->implementation = circuit_;
        bool changed = false;
        for (const auto id : selected) {
            const auto port = std::find_if(editing_definition_->ports.begin(), editing_definition_->ports.end(),
                [&](const hcs::editor::CustomPort& item) { return item.terminal_component == id; });
            if (port != editing_definition_->ports.end()) {
                const auto port_id = port->id;
                hcs::editor::PersistenceError error;
                if (!hcs::editor::remove_custom_component_port(*editing_definition_, port_id, error)) {
                    set_status("Remove port failed: " + error.describe());
                    return;
                }
                changed = true;
            } else {
                changed = editing_definition_->implementation.remove_component(id) || changed;
            }
        }
        if (selected.empty() && circuit_.selection().wire) {
            changed = editing_definition_->implementation.remove_wire(*circuit_.selection().wire);
        }
        if (!changed) return;
        circuit_ = editing_definition_->implementation;
        definition_metadata_dirty_ = true;
        runtime_.invalidate();
        try { runtime_.synchronize(circuit_, library_); }
        catch (const std::exception& exception) { set_status("Definition changed; runtime rebuild warning: " + std::string(exception.what())); }
    }

    void create_custom_from_selection(const std::string& name) {
        if (circuit_.selected_components().empty()) {
            set_status("Select one or more components first (Ctrl/Shift-click to build a selection).");
            return;
        }
        hcs::editor::CustomComponentDefinition definition;
        definition.name = name;
        definition.id = hcs::editor::make_definition_id(name);
        for (std::uint32_t suffix = 2; library_.find(definition.id); ++suffix)
            definition.id = hcs::editor::make_definition_id(name) + "-" + std::to_string(suffix);
        definition.description = "User-created component extracted from a circuit selection";
        definition.category = "Custom";
        ComponentId created = hcs::editor::kInvalidComponent;
        hcs::editor::PersistenceError error;
        const auto selected = circuit_.selected_components();
        if (!hcs::editor::create_custom_component_from_selection(circuit_, selected, std::move(definition), library_, created, error)) {
            set_status("Create custom component failed: " + error.describe());
            return;
        }
        if (!library_.save_definition(hcs::editor::ComponentLibrary::default_directory(),
                                      circuit_.find_component(created)->definition_id, error)) {
            set_status("Component created in memory, but library save failed: " + error.describe());
        } else {
            set_status("Created and saved custom component '" + name + "'.");
        }
        const auto definition_id = circuit_.find_component(created)->definition_id;
        begin_definition_edit(definition_id);
    }

    bool save_to_path(std::filesystem::path path) {
        if (editing_definition_) { set_status("Use SAVE DEF to save the component definition."); return false; }
        if (path.extension().empty()) path += std::string(hcs::editor::kCircuitFileExtension);
        hcs::editor::PersistenceError error;
        if (!hcs::editor::save_circuit_file(path, circuit_, document_name_, library_, error)) {
            set_status("Save failed: " + error.describe());
            return false;
        }
        current_path_ = std::move(path);
        saved_document_revision_ = circuit_.document_revision();
        set_status("Saved " + current_path_.string());
        return true;
    }

    void save_current_or_prompt() {
        if (editing_definition_) {
            if (text_mode_ && text_buffer_dirty_ && !apply_text()) return;
            (void)save_definition_edit();
            return;
        }
        if (text_mode_ && text_buffer_dirty_) {
            text_error_ = "Apply the edited text before saving the circuit.";
            return;
        }
        if (current_path_.empty()) begin_path_entry(PathMode::SaveAs);
        else (void)save_to_path(current_path_);
    }

    void finish_path_entry() {
        if (path_buffer_.empty()) {
            set_status("A file path is required.");
            return;
        }
        const auto mode = path_mode_;
        const std::filesystem::path path(path_buffer_);
        path_mode_ = PathMode::None;
        SDL_StopTextInput(window_);
        path_buffer_.clear();
        if (mode == PathMode::DefinitionName) {
            create_custom_from_selection(path.string());
        } else if (mode == PathMode::NewDefinitionName) {
            create_empty_definition(path.string());
        } else if (mode == PathMode::EditDefinitionName && editing_definition_) {
            editing_definition_->name = path.string();
            document_name_ = editing_definition_->name;
            definition_metadata_dirty_ = true;
            set_status("Definition name updated. Stable definition ID was preserved.");
        } else if (mode == PathMode::EditDefinitionDescription && editing_definition_) {
            editing_definition_->description = path.string();
            definition_metadata_dirty_ = true;
            set_status("Definition description updated.");
        } else if (mode == PathMode::EditDefinitionCategory && editing_definition_) {
            editing_definition_->category = path.string();
            definition_metadata_dirty_ = true;
            set_status("Definition category updated.");
        } else if (mode == PathMode::PortName && editing_definition_) {
            hcs::editor::PersistenceError error;
            if (hcs::editor::rename_custom_component_port(*editing_definition_, pending_port_id_, path.string(), error)) {
                definition_metadata_dirty_ = true;
                set_status("Port display name updated; its stable port ID and connections are unchanged.");
            } else set_status("Rename port failed: " + error.describe());
            pending_port_id_.clear();
        } else if (mode == PathMode::SymbolObjectText && editing_definition_ && selected_symbol_object_id_) {
            for (auto& object : editing_definition_->symbol.objects)
                if (auto* value = std::get_if<hcs::editor::SymbolText>(&object); value && value->id == *selected_symbol_object_id_) {
                    value->value = path.string(); mark_symbol_changed(); set_status("Symbol text object updated."); break;
                }
        } else if (mode == PathMode::SymbolPortLabel && editing_definition_ && selected_symbol_port_id_) {
            if (hcs::editor::set_symbol_port_label(editing_definition_->symbol,*selected_symbol_port_id_,path.string(),true)) {
                mark_symbol_changed(); set_status("Visual port label updated; logical port name and ID are unchanged.");
            }
        } else if (mode == PathMode::Open) {
            hcs::editor::PersistenceError error;
            std::string loaded_name;
            if (!hcs::editor::load_circuit_file(path, circuit_, loaded_name, library_, error)) {
                set_status("Open failed: " + error.describe());
                return;
            }
            current_path_ = path;
            document_name_ = std::move(loaded_name);
            text_buffer_.clear();
            text_buffer_dirty_ = false;
            placement_.reset();
            wire_start_.reset();
            circuit_.clear_selection();
            runtime_.synchronize(circuit_, library_);
            controller_.reset(circuit_, library_);
            controller_.pause();
            saved_document_revision_ = circuit_.document_revision();
            set_status("Opened " + current_path_.string());
        } else if (mode == PathMode::SaveAs) {
            auto normalized_path = path;
            if (normalized_path.extension().empty()) normalized_path += std::string(hcs::editor::kCircuitFileExtension);
            std::error_code exists_error;
            const bool exists = std::filesystem::exists(normalized_path, exists_error);
            if (exists_error) { set_status("Could not inspect output path: " + exists_error.message()); return; }
            if (exists && normalized_path != current_path_) {
                overwrite_path_ = normalized_path;
                overwrite_confirmation_ = true;
                return;
            }
            complete_save_as(normalized_path);
        }
    }

    void complete_save_as(const std::filesystem::path& path) {
        const auto continuation = after_save_action_;
        if (save_to_path(path)) {
            after_save_action_ = DeferredAction::None;
            if (continuation != DeferredAction::None) execute_action(continuation);
        }
    }

    SymbolCanvas symbol_canvas(int width, int height) const {
        SymbolCanvas canvas;
        canvas.rect = {198.0F, 112.0F, std::max(280.0F, static_cast<float>(width) - 420.0F),
                       std::max(220.0F, static_cast<float>(height) - 190.0F)};
        if (!editing_definition_) return canvas;
        const auto& size = editing_definition_->symbol.size;
        const float fit_x = std::max(1.0F, canvas.rect.width - 64.0F) / std::max(1.0F, size.x);
        const float fit_y = std::max(1.0F, canvas.rect.height - 64.0F) / std::max(1.0F, size.y);
        canvas.zoom = std::clamp(std::min(fit_x, fit_y) * symbol_zoom_factor_, 0.25F, 6.0F);
        const auto transformed = size;
        canvas.origin = {canvas.rect.x + (canvas.rect.width - transformed.x * canvas.zoom) * 0.5F + symbol_pan_offset_.x,
                         canvas.rect.y + (canvas.rect.height - transformed.y * canvas.zoom) * 0.5F + symbol_pan_offset_.y};
        return canvas;
    }

    Vec2 symbol_screen_to_local(Vec2 screen) const {
        int width = kInitialWidth, height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        const auto canvas = symbol_canvas(width, height);
        return {(screen.x - canvas.origin.x) / canvas.zoom,
                (screen.y - canvas.origin.y) / canvas.zoom};
    }

    bool inside_symbol_canvas(Vec2 screen) const {
        int width = kInitialWidth, height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        const auto rect = symbol_canvas(width, height).rect;
        return screen.x >= rect.x && screen.y >= rect.y &&
               screen.x <= rect.x + rect.width && screen.y <= rect.y + rect.height;
    }

    Vec2 snap_symbol_point(Vec2 point) const {
        if (!symbol_snap_ || !editing_definition_) return point;
        constexpr float grid = 5.0F;
        point.x = std::round(point.x / grid) * grid;
        point.y = std::round(point.y / grid) * grid;
        point.x = std::clamp(point.x, 0.0F, editing_definition_->symbol.size.x);
        point.y = std::clamp(point.y, 0.0F, editing_definition_->symbol.size.y);
        return point;
    }

    std::string next_symbol_object_id() const {
        if (!editing_definition_) return "object";
        std::uint32_t suffix = 1;
        for (;;) {
            const auto candidate = "object_" + std::to_string(suffix++);
            const bool used = std::any_of(editing_definition_->symbol.objects.begin(), editing_definition_->symbol.objects.end(),
                [&](const hcs::editor::SymbolObject& object) {
                    return std::visit([&](const auto& item) { return item.id == candidate; }, object);
                });
            if (!used) return candidate;
        }
    }

    void mark_symbol_changed() {
        if (!editing_definition_) return;
        editing_definition_->size = editing_definition_->symbol.size;
        definition_metadata_dirty_ = true;
        update_window_title();
    }

    void open_symbol_designer() {
        if (!editing_definition_) return;
        symbol_designer_mode_ = true;
        symbol_text_mode_ = false;
        symbol_tool_ = SymbolTool::Select;
        selected_symbol_object_id_.reset();
        selected_symbol_port_id_.reset();
        selected_symbol_vertex_index_.reset();
        symbol_vertex_edit_mode_=false;
        symbol_pending_points_.clear();
        symbol_drag_ = SymbolDrag::None;
        library_panel_ = false;
        controller_.pause();
        set_status("Symbol Designer: choose a tool, draw on the canvas, drag ports, or open SYMBOL TEXT.");
    }

    void close_symbol_designer() {
        if (symbol_text_buffer_dirty_) {
            set_status("Unapplied symbol text is preserved. Apply or reload it before leaving the definition editor.");
            return;
        }
        symbol_designer_mode_ = false;
        symbol_drag_ = SymbolDrag::None;
        symbol_pending_points_.clear();
        set_status("Returned to the component logic editor.");
    }

    void enter_symbol_text_mode() {
        if (!editing_definition_) return;
        if (!symbol_text_buffer_dirty_) reload_symbol_text();
        symbol_text_mode_ = true;
        SDL_StartTextInput(window_);
    }

    void leave_symbol_text_mode() {
        symbol_text_mode_ = false;
        SDL_StopTextInput(window_);
        if (symbol_text_buffer_dirty_)
            set_status("Unapplied symbol text is preserved. Apply it or use RELOAD SYMBOL to discard it.");
    }

    void reload_symbol_text() {
        if (!editing_definition_) return;
        hcs::editor::SymbolError error;
        std::string generated;
        if (!hcs::editor::serialize_symbol(editing_definition_->symbol, generated, error)) {
            symbol_text_error_ = "Cannot serialize symbol: " + error.describe();
            return;
        }
        symbol_text_buffer_ = std::move(generated);
        symbol_text_cursor_ = symbol_text_buffer_.size();
        symbol_text_selection_anchor_.reset();
        symbol_text_buffer_dirty_ = false;
        symbol_text_error_.clear();
    }

    bool apply_symbol_text() {
        if (!editing_definition_) return false;
        hcs::editor::SymbolDefinition candidate;
        hcs::editor::SymbolError error;
        if (!hcs::editor::parse_symbol(symbol_text_buffer_, candidate, error)) {
            symbol_text_error_ = "Apply failed: " + error.describe();
            return false;
        }
        std::set<std::string> expected, actual;
        for (const auto& port : editing_definition_->ports) expected.insert(port.id);
        for (const auto& port : candidate.ports) actual.insert(port.port_id);
        if (expected != actual) {
            symbol_text_error_ = "Apply failed: visual port IDs must exactly match the logical component port IDs.";
            return false;
        }
        editing_definition_->symbol = std::move(candidate);
        editing_definition_->size = editing_definition_->symbol.size;
        definition_metadata_dirty_ = true;
        symbol_text_buffer_dirty_ = false;
        symbol_text_error_.clear();
        reload_symbol_text();
        set_status("Applied symbol text. The logical circuit and stable port IDs were preserved.");
        return true;
    }

    void insert_symbol_text(std::string_view value) {
        if (value.empty() || symbol_text_buffer_.size() + value.size() > 4U * 1024U * 1024U) return;
        if (symbol_text_selection_anchor_) {
            const auto first = std::min(*symbol_text_selection_anchor_, symbol_text_cursor_);
            const auto last = std::max(*symbol_text_selection_anchor_, symbol_text_cursor_);
            symbol_text_buffer_.erase(first, last - first);
            symbol_text_cursor_ = first;
            symbol_text_selection_anchor_.reset();
        }
        symbol_text_buffer_.insert(symbol_text_cursor_, value);
        symbol_text_cursor_ += value.size();
        symbol_text_buffer_dirty_ = true;
        symbol_text_error_.clear();
    }

    std::size_t previous_symbol_text_position(std::size_t position) const {
        if (position == 0) return 0;
        --position;
        while (position > 0 && (static_cast<unsigned char>(symbol_text_buffer_[position]) & 0xC0U) == 0x80U) --position;
        return position;
    }

    std::size_t next_symbol_text_position(std::size_t position) const {
        if (position >= symbol_text_buffer_.size()) return symbol_text_buffer_.size();
        ++position;
        while (position < symbol_text_buffer_.size() &&
               (static_cast<unsigned char>(symbol_text_buffer_[position]) & 0xC0U) == 0x80U) ++position;
        return position;
    }

    void handle_symbol_text_key(const SDL_KeyboardEvent& event, bool control, bool shift) {
        if (control && event.key == SDLK_S) { (void)apply_symbol_text(); return; }
        if (control && event.key == SDLK_R) { reload_symbol_text(); return; }
        if (control && event.key == SDLK_T) { leave_symbol_text_mode(); return; }
        if (control && event.key == SDLK_A) { symbol_text_selection_anchor_ = 0; symbol_text_cursor_ = symbol_text_buffer_.size(); return; }
        if (control && (event.key == SDLK_C || event.key == SDLK_X)) {
            if (symbol_text_selection_anchor_) {
                const auto first = std::min(*symbol_text_selection_anchor_, symbol_text_cursor_);
                const auto last = std::max(*symbol_text_selection_anchor_, symbol_text_cursor_);
                const auto value = symbol_text_buffer_.substr(first, last - first);
                (void)SDL_SetClipboardText(value.c_str());
                if (event.key == SDLK_X) {
                    symbol_text_buffer_.erase(first, last - first); symbol_text_cursor_ = first;
                    symbol_text_selection_anchor_.reset(); symbol_text_buffer_dirty_ = true;
                }
            }
            return;
        }
        if (control && event.key == SDLK_V) {
            char* clipboard = SDL_GetClipboardText();
            if (clipboard) { insert_symbol_text(clipboard); SDL_free(clipboard); }
            return;
        }
        if (event.key == SDLK_ESCAPE) { leave_symbol_text_mode(); return; }
        if (event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) { insert_symbol_text("\n"); return; }
        if (event.key == SDLK_TAB) { insert_symbol_text("  "); return; }
        if (!shift && symbol_text_selection_anchor_ && (event.key == SDLK_LEFT || event.key == SDLK_RIGHT ||
            event.key == SDLK_HOME || event.key == SDLK_END)) {
            symbol_text_cursor_ = event.key == SDLK_LEFT || event.key == SDLK_HOME
                ? std::min(*symbol_text_selection_anchor_, symbol_text_cursor_)
                : std::max(*symbol_text_selection_anchor_, symbol_text_cursor_);
            symbol_text_selection_anchor_.reset();
        }
        if (shift && !symbol_text_selection_anchor_) symbol_text_selection_anchor_ = symbol_text_cursor_;
        switch (event.key) {
            case SDLK_BACKSPACE:
            case SDLK_DELETE: {
                if (symbol_text_selection_anchor_) {
                    const auto first = std::min(*symbol_text_selection_anchor_, symbol_text_cursor_);
                    const auto last = std::max(*symbol_text_selection_anchor_, symbol_text_cursor_);
                    symbol_text_buffer_.erase(first, last-first); symbol_text_cursor_ = first;
                    symbol_text_selection_anchor_.reset(); symbol_text_buffer_dirty_ = true;
                } else if (event.key == SDLK_BACKSPACE && symbol_text_cursor_ > 0) {
                    const auto previous = previous_symbol_text_position(symbol_text_cursor_);
                    symbol_text_buffer_.erase(previous, symbol_text_cursor_ - previous);
                    symbol_text_cursor_ = previous; symbol_text_buffer_dirty_ = true;
                } else if (event.key == SDLK_DELETE && symbol_text_cursor_ < symbol_text_buffer_.size()) {
                    symbol_text_buffer_.erase(symbol_text_cursor_, next_symbol_text_position(symbol_text_cursor_) - symbol_text_cursor_);
                    symbol_text_buffer_dirty_ = true;
                }
                break;
            }
            case SDLK_LEFT: symbol_text_cursor_ = previous_symbol_text_position(symbol_text_cursor_); break;
            case SDLK_RIGHT: symbol_text_cursor_ = next_symbol_text_position(symbol_text_cursor_); break;
            case SDLK_HOME: {
                const auto pos = symbol_text_buffer_.rfind('\n', symbol_text_cursor_ == 0 ? 0 : symbol_text_cursor_-1);
                symbol_text_cursor_ = pos == std::string::npos ? 0 : pos + 1; break;
            }
            case SDLK_END: {
                const auto pos = symbol_text_buffer_.find('\n', symbol_text_cursor_);
                symbol_text_cursor_ = pos == std::string::npos ? symbol_text_buffer_.size() : pos; break;
            }
            default: break;
        }
    }

    void begin_symbol_object_text_edit() {
        if (!editing_definition_) return;
        if (selected_symbol_object_id_) {
            const auto found = std::find_if(editing_definition_->symbol.objects.begin(), editing_definition_->symbol.objects.end(),
                [&](const hcs::editor::SymbolObject& object) {
                    return std::visit([&](const auto& item) { return item.id == *selected_symbol_object_id_; }, object);
                });
            if (found != editing_definition_->symbol.objects.end()) {
                if (const auto* text = std::get_if<hcs::editor::SymbolText>(&*found)) begin_path_entry(PathMode::SymbolObjectText), path_buffer_ = text->value;
                else set_status("The selected object is not a text object.");
                return;
            }
        }
        if (selected_symbol_port_id_) {
            const auto port = std::find_if(editing_definition_->symbol.ports.begin(), editing_definition_->symbol.ports.end(),
                [&](const auto& item) { return item.port_id == *selected_symbol_port_id_; });
            if (port != editing_definition_->symbol.ports.end()) {
                begin_path_entry(PathMode::SymbolPortLabel);
                path_buffer_ = port->label;
                return;
            }
        }
        set_status("Select a text object or port before pressing E.");
    }

    void finish_symbol_path() {
        if (!editing_definition_ || symbol_pending_points_.size() < (symbol_tool_ == SymbolTool::Polygon ? 3U : 2U)) {
            symbol_pending_points_.clear();
            return;
        }
        const auto id = next_symbol_object_id();
        if (symbol_tool_ == SymbolTool::Path) {
            hcs::editor::SymbolPath path;
            path.id = id;
            path.points = symbol_pending_points_;
            editing_definition_->symbol.objects.emplace_back(std::move(path));
        } else {
            hcs::editor::SymbolShape shape;
            shape.id = id;
            shape.type = symbol_tool_ == SymbolTool::Polygon ? hcs::editor::SymbolShapeType::Polygon : hcs::editor::SymbolShapeType::Polyline;
            shape.points = symbol_pending_points_;
            if (shape.type == hcs::editor::SymbolShapeType::Polyline) shape.fill = "none";
            editing_definition_->symbol.objects.emplace_back(std::move(shape));
        }
        selected_symbol_object_id_ = id;
        selected_symbol_port_id_.reset();
        symbol_pending_points_.clear();
        mark_symbol_changed();
        set_status("Added vector path. Drag its contour vertices in the text source for exact editing; object can be moved as a whole.");
    }

    void create_symbol_shape_from_drag(Vec2 a, Vec2 b) {
        if (!editing_definition_) return;
        a = snap_symbol_point(a); b = snap_symbol_point(b);
        hcs::editor::SymbolShape shape;
        shape.id = next_symbol_object_id();
        shape.x = std::min(a.x, b.x); shape.y = std::min(a.y, b.y);
        shape.width = std::max(1.0F, std::abs(b.x - a.x));
        shape.height = std::max(1.0F, std::abs(b.y - a.y));
        switch (symbol_tool_) {
            case SymbolTool::Rectangle: shape.type = hcs::editor::SymbolShapeType::Rectangle; break;
            case SymbolTool::RoundedRectangle: shape.type = hcs::editor::SymbolShapeType::RoundedRectangle; shape.radius = 10.0F; break;
            case SymbolTool::Circle: {
                shape.type = hcs::editor::SymbolShapeType::Circle;
                const auto symbol_size = editing_definition_->symbol.size;
                const float diameter = std::clamp(std::max(shape.width, shape.height), 1.0F,
                    std::min(symbol_size.x, symbol_size.y));
                const float center_x = std::clamp((a.x + b.x) * 0.5F,
                    diameter * 0.5F, symbol_size.x - diameter * 0.5F);
                const float center_y = std::clamp((a.y + b.y) * 0.5F,
                    diameter * 0.5F, symbol_size.y - diameter * 0.5F);
                shape.width = diameter; shape.height = diameter;
                shape.x = center_x - diameter * 0.5F; shape.y = center_y - diameter * 0.5F;
                break;
            }
            case SymbolTool::Ellipse: shape.type = hcs::editor::SymbolShapeType::Ellipse; break;
            case SymbolTool::Line:
                shape.type = hcs::editor::SymbolShapeType::Line;
                shape.fill = "none";
                // Store both endpoints explicitly so negative slopes are preserved.
                shape.x = a.x; shape.y = a.y; shape.width = 0.0F; shape.height = 0.0F;
                shape.points = {{a.x, a.y}, {b.x, b.y}};
                break;
            case SymbolTool::Arc: shape.type = hcs::editor::SymbolShapeType::Arc; shape.fill = "none"; shape.start_angle = 0.0F; shape.end_angle = 180.0F; break;
            default: return;
        }
        editing_definition_->symbol.objects.emplace_back(shape);
        selected_symbol_object_id_ = shape.id;
        selected_symbol_port_id_.reset();
        mark_symbol_changed();
    }

    void add_symbol_text_at(Vec2 position) {
        if (!editing_definition_) return;
        hcs::editor::SymbolText text;
        text.id = next_symbol_object_id();
        text.value = "Label";
        text.x = position.x; text.y = position.y;
        selected_symbol_object_id_ = text.id;
        selected_symbol_port_id_.reset();
        editing_definition_->symbol.objects.emplace_back(std::move(text));
        mark_symbol_changed();
        begin_path_entry(PathMode::SymbolObjectText);
        path_buffer_ = "Label";
    }

    void adjust_selected_symbol_stroke(float delta) {
        if (!editing_definition_ || !selected_symbol_object_id_) return;
        auto candidate = editing_definition_->symbol;
        bool changed = false;
        for (auto& object : candidate.objects) {
            std::visit([&](auto& item) {
                using T = std::decay_t<decltype(item)>;
                if (item.id != *selected_symbol_object_id_) return;
                if constexpr (std::is_same_v<T, hcs::editor::SymbolShape> ||
                              std::is_same_v<T, hcs::editor::SymbolPath>) {
                    item.stroke_width = std::clamp(item.stroke_width + delta, 0.0F, 64.0F);
                    changed = true;
                }
            }, object);
            if (changed) break;
        }
        hcs::editor::SymbolError error;
        if (changed && hcs::editor::validate_symbol(candidate, error)) {
            editing_definition_->symbol = std::move(candidate);
            mark_symbol_changed();
        } else if (changed) set_status("Stroke width could not be applied: " + error.describe());
    }

    void adjust_selected_symbol_text_size(float delta) {
        if (!editing_definition_ || !selected_symbol_object_id_) return;
        auto candidate = editing_definition_->symbol;
        bool changed = false;
        for (auto& object : candidate.objects) {
            if (auto* text = std::get_if<hcs::editor::SymbolText>(&object);
                text && text->id == *selected_symbol_object_id_) {
                text->size = std::clamp(text->size + delta, 4.0F, 128.0F);
                changed = true;
                break;
            }
        }
        hcs::editor::SymbolError error;
        if (changed && hcs::editor::validate_symbol(candidate, error)) {
            editing_definition_->symbol = std::move(candidate);
            mark_symbol_changed();
        } else if (changed) set_status("Text size could not be applied within the symbol bounds.");
    }

    hcs::editor::SymbolPortOrientation symbol_port_orientation(Vec2 point) const {
        const auto size = editing_definition_->symbol.size;
        const float distances[] = {std::abs(point.x), std::abs(size.x-point.x), std::abs(point.y), std::abs(size.y-point.y)};
        const auto found = static_cast<int>(std::min_element(std::begin(distances), std::end(distances)) - std::begin(distances));
        switch (found) {
            case 0: return hcs::editor::SymbolPortOrientation::Left;
            case 1: return hcs::editor::SymbolPortOrientation::Right;
            case 2: return hcs::editor::SymbolPortOrientation::Top;
            default: return hcs::editor::SymbolPortOrientation::Bottom;
        }
    }

    void apply_symbol_preset() {
        if (!editing_definition_) return;
        static const std::array<std::string_view, 10> presets{
            "rectangle", "rounded_rect", "circle", "triangle", "and", "or", "xor", "not", "dff", "mux"};
        symbol_preset_index_ = (symbol_preset_index_ + 1U) % presets.size();
        std::vector<std::pair<std::string,std::string>> ports;
        for (const auto& port : editing_definition_->ports)
            ports.emplace_back(port.id, port.direction == hcs::editor::PinDirection::Input ? "input" : "output");
        auto symbol = hcs::editor::make_symbol_preset(presets[symbol_preset_index_], editing_definition_->symbol.size, ports);
        for (auto& visual : symbol.ports) {
            const auto old = std::find_if(editing_definition_->symbol.ports.begin(), editing_definition_->symbol.ports.end(),
                [&](const auto& item) { return item.port_id == visual.port_id; });
            if (old != editing_definition_->symbol.ports.end()) visual.label = old->label;
        }
        editing_definition_->symbol = std::move(symbol);
        selected_symbol_object_id_.reset(); selected_symbol_port_id_.reset();
        mark_symbol_changed();
        set_status("Applied editable symbol preset " + std::string(presets[symbol_preset_index_]) + ".");
    }

    void render_symbol_text_editor() {
        int width = kInitialWidth, height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        SDL_SetRenderDrawColor(renderer_, 13, 18, 25, 255);
        (void)SDL_RenderClear(renderer_);
        CircuitRenderer::fill_rect(renderer_, 0, 0, static_cast<float>(width), kToolbarHeight, kToolbar);
        for (const auto& button : buttons()) render_button(button);
        CircuitRenderer::text(renderer_, 390, 17, "SYMBOL SOURCE  |  HCSS 1  |  CTRL+S APPLY  CTRL+R RELOAD  CTRL+T VISUAL", 0.78F, kMuted);
        CircuitRenderer::text(renderer_, 16, 86, (editing_definition_ ? editing_definition_->name : "Symbol") .c_str(), 0.9F, kLogicOne);
        if (!symbol_text_error_.empty()) CircuitRenderer::text(renderer_, 16, 106, symbol_text_error_.c_str(), 0.72F, SDL_Color{255,128,128,255});
        std::size_t cursor_line = 0, cursor_start = 0;
        for (std::size_t i=0; i<symbol_text_cursor_ && i<symbol_text_buffer_.size(); ++i)
            if (symbol_text_buffer_[i]=='\n') { ++cursor_line; cursor_start=i+1; }
        const auto visible_lines = static_cast<std::size_t>(std::max(1,(height-180)/17));
        const auto first_line = cursor_line >= visible_lines ? cursor_line-visible_lines+1 : 0;
        std::size_t line_number=0, line_start=0;
        float y=130;
        while (line_start<=symbol_text_buffer_.size() && y<static_cast<float>(height)-35) {
            const auto nl=symbol_text_buffer_.find('\n',line_start);
            const auto end=nl==std::string::npos?symbol_text_buffer_.size():nl;
            if (line_number>=first_line) {
                const auto number=std::to_string(line_number+1);
                CircuitRenderer::text(renderer_,16,y,number.c_str(),0.72F,kMuted);
                const auto line=symbol_text_buffer_.substr(line_start,end-line_start);
                CircuitRenderer::text(renderer_,64,y,line.c_str(),0.82F,kText,hcs::gui::FontRole::Code);
                if (line_number==cursor_line && symbol_text_cursor_-cursor_start<=line.size()) {
                    const auto prefix=line.substr(0,symbol_text_cursor_-cursor_start);
                    const auto extent=CircuitRenderer::text_size(prefix.c_str(),0.82F,hcs::gui::FontRole::Code);
                    CircuitRenderer::line(renderer_,{64+extent.x,y-1},{64+extent.x,y+14},kLogicOne);
                }
                y+=17;
            }
            if (nl==std::string::npos) break;
            line_start=nl+1; ++line_number;
        }
        CircuitRenderer::fill_rect(renderer_,0,static_cast<float>(height)-38,static_cast<float>(width),38,kToolbar);
        CircuitRenderer::text(renderer_,12,static_cast<float>(height)-26,
            symbol_text_buffer_dirty_?"UNAPPLIED SYMBOL SOURCE CHANGES":"SYMBOL SOURCE SYNCHRONIZED",0.7F,
            symbol_text_buffer_dirty_?kLogicOne:kMuted);
        SDL_RenderPresent(renderer_);
    }

    const std::vector<hcs::editor::SymbolPoint>* selected_symbol_points() const {
        if (!editing_definition_ || !selected_symbol_object_id_) return nullptr;
        for (const auto& object : editing_definition_->symbol.objects) {
            const auto points = std::visit([](const auto& item) -> const std::vector<hcs::editor::SymbolPoint>* {
                using T = std::decay_t<decltype(item)>;
                if constexpr (std::is_same_v<T, hcs::editor::SymbolText>) return nullptr;
                else return &item.points;
            }, object);
            const auto id = std::visit([](const auto& item) { return item.id; }, object);
            if (id == *selected_symbol_object_id_) return points;
        }
        return nullptr;
    }

    bool selected_symbol_object_supports_vertices() const {
        const auto* points = selected_symbol_points();
        return points && points->size() >= 2;
    }

    void draw_symbol_vertex_handles() {
        if (!symbol_vertex_edit_mode_ || !selected_symbol_object_id_ || !editing_definition_) return;
        const auto* points = selected_symbol_points();
        if (!points) return;
        int width=kInitialWidth,height=kInitialHeight;
        (void)SDL_GetWindowSize(window_,&width,&height);
        const auto canvas=symbol_canvas(width,height);
        for(std::size_t i=0;i<points->size();++i) {
            const float x=canvas.origin.x+(*points)[i].x*canvas.zoom;
            const float y=canvas.origin.y+(*points)[i].y*canvas.zoom;
            const float r=(selected_symbol_vertex_index_ && *selected_symbol_vertex_index_==i)?6.0F:4.0F;
            const SDL_Color color=(selected_symbol_vertex_index_ && *selected_symbol_vertex_index_==i)
                ? SDL_Color{255,190,82,255}:SDL_Color{100,190,255,255};
            CircuitRenderer::fill_rect(renderer_,x-r,y-r,r*2.0F,r*2.0F,color);
            CircuitRenderer::outline_rect(renderer_,x-r-1,y-r-1,r*2.0F+2,r*2.0F+2,SDL_Color{12,18,27,255});
        }
    }

    void draw_symbol_selection_handles() {
        if (!editing_definition_) return;
        int width=kInitialWidth,height=kInitialHeight;
        (void)SDL_GetWindowSize(window_,&width,&height);
        const auto canvas=symbol_canvas(width,height);
        if (selected_symbol_object_id_) {
            const auto bounds=symbol_object_bounds(*selected_symbol_object_id_);
            if (!bounds) return;
            const float x=canvas.origin.x+bounds->x*canvas.zoom;
            const float y=canvas.origin.y+bounds->y*canvas.zoom;
            const float w=std::max(1.0F,bounds->width*canvas.zoom);
            const float h=std::max(1.0F,bounds->height*canvas.zoom);
            const SDL_Color color{90,180,255,230};
            CircuitRenderer::outline_rect(renderer_,x-2.0F,y-2.0F,w+4.0F,h+4.0F,color);
            constexpr float handle=8.0F;
            CircuitRenderer::fill_rect(renderer_,x+w-handle*0.5F,y+h-handle*0.5F,handle,handle,
                                       SDL_Color{255,190,82,255});
            CircuitRenderer::outline_rect(renderer_,x+w-handle*0.5F-1.0F,y+h-handle*0.5F-1.0F,
                                          handle+2.0F,handle+2.0F,SDL_Color{12,18,27,255});
        } else if (selected_symbol_port_id_) {
            const auto found=std::find_if(editing_definition_->symbol.ports.begin(),editing_definition_->symbol.ports.end(),
                [&](const auto& port){return port.port_id==*selected_symbol_port_id_;});
            if (found==editing_definition_->symbol.ports.end()) return;
            const float x=canvas.origin.x+found->x*canvas.zoom;
            const float y=canvas.origin.y+found->y*canvas.zoom;
            CircuitRenderer::outline_rect(renderer_,x-7.0F,y-7.0F,14.0F,14.0F,SDL_Color{255,190,82,255});
            CircuitRenderer::outline_rect(renderer_,x-9.0F,y-9.0F,18.0F,18.0F,SDL_Color{12,18,27,255});
        }
    }

    void draw_symbol_creation_preview() {
        if (!editing_definition_) return;
        int width=kInitialWidth,height=kInitialHeight;
        (void)SDL_GetWindowSize(window_,&width,&height);
        const auto canvas=symbol_canvas(width,height);
        const SDL_Color preview{255,190,82,230};
        const auto screen_point=[&](const hcs::editor::SymbolPoint& point) {
            return Vec2{canvas.origin.x+point.x*canvas.zoom,canvas.origin.y+point.y*canvas.zoom};
        };
        if (!symbol_pending_points_.empty()) {
            for (std::size_t i=1;i<symbol_pending_points_.size();++i)
                CircuitRenderer::line(renderer_,screen_point(symbol_pending_points_[i-1]),
                                      screen_point(symbol_pending_points_[i]),preview);
            if (symbol_cursor_valid_) {
                const auto cursor=screen_point({symbol_cursor_local_.x,symbol_cursor_local_.y});
                CircuitRenderer::line(renderer_,screen_point(symbol_pending_points_.back()),cursor,preview);
                if (symbol_tool_==SymbolTool::Polygon && symbol_pending_points_.size()>=2)
                    CircuitRenderer::line(renderer_,cursor,screen_point(symbol_pending_points_.front()),SDL_Color{100,150,190,180});
            }
            for (const auto& point:symbol_pending_points_) {
                const auto p=screen_point(point);
                CircuitRenderer::fill_rect(renderer_,p.x-3.0F,p.y-3.0F,6.0F,6.0F,preview);
            }
        }
        if (symbol_drag_==SymbolDrag::Draw) {
            const auto a=screen_point({symbol_drag_start_.x,symbol_drag_start_.y});
            const auto b=screen_point({symbol_drag_last_.x,symbol_drag_last_.y});
            if (symbol_tool_==SymbolTool::Line) {
                CircuitRenderer::line(renderer_,a,b,preview);
            } else {
                const float x=std::min(a.x,b.x), y=std::min(a.y,b.y);
                const float w=std::max(1.0F,std::abs(b.x-a.x));
                const float h=std::max(1.0F,std::abs(b.y-a.y));
                CircuitRenderer::outline_rect(renderer_,x,y,w,h,preview);
            }
        }
    }

    void render_symbol_designer() {
        if (!editing_definition_) return;
        int width = kInitialWidth, height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        SDL_SetRenderDrawColor(renderer_, 15, 19, 27, 255);
        (void)SDL_RenderClear(renderer_);
        CircuitRenderer::fill_rect(renderer_,0,0,static_cast<float>(width),kToolbarHeight,kToolbar);
        CircuitRenderer::line(renderer_,{0,kToolbarHeight},{static_cast<float>(width),kToolbarHeight},kButtonBorder);
        for (const auto& button : buttons()) render_button(button);
        const auto canvas = symbol_canvas(width,height);
        CircuitRenderer::fill_rect(renderer_,canvas.rect.x,canvas.rect.y,canvas.rect.width,canvas.rect.height,SDL_Color{17,23,32,255});
        CircuitRenderer::outline_rect(renderer_,canvas.rect.x,canvas.rect.y,canvas.rect.width,canvas.rect.height,kButtonBorder);
        for (float x=canvas.rect.x; x<canvas.rect.x+canvas.rect.width; x+=20.0F)
            CircuitRenderer::line(renderer_,{x,canvas.rect.y},{x,canvas.rect.y+canvas.rect.height},SDL_Color{27,35,47,255});
        for (float y=canvas.rect.y; y<canvas.rect.y+canvas.rect.height; y+=20.0F)
            CircuitRenderer::line(renderer_,{canvas.rect.x,y},{canvas.rect.x+canvas.rect.width,y},SDL_Color{27,35,47,255});
        CircuitRenderer::outline_rect(renderer_,canvas.origin.x,canvas.origin.y,
            editing_definition_->symbol.size.x*canvas.zoom,editing_definition_->symbol.size.y*canvas.zoom,SDL_Color{65,83,105,255});
        hcs::gui::SymbolRenderer::draw(renderer_,*editing_definition_,canvas.origin,canvas.zoom,{},true);
        draw_symbol_creation_preview();
        draw_symbol_vertex_handles();
        draw_symbol_selection_handles();
        // Left object/port list.
        CircuitRenderer::fill_rect(renderer_,0,kToolbarHeight,188,static_cast<float>(height)-kToolbarHeight-kStatusHeight,SDL_Color{23,31,43,255});
        CircuitRenderer::text(renderer_,12,88,"OBJECTS",0.82F,kText);
        float row_y=112.0F;
        for (const auto& object:editing_definition_->symbol.objects) {
            const auto id=std::visit([](const auto& item){return item.id;},object);
            const auto kind=std::visit([](const auto& item)->std::string_view {
                using T=std::decay_t<decltype(item)>;
                if constexpr(std::is_same_v<T,hcs::editor::SymbolShape>) return hcs::editor::symbol_shape_name(item.type);
                if constexpr(std::is_same_v<T,hcs::editor::SymbolPath>) return "path";
                return "text";
            },object);
            const bool selected=selected_symbol_object_id_ && *selected_symbol_object_id_==id;
            if(selected) CircuitRenderer::fill_rect(renderer_,4,row_y-3,180,21,kButtonActive);
            const auto label=std::string(kind)+"  "+id;
            CircuitRenderer::text(renderer_,10,row_y,label.c_str(),0.62F,selected?kText:kMuted);
            row_y+=23;
            if(row_y>static_cast<float>(height)-220) break;
        }
        row_y=std::max(row_y+8,static_cast<float>(height)-190);
        CircuitRenderer::text(renderer_,12,row_y,"PORTS",0.82F,kText); row_y+=24;
        for(const auto& port:editing_definition_->symbol.ports) {
            const bool selected=selected_symbol_port_id_ && *selected_symbol_port_id_==port.port_id;
            if(selected) CircuitRenderer::fill_rect(renderer_,4,row_y-3,180,21,kButtonActive);
            const auto label=port.port_id+"  "+std::string(hcs::editor::symbol_orientation_name(port.orientation));
            CircuitRenderer::text(renderer_,10,row_y,label.c_str(),0.62F,selected?kText:kLogicOne);
            row_y+=23;
        }
        // Right property/help panel.
        const float right_x=static_cast<float>(width)-210.0F;
        CircuitRenderer::fill_rect(renderer_,right_x,kToolbarHeight,210,static_cast<float>(height)-kToolbarHeight-kStatusHeight,SDL_Color{23,31,43,255});
        CircuitRenderer::text(renderer_,right_x+12,88,"PROPERTIES",0.82F,kText);
        float info_y=116.0F;
        if(selected_symbol_port_id_) {
            const auto it=std::find_if(editing_definition_->symbol.ports.begin(),editing_definition_->symbol.ports.end(),
                [&](const auto& port){return port.port_id==*selected_symbol_port_id_;});
            if(it!=editing_definition_->symbol.ports.end()) {
                const auto name="PORT: "+it->port_id;
                CircuitRenderer::text(renderer_,right_x+12,info_y,name.c_str(),0.68F,kLogicOne); info_y+=20;
                const auto coords="POS: "+std::to_string(static_cast<int>(it->x))+", "+std::to_string(static_cast<int>(it->y));
                CircuitRenderer::text(renderer_,right_x+12,info_y,coords.c_str(),0.62F,kMuted); info_y+=20;
                CircuitRenderer::text(renderer_,right_x+12,info_y,"E: EDIT LABEL",0.62F,kText); info_y+=18;
                CircuitRenderer::text(renderer_,right_x+12,info_y,"O: CYCLE ORIENTATION",0.56F,kMuted); info_y+=18;
                CircuitRenderer::text(renderer_,right_x+12,info_y,"SHIFT+O: ANGLE",0.56F,kMuted);
            }
        } else if(selected_symbol_object_id_) {
            const auto selected=std::find_if(editing_definition_->symbol.objects.begin(),editing_definition_->symbol.objects.end(),
                [&](const auto& object){return std::visit([&](const auto& item){return item.id==*selected_symbol_object_id_;},object);});
            CircuitRenderer::text(renderer_,right_x+12,info_y,("OBJECT: "+*selected_symbol_object_id_).c_str(),0.65F,kLogicOne); info_y+=22;
            if(selected!=editing_definition_->symbol.objects.end()) {
                std::visit([&](const auto& item) {
                    using T=std::decay_t<decltype(item)>;
                    if constexpr(std::is_same_v<T,hcs::editor::SymbolShape> || std::is_same_v<T,hcs::editor::SymbolPath>) {
                        const auto width_text="STROKE: "+std::to_string(item.stroke_width);
                        CircuitRenderer::text(renderer_,right_x+12,info_y,width_text.c_str(),0.58F,kText); info_y+=18;
                        if constexpr(std::is_same_v<T,hcs::editor::SymbolShape>) {
                            const auto type_text="TYPE: "+std::string(hcs::editor::symbol_shape_name(item.type));
                            CircuitRenderer::text(renderer_,right_x+12,info_y,type_text.c_str(),0.58F,kMuted); info_y+=18;
                        } else {
                            const auto point_text="POINTS: "+std::to_string(item.points.size());
                            CircuitRenderer::text(renderer_,right_x+12,info_y,point_text.c_str(),0.58F,kMuted); info_y+=18;
                        }
                    } else {
                        const auto size_text="SIZE: "+std::to_string(item.size);
                        CircuitRenderer::text(renderer_,right_x+12,info_y,size_text.c_str(),0.58F,kText); info_y+=18;
                        CircuitRenderer::text(renderer_,right_x+12,info_y,"FONT: UI / CODE",0.55F,kMuted); info_y+=18;
                    }
                },*selected);
            }
            CircuitRenderer::text(renderer_,right_x+12,info_y,"R ROTATE  D DUPLICATE",0.58F,kMuted); info_y+=18;
            CircuitRenderer::text(renderer_,right_x+12,info_y,"[ ] CHANGE LAYER",0.58F,kMuted); info_y+=18;
            CircuitRenderer::text(renderer_,right_x+12,info_y,"V VISIBILITY  L LOCK",0.58F,kMuted); info_y+=18;
            CircuitRenderer::text(renderer_,right_x+12,info_y,"E EDIT TEXT",0.58F,kMuted); info_y+=18;
            CircuitRenderer::text(renderer_,right_x+12,info_y,"P EDIT PATH VERTICES",0.55F,kLogicOne);
        }
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-150,"DRAG TO DRAW",0.62F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-130,"ENTER FINISH POLYGON/PATH",0.55F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-112,"P: VERTEX; CTRL+CLICK: ADD",0.51F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-94,"DELETE: REMOVE VERTEX/OBJECT",0.51F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-76,"C: CLOSE PATH; HOME/END: LAYER",0.48F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-58,"SHIFT+ARROWS/H/V: ALIGN",0.50F,kMuted);
        CircuitRenderer::text(renderer_,right_x+12,static_cast<float>(height)-40,"WIDTH / SIZE: +/- BUTTONS",0.49F,kMuted);
        CircuitRenderer::fill_rect(renderer_,0,static_cast<float>(height)-kStatusHeight,static_cast<float>(width),kStatusHeight,kToolbar);
        CircuitRenderer::text(renderer_,12,static_cast<float>(height)-28,
            ("SYMBOL "+editing_definition_->id+ (definition_metadata_dirty_?" *":"")+"  |  "+std::string(symbol_tool_==SymbolTool::Select?"SELECT":symbol_tool_==SymbolTool::Polygon?"POLYGON":symbol_tool_==SymbolTool::Path?"PATH":"DRAW TOOL" )).c_str(),0.68F,kMuted);
        SDL_RenderPresent(renderer_);
    }

    void on_symbol_mouse_down(const SDL_MouseButtonEvent& event) {
        const Vec2 screen{event.x,event.y};
        if(event.button==SDL_BUTTON_MIDDLE && inside_symbol_canvas(screen)) {
            symbol_drag_=SymbolDrag::Pan; symbol_drag_screen_=screen; return;
        }
        if(event.button!=SDL_BUTTON_LEFT) return;
        if(screen.y<kToolbarHeight) {
            for(const auto& button:buttons()) {
                const auto& r=button.rect;
                if(screen.x>=r.x && screen.y>=r.y && screen.x<=r.x+r.width && screen.y<=r.y+r.height) {handle_button(button.action); return;}
            }
            return;
        }
        if(screen.x<188.0F) {
            int width=kInitialWidth,height=kInitialHeight; (void)SDL_GetWindowSize(window_,&width,&height);
            float row_y=112.0F;
            for(const auto& object:editing_definition_->symbol.objects) {
                if(screen.y>=row_y-3 && screen.y<=row_y+18) {
                    selected_symbol_object_id_=std::visit([](const auto& item){return item.id;},object);
                    selected_symbol_port_id_.reset(); selected_symbol_vertex_index_.reset(); return;
                }
                row_y+=23;
                if(row_y>static_cast<float>(height)-220) break;
            }
            row_y=std::max(row_y+8,static_cast<float>(height)-190)+24;
            for(const auto& port:editing_definition_->symbol.ports) {
                if(screen.y>=row_y-3 && screen.y<=row_y+18) {
                    selected_symbol_port_id_=port.port_id; selected_symbol_object_id_.reset(); selected_symbol_vertex_index_.reset(); return;
                }
                row_y+=23;
            }
            return;
        }
        if(!inside_symbol_canvas(screen)) return;
        const auto point=snap_symbol_point(symbol_screen_to_local(screen));
        symbol_cursor_local_=point;
        symbol_cursor_valid_=true;
        if(symbol_tool_==SymbolTool::Polygon || symbol_tool_==SymbolTool::Path) {
            if(symbol_pending_points_.size()<1024) symbol_pending_points_.push_back({point.x,point.y});
            return;
        }
        if(symbol_tool_==SymbolTool::Text) {add_symbol_text_at(point); return;}
        if(symbol_tool_==SymbolTool::Select) {
            if(symbol_vertex_edit_mode_ && selected_symbol_object_id_) {
                if (SDL_GetModState() & SDL_KMOD_CTRL) {
                    const auto inserted=hcs::editor::insert_symbol_object_vertex(editing_definition_->symbol,
                        *selected_symbol_object_id_,point,12.0F/canvas_zoom());
                    if(inserted) {
                        selected_symbol_vertex_index_=*inserted;
                        symbol_drag_=SymbolDrag::Vertex;
                        symbol_drag_last_=point;
                        mark_symbol_changed();
                        return;
                    }
                }
                const auto vertex=hcs::editor::symbol_object_vertex_at(editing_definition_->symbol,
                    *selected_symbol_object_id_,point,9.0F/canvas_zoom());
                if(vertex) {
                    selected_symbol_vertex_index_=*vertex;
                    symbol_drag_=SymbolDrag::Vertex;
                    symbol_drag_last_=point;
                    return;
                }
            }
            if (selected_symbol_object_id_) {
                const auto bounds=symbol_object_bounds(*selected_symbol_object_id_);
                const float handle_tolerance=10.0F/canvas_zoom();
                if (bounds && point.x >= bounds->x+bounds->width-handle_tolerance &&
                    point.y >= bounds->y+bounds->height-handle_tolerance) {
                    symbol_drag_=SymbolDrag::Resize;
                    symbol_drag_last_=point;
                    return;
                }
            }
            if(const auto port=hcs::editor::symbol_port_at(editing_definition_->symbol,point,10.0F/canvas_zoom())) {
                selected_symbol_port_id_=*port; selected_symbol_object_id_.reset(); selected_symbol_vertex_index_.reset();
                symbol_drag_=SymbolDrag::Port; symbol_drag_last_=point; return;
            }
            if(const auto object=hcs::editor::symbol_object_at(editing_definition_->symbol,point,5.0F/canvas_zoom())) {
                selected_symbol_object_id_=*object; selected_symbol_port_id_.reset(); selected_symbol_vertex_index_.reset();
                const auto bounds=symbol_object_bounds(*object);
                if(bounds && point.x>=bounds->x+bounds->width-8.0F/canvas_zoom() && point.y>=bounds->y+bounds->height-8.0F/canvas_zoom())
                    symbol_drag_=SymbolDrag::Resize;
                else symbol_drag_=SymbolDrag::Object;
                symbol_drag_last_=point; return;
            }
            selected_symbol_object_id_.reset(); selected_symbol_port_id_.reset(); selected_symbol_vertex_index_.reset(); symbol_drag_=SymbolDrag::None; return;
        }
        symbol_drag_=SymbolDrag::Draw; symbol_drag_start_=point; symbol_drag_last_=point;
    }

    float canvas_zoom() const {
        int width=kInitialWidth,height=kInitialHeight; (void)SDL_GetWindowSize(window_,&width,&height);
        return symbol_canvas(width,height).zoom;
    }

    std::optional<Rect> symbol_object_bounds(std::string_view id) const {
        if(!editing_definition_) return std::nullopt;
        for(const auto& object:editing_definition_->symbol.objects) {
            const auto result=std::visit([&](const auto& item)->std::optional<Rect> {
                if(item.id!=id) return std::nullopt;
                using T=std::decay_t<decltype(item)>;
                if constexpr(std::is_same_v<T,hcs::editor::SymbolText>) {
                    const float width = std::max(item.size, static_cast<float>(item.value.size()) * item.size * 0.55F);
                    const float left = item.alignment == hcs::editor::SymbolTextAlignment::Center ? item.x - width * 0.5F :
                                       item.alignment == hcs::editor::SymbolTextAlignment::Right ? item.x - width : item.x;
                    return Rect{left, item.y, width, item.size * 1.4F};
                }
                else if constexpr(std::is_same_v<T,hcs::editor::SymbolShape>) {
                    if(!item.points.empty()) {
                        float min_x=item.points[0].x,max_x=min_x,min_y=item.points[0].y,max_y=min_y;
                        for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
                        return Rect{min_x,min_y,std::max(1.0F,max_x-min_x),std::max(1.0F,max_y-min_y)};
                    }
                    return Rect{item.x,item.y,item.width,item.height};
                } else {
                    if(item.points.empty()) return std::nullopt;
                    float min_x=item.points[0].x,max_x=min_x,min_y=item.points[0].y,max_y=min_y;
                    for(const auto& p:item.points){min_x=std::min(min_x,p.x);max_x=std::max(max_x,p.x);min_y=std::min(min_y,p.y);max_y=std::max(max_y,p.y);}
                    return Rect{min_x,min_y,std::max(1.0F,max_x-min_x),std::max(1.0F,max_y-min_y)};
                }
            },object);
            if(result) return result;
        }
        return std::nullopt;
    }

    void on_symbol_mouse_up(const SDL_MouseButtonEvent& event) {
        if(event.button==SDL_BUTTON_MIDDLE && symbol_drag_==SymbolDrag::Pan) {symbol_drag_=SymbolDrag::None;return;}
        if(event.button!=SDL_BUTTON_LEFT) return;
        if(symbol_drag_==SymbolDrag::Draw) create_symbol_shape_from_drag(symbol_drag_start_,symbol_drag_last_);
        symbol_drag_=SymbolDrag::None;
    }

    void on_symbol_mouse_motion(const SDL_MouseMotionEvent& event) {
        const Vec2 screen{event.x,event.y};
        if(symbol_drag_==SymbolDrag::Pan) {
            symbol_pan_offset_=symbol_pan_offset_+(screen-symbol_drag_screen_); symbol_drag_screen_=screen; return;
        }
        if (inside_symbol_canvas(screen)) {
            symbol_cursor_local_=snap_symbol_point(symbol_screen_to_local(screen));
            symbol_cursor_valid_=true;
        } else {
            symbol_cursor_valid_=false;
        }
        if(!inside_symbol_canvas(screen)) return;
        const auto point=symbol_cursor_local_;
        if(symbol_drag_==SymbolDrag::Draw) {symbol_drag_last_=point;return;}
        const auto delta=point-symbol_drag_last_;
        if(symbol_drag_==SymbolDrag::Object && selected_symbol_object_id_) {
            (void)hcs::editor::move_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,delta);
            mark_symbol_changed();
        } else if(symbol_drag_==SymbolDrag::Resize && selected_symbol_object_id_) {
            (void)hcs::editor::resize_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,delta);
            mark_symbol_changed();
        } else if(symbol_drag_==SymbolDrag::Port && selected_symbol_port_id_) {
            (void)hcs::editor::move_symbol_port(editing_definition_->symbol,*selected_symbol_port_id_,point,
                symbol_port_orientation(point));
            mark_symbol_changed();
        } else if(symbol_drag_==SymbolDrag::Vertex && selected_symbol_object_id_ && selected_symbol_vertex_index_) {
            (void)hcs::editor::move_symbol_object_vertex(editing_definition_->symbol,*selected_symbol_object_id_,
                *selected_symbol_vertex_index_,point);
            mark_symbol_changed();
        }
        symbol_drag_last_=point;
    }

    void delete_symbol_selection() {
        if(!editing_definition_) return;
        if(selected_symbol_vertex_index_ && selected_symbol_object_id_) {
            if(hcs::editor::remove_symbol_object_vertex(editing_definition_->symbol,*selected_symbol_object_id_,*selected_symbol_vertex_index_)) {
                selected_symbol_vertex_index_.reset(); mark_symbol_changed();
            } else set_status("A path must retain at least two points; a closed polygon must retain at least three.");
            return;
        }
        if(selected_symbol_port_id_) {
            hcs::editor::PersistenceError error;
            if(!hcs::editor::remove_custom_component_port(*editing_definition_,*selected_symbol_port_id_,error)) {
                set_status("Cannot remove port: "+error.describe()); return;
            }
            circuit_=editing_definition_->implementation;
            selected_symbol_port_id_.reset(); definition_metadata_dirty_=true;
            runtime_.invalidate();
            try {runtime_.synchronize(circuit_,library_);} catch(const std::exception& e){set_status("Port removed; runtime warning: "+std::string(e.what()));}
        } else if(selected_symbol_object_id_) {
            if(hcs::editor::remove_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_)) {
                selected_symbol_object_id_.reset(); mark_symbol_changed();
            }
        }
    }

    void handle_symbol_designer_key(const SDL_KeyboardEvent& event, bool control) {
        if(control && event.key==SDLK_S) { (void)save_definition_edit(); return; }
        if(control && event.key==SDLK_T) { enter_symbol_text_mode(); return; }
        if(event.key==SDLK_ESCAPE) {
            if(symbol_vertex_edit_mode_) { symbol_vertex_edit_mode_=false; selected_symbol_vertex_index_.reset(); }
            else if(!symbol_pending_points_.empty()) symbol_pending_points_.clear();
            else close_symbol_designer();
            return;
        }
        if(event.key==SDLK_RETURN || event.key==SDLK_KP_ENTER) {finish_symbol_path();return;}
        if(event.key==SDLK_DELETE || event.key==SDLK_BACKSPACE) {delete_symbol_selection();return;}
        if(event.key==SDLK_G) {symbol_snap_=!symbol_snap_;return;}
        if(event.key==SDLK_E) {begin_symbol_object_text_edit();return;}
        if(event.key==SDLK_P && selected_symbol_object_id_) {
            if(selected_symbol_object_supports_vertices()) {
                symbol_vertex_edit_mode_=!symbol_vertex_edit_mode_;
                selected_symbol_vertex_index_.reset();
                set_status(symbol_vertex_edit_mode_?"Vertex editing enabled. Drag a handle; Ctrl+click an edge to insert a vertex; Delete removes the selected vertex.":"Vertex editing disabled.");
            } else set_status("Select a polygon or path with editable vertices first.");
            return;
        }
        if(event.key==SDLK_O && selected_symbol_port_id_) {
            const auto found=std::find_if(editing_definition_->symbol.ports.begin(),editing_definition_->symbol.ports.end(),
                [&](const auto& port){return port.port_id==*selected_symbol_port_id_;});
            if(found!=editing_definition_->symbol.ports.end()) {
                const bool angle=(SDL_GetModState() & SDL_KMOD_SHIFT)!=0;
                if(angle) {
                    (void)hcs::editor::set_symbol_port_orientation(editing_definition_->symbol,*selected_symbol_port_id_,
                        hcs::editor::SymbolPortOrientation::Angle,found->angle_degrees);
                } else {
                    auto next=found->orientation;
                    switch(next) {
                        case hcs::editor::SymbolPortOrientation::Left: next=hcs::editor::SymbolPortOrientation::Right; break;
                        case hcs::editor::SymbolPortOrientation::Right: next=hcs::editor::SymbolPortOrientation::Top; break;
                        case hcs::editor::SymbolPortOrientation::Top: next=hcs::editor::SymbolPortOrientation::Bottom; break;
                        case hcs::editor::SymbolPortOrientation::Bottom: next=hcs::editor::SymbolPortOrientation::Left; break;
                        case hcs::editor::SymbolPortOrientation::Angle: next=hcs::editor::SymbolPortOrientation::Left; break;
                    }
                    (void)hcs::editor::set_symbol_port_orientation(editing_definition_->symbol,*selected_symbol_port_id_,next,0.0F);
                }
                mark_symbol_changed();
            }
            return;
        }
        if(selected_symbol_port_id_ && (event.key==SDLK_LEFT || event.key==SDLK_RIGHT)) {
            const auto found=std::find_if(editing_definition_->symbol.ports.begin(),editing_definition_->symbol.ports.end(),
                [&](const auto& port){return port.port_id==*selected_symbol_port_id_;});
            if(found!=editing_definition_->symbol.ports.end() && found->orientation==hcs::editor::SymbolPortOrientation::Angle) {
                const float delta=event.key==SDLK_LEFT?-15.0F:15.0F;
                (void)hcs::editor::set_symbol_port_orientation(editing_definition_->symbol,*selected_symbol_port_id_,
                    hcs::editor::SymbolPortOrientation::Angle,found->angle_degrees+delta);
                mark_symbol_changed(); return;
            }
        }
        if (selected_symbol_object_id_ && (SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
            std::optional<hcs::editor::SymbolAlignment> alignment;
            switch (event.key) {
                case SDLK_LEFT: alignment = hcs::editor::SymbolAlignment::Left; break;
                case SDLK_H: alignment = hcs::editor::SymbolAlignment::HorizontalCenter; break;
                case SDLK_RIGHT: alignment = hcs::editor::SymbolAlignment::Right; break;
                case SDLK_UP: alignment = hcs::editor::SymbolAlignment::Top; break;
                case SDLK_V: alignment = hcs::editor::SymbolAlignment::VerticalCenter; break;
                case SDLK_DOWN: alignment = hcs::editor::SymbolAlignment::Bottom; break;
                default: break;
            }
            if (alignment) {
                if (hcs::editor::align_symbol_object(editing_definition_->symbol, *selected_symbol_object_id_, *alignment)) {
                    mark_symbol_changed();
                    set_status("Aligned selected object to the symbol canvas.");
                }
                return;
            }
        }
        if(selected_symbol_object_id_) {
            if(event.key==SDLK_HOME || event.key==SDLK_END) {
                const int direction = event.key==SDLK_HOME ? -1 : 1;
                bool changed = false;
                while (hcs::editor::reorder_symbol_object(editing_definition_->symbol, *selected_symbol_object_id_, direction)) changed = true;
                if (changed) mark_symbol_changed();
                return;
            }
            if(event.key==SDLK_C) {
                const auto found=std::find_if(editing_definition_->symbol.objects.begin(),editing_definition_->symbol.objects.end(),
                    [&](const auto& object){return std::visit([&](const auto& item){return item.id==*selected_symbol_object_id_;},object);});
                if(found!=editing_definition_->symbol.objects.end()) {
                    if(const auto* path=std::get_if<hcs::editor::SymbolPath>(&*found)) {
                        const bool was_closed=path->closed;
                        if(hcs::editor::set_symbol_path_closed(editing_definition_->symbol,*selected_symbol_object_id_,!was_closed)) {
                            mark_symbol_changed(); set_status(was_closed?"Path opened.":"Path closed.");
                        }
                    }
                }
                return;
            }
            if(event.key==SDLK_R) {if(hcs::editor::rotate_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,90.0F)) mark_symbol_changed();}
            else if(event.key==SDLK_D) {const auto id=next_symbol_object_id();if(hcs::editor::duplicate_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,id)){selected_symbol_object_id_=id;mark_symbol_changed();}}
            else if(event.key==SDLK_LEFTBRACKET) {if(hcs::editor::reorder_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,-1))mark_symbol_changed();}
            else if(event.key==SDLK_RIGHTBRACKET) {if(hcs::editor::reorder_symbol_object(editing_definition_->symbol,*selected_symbol_object_id_,1))mark_symbol_changed();}
            else if(event.key==SDLK_V || event.key==SDLK_L) {
                for(auto& object:editing_definition_->symbol.objects) std::visit([&](auto& item){if(item.id==*selected_symbol_object_id_){if(event.key==SDLK_V)item.visible=!item.visible;else item.locked=!item.locked;}},object);
                mark_symbol_changed();
            }
            else if(event.key==SDLK_F) {
                for(auto& object:editing_definition_->symbol.objects) if(auto* shape=std::get_if<hcs::editor::SymbolShape>(&object);shape && shape->id==*selected_symbol_object_id_) {
                    shape->fill=shape->fill=="none"?"#26384C":"none"; mark_symbol_changed(); break;
                }
            }
        }
    }

    void enter_text_mode() {
        if (!text_buffer_dirty_) reload_text_from_model();
        text_mode_ = true;
        controller_.pause();
        SDL_StartTextInput(window_);
    }

    void leave_text_mode() {
        text_mode_ = false;
        SDL_StopTextInput(window_);
        if (text_buffer_dirty_) set_status("Unapplied text edits are preserved; use TEXT to return or RELOAD TEXT to discard them.");
        update_window_title();
    }

    void reload_text_from_model() {
        hcs::editor::PersistenceError error;
        std::string generated;
        if (!hcs::editor::serialize_circuit(circuit_, document_name_, library_, generated, error)) {
            text_error_ = "Cannot generate text: " + error.describe();
            return;
        }
        text_buffer_ = std::move(generated);
        text_cursor_ = text_buffer_.size();
        text_selection_anchor_.reset();
        text_buffer_dirty_ = false;
        text_base_revision_ = circuit_.document_revision();
        text_error_.clear();
        update_window_title();
    }

    bool apply_text() {
        if (text_base_revision_ != circuit_.document_revision()) {
            text_error_ = "The graphical circuit changed after this text was generated. Use RELOAD TEXT before applying.";
            return false;
        }
        hcs::editor::PersistenceError error;
        std::string new_name;
        if (!hcs::editor::parse_circuit(text_buffer_, circuit_, new_name, library_, error)) {
            text_error_ = "Apply failed: " + error.describe();
            return false;
        }
        document_name_ = std::move(new_name);
        runtime_.synchronize(circuit_, library_);
        controller_.reset(circuit_, library_);
        controller_.pause();
        reload_text_from_model();
        set_status("Applied text changes to the graphical circuit. Save to write them to disk.");
        return true;
    }

    void erase_text_selection() {
        if (!text_selection_anchor_) return;
        const auto first = std::min(*text_selection_anchor_, text_cursor_);
        const auto last = std::max(*text_selection_anchor_, text_cursor_);
        text_buffer_.erase(first, last - first);
        text_cursor_ = first;
        text_selection_anchor_.reset();
    }

    void insert_text(std::string_view value) {
        if (value.empty() || text_buffer_.size() + value.size() > 1024U * 1024U) return;
        erase_text_selection();
        text_buffer_.insert(text_cursor_, value);
        text_cursor_ += value.size();
        text_buffer_dirty_ = true;
        text_error_.clear();
        update_window_title();
    }

    void on_text_input(const char* value) {
        if (!value) return;
        if (path_mode_ != PathMode::None) {
            if (path_buffer_.size() + std::char_traits<char>::length(value) <= 4096U) path_buffer_ += value;
        } else if (symbol_text_mode_ && confirm_pending_ == DeferredAction::None) {
            insert_symbol_text(value);
        } else if (text_mode_ && confirm_pending_ == DeferredAction::None) {
            insert_text(value);
        } else if (library_panel_ && path_mode_ == PathMode::None) {
            const auto length = std::char_traits<char>::length(value);
            if (library_search_.size() + length <= 128U) library_search_ += value;
        }
    }

    std::size_t previous_text_position(std::size_t position) const {
        if (position == 0) return 0;
        --position;
        while (position > 0 && (static_cast<unsigned char>(text_buffer_[position]) & 0xC0U) == 0x80U) --position;
        return position;
    }

    std::size_t next_text_position(std::size_t position) const {
        if (position >= text_buffer_.size()) return text_buffer_.size();
        ++position;
        while (position < text_buffer_.size() && (static_cast<unsigned char>(text_buffer_[position]) & 0xC0U) == 0x80U) ++position;
        return position;
    }

    void handle_text_key(const SDL_KeyboardEvent& event, bool control, bool shift) {
        if (control && (event.key == SDLK_S)) {
            if (apply_text()) save_current_or_prompt();
            return;
        }
        if (control && (event.key == SDLK_R)) { reload_text_from_model(); return; }
        if (control && (event.key == SDLK_T)) { leave_text_mode(); return; }
        if (control && (event.key == SDLK_A)) {
            text_selection_anchor_ = 0;
            text_cursor_ = text_buffer_.size();
            return;
        }
        if (control && (event.key == SDLK_C || event.key == SDLK_X)) {
            if (text_selection_anchor_) {
                const auto first = std::min(*text_selection_anchor_, text_cursor_);
                const auto last = std::max(*text_selection_anchor_, text_cursor_);
                const auto selected = text_buffer_.substr(first, last - first);
                (void)SDL_SetClipboardText(selected.c_str());
                if (event.key == SDLK_X) { erase_text_selection(); text_buffer_dirty_ = true; }
            }
            return;
        }
        if (control && (event.key == SDLK_V)) {
            char* clipboard = SDL_GetClipboardText();
            if (clipboard) { insert_text(clipboard); SDL_free(clipboard); }
            return;
        }
        if (event.key == SDLK_ESCAPE) { leave_text_mode(); return; }
        if (event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) { insert_text("\n"); return; }
        if (event.key == SDLK_TAB) { insert_text("  "); return; }

        if (!shift && text_selection_anchor_ && (event.key == SDLK_LEFT || event.key == SDLK_RIGHT ||
            event.key == SDLK_HOME || event.key == SDLK_END)) {
            text_cursor_ = event.key == SDLK_LEFT || event.key == SDLK_HOME
                ? std::min(*text_selection_anchor_, text_cursor_) : std::max(*text_selection_anchor_, text_cursor_);
            text_selection_anchor_.reset();
        }
        if (shift && !text_selection_anchor_) text_selection_anchor_ = text_cursor_;
        switch (event.key) {
            case SDLK_BACKSPACE:
                if (text_selection_anchor_) { erase_text_selection(); text_buffer_dirty_ = true; }
                else if (text_cursor_ > 0) {
                    const auto previous = previous_text_position(text_cursor_);
                    text_buffer_.erase(previous, text_cursor_ - previous);
                    text_cursor_ = previous;
                    text_buffer_dirty_ = true;
                }
                break;
            case SDLK_DELETE:
                if (text_selection_anchor_) { erase_text_selection(); text_buffer_dirty_ = true; }
                else if (text_cursor_ < text_buffer_.size()) {
                    text_buffer_.erase(text_cursor_, next_text_position(text_cursor_) - text_cursor_);
                    text_buffer_dirty_ = true;
                }
                break;
            case SDLK_LEFT: text_cursor_ = previous_text_position(text_cursor_); break;
            case SDLK_RIGHT: text_cursor_ = next_text_position(text_cursor_); break;
            case SDLK_HOME: {
                const auto newline = text_buffer_.rfind('\n', text_cursor_ == 0 ? 0 : text_cursor_ - 1);
                text_cursor_ = newline == std::string::npos ? 0 : newline + 1;
                break;
            }
            case SDLK_END: {
                const auto newline = text_buffer_.find('\n', text_cursor_);
                text_cursor_ = newline == std::string::npos ? text_buffer_.size() : newline;
                break;
            }
            default: break;
        }
        update_window_title();
    }

    void render_text_mode() {
        int width = kInitialWidth;
        int height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        SDL_SetRenderDrawColor(renderer_, 13, 18, 25, 255);
        (void)SDL_RenderClear(renderer_);
        CircuitRenderer::fill_rect(renderer_, 0.0F, 0.0F, static_cast<float>(width), kToolbarHeight, kToolbar);
        for (const auto& button : buttons()) render_button(button);
        CircuitRenderer::text(renderer_, 365.0F, 17.0F, "HCS TEXT EDITOR  |  CTRL+S APPLY+SAVE  CTRL+R RELOAD  CTRL+T GRAPH", 0.85F, kMuted);
        CircuitRenderer::text(renderer_, 16.0F, 82.0F, (current_filename() + (text_buffer_dirty_ ? "  [UNAPPLIED EDITS]" : "")).c_str(), 1.0F, kLogicOne);
        if (!text_error_.empty()) CircuitRenderer::text(renderer_, 16.0F, 98.0F, text_error_.c_str(), 0.78F, SDL_Color{255, 128, 128, 255});

        std::size_t cursor_line = 0;
        std::size_t cursor_line_start = 0;
        for (std::size_t i = 0; i < text_cursor_ && i < text_buffer_.size(); ++i) {
            if (text_buffer_[i] == '\n') {
                ++cursor_line;
                cursor_line_start = i + 1;
            }
        }
        const std::size_t cursor_line_byte_offset = text_cursor_ - cursor_line_start;
        const std::size_t visible_lines = height > 180 ? static_cast<std::size_t>((height - 160) / 16) : 1U;
        const std::size_t first_line = cursor_line >= visible_lines ? cursor_line - visible_lines + 1 : 0;
        std::size_t line_number = 0;
        std::size_t line_start = 0;
        float y = 124.0F;
        while (line_start <= text_buffer_.size() && y < static_cast<float>(height) - 36.0F) {
            const auto newline = text_buffer_.find('\n', line_start);
            const auto line_end = newline == std::string::npos ? text_buffer_.size() : newline;
            if (line_number >= first_line) {
                const auto number = std::to_string(line_number + 1);
                CircuitRenderer::text(renderer_, 16.0F, y, number.c_str(), 0.85F, kMuted);
                const auto source = text_buffer_.substr(line_start, line_end - line_start);
                const float text_area_width = std::max(1.0F, static_cast<float>(width) - 110.0F);
                std::size_t visible_bytes = source.size();
                while (visible_bytes > 0) {
                    const std::string candidate = source.substr(0, visible_bytes);
                    if (CircuitRenderer::text_size(candidate.c_str(), 0.9F, hcs::gui::FontRole::Code).x <= text_area_width)
                        break;
                    --visible_bytes;
                    while (visible_bytes > 0 &&
                           (static_cast<unsigned char>(source[visible_bytes]) & 0xC0U) == 0x80U)
                        --visible_bytes;
                }
                const auto visible = source.substr(0, visible_bytes);
                CircuitRenderer::text(renderer_, 64.0F, y, visible.c_str(), 0.9F, kText, hcs::gui::FontRole::Code);
                if (line_number == cursor_line && cursor_line_byte_offset <= visible_bytes) {
                    const auto prefix = source.substr(0, cursor_line_byte_offset);
                    const float cursor_x = 64.0F +
                        CircuitRenderer::text_size(prefix.c_str(), 0.9F, hcs::gui::FontRole::Code).x;
                    const float cursor_height = std::max(9.0F,
                        CircuitRenderer::text_size("Ag", 0.9F, hcs::gui::FontRole::Code).y);
                    CircuitRenderer::line(renderer_, {cursor_x, y - 1.0F},
                                          {cursor_x, y + cursor_height}, kLogicOne);
                }
            }
            ++line_number;
            if (newline == std::string::npos) break;
            line_start = newline + 1;
            y += 16.0F;
        }
        CircuitRenderer::fill_rect(renderer_, 0.0F, static_cast<float>(height) - 28.0F, static_cast<float>(width), 28.0F, kToolbar);
        CircuitRenderer::text(renderer_, 12.0F, static_cast<float>(height) - 18.0F,
                              "EDIT TEXT  |  ARROWS MOVE  SHIFT SELECT  CTRL+A SELECT ALL  CTRL+C/V CLIPBOARD  ENTER NEW LINE  BACKSPACE DELETE",
                              0.72F, kMuted);
        render_library_panel(width, height);
        render_overlays(renderer_, width, height);
        SDL_RenderPresent(renderer_);
    }

    void render_overlays(SDL_Renderer* target, int width, int height) {
        if (overwrite_confirmation_) {
            CircuitRenderer::fill_rect(target, 140.0F, static_cast<float>(height) * 0.5F - 55.0F,
                                       static_cast<float>(width) - 280.0F, 110.0F, SDL_Color{27, 37, 50, 255});
            CircuitRenderer::outline_rect(target, 140.0F, static_cast<float>(height) * 0.5F - 55.0F,
                                          static_cast<float>(width) - 280.0F, 110.0F, kLogicOne);
            CircuitRenderer::text(target, 165.0F, static_cast<float>(height) * 0.5F - 22.0F,
                                  "FILE EXISTS - OVERWRITE?", 1.0F, kText);
            CircuitRenderer::text(target, 165.0F, static_cast<float>(height) * 0.5F + 16.0F,
                                  "Y OVERWRITE  |  N OR ESC CANCEL", 0.95F, kLogicOne);
        } else if (path_mode_ != PathMode::None) {
            CircuitRenderer::fill_rect(target, 120.0F, static_cast<float>(height) * 0.5F - 70.0F,
                                       static_cast<float>(width) - 240.0F, 140.0F, SDL_Color{27, 37, 50, 255});
            CircuitRenderer::outline_rect(target, 120.0F, static_cast<float>(height) * 0.5F - 70.0F,
                                          static_cast<float>(width) - 240.0F, 140.0F, kSelection);
            CircuitRenderer::text(target, 145.0F, static_cast<float>(height) * 0.5F - 42.0F,
                path_mode_ == PathMode::Open ? "OPEN HCS FILE - ENTER PATH" :
                path_mode_ == PathMode::DefinitionName ? "CUSTOM COMPONENT NAME - ENTER NAME" :
                path_mode_ == PathMode::NewDefinitionName ? "NEW EMPTY COMPONENT - ENTER NAME" :
                path_mode_ == PathMode::EditDefinitionName ? "RENAME COMPONENT DEFINITION" :
                path_mode_ == PathMode::EditDefinitionDescription ? "EDIT COMPONENT DESCRIPTION" :
                path_mode_ == PathMode::EditDefinitionCategory ? "EDIT COMPONENT CATEGORY" :
                path_mode_ == PathMode::PortName ? "RENAME EXTERNAL PORT" :
                path_mode_ == PathMode::SymbolObjectText ? "EDIT SYMBOL TEXT OBJECT" :
                path_mode_ == PathMode::SymbolPortLabel ? "EDIT VISUAL PORT LABEL" :
                "SAVE HCS FILE AS - ENTER PATH", 1.0F, kText);
            CircuitRenderer::text(target, 145.0F, static_cast<float>(height) * 0.5F - 8.0F,
                                  path_buffer_.c_str(), 1.0F, kLogicOne);
            CircuitRenderer::text(target, 145.0F, static_cast<float>(height) * 0.5F + 35.0F,
                                  path_mode_ == PathMode::SaveAs ?
                                  "ENTER CONFIRM  ESC CANCEL  EXTENSION .HCS IS ADDED WHEN MISSING" :
                                  "ENTER CONFIRM  ESC CANCEL", 0.8F, kMuted);
        } else if (confirm_pending_ != DeferredAction::None) {
            CircuitRenderer::fill_rect(target, 140.0F, static_cast<float>(height) * 0.5F - 65.0F,
                                       static_cast<float>(width) - 280.0F, 130.0F, SDL_Color{27, 37, 50, 255});
            CircuitRenderer::outline_rect(target, 140.0F, static_cast<float>(height) * 0.5F - 65.0F,
                                          static_cast<float>(width) - 280.0F, 130.0F, kLogicOne);
            CircuitRenderer::text(target, 165.0F, static_cast<float>(height) * 0.5F - 34.0F,
                                  "UNSAVED CHANGES - SAVE BEFORE CONTINUING?", 1.0F, kText);
            CircuitRenderer::text(target, 165.0F, static_cast<float>(height) * 0.5F + 8.0F,
                                  "Y SAVE  |  N DISCARD  |  ESC CANCEL", 0.95F, kLogicOne);
        }
        if (!status_message_.empty() && path_mode_ == PathMode::None && confirm_pending_ == DeferredAction::None && !overwrite_confirmation_) {
            CircuitRenderer::text(target, 16.0F, 82.0F, status_message_.c_str(), 0.8F, kMuted);
        }
    }

    std::string backend_button_label() const {
        const char* label = "UNKNOWN";
        switch (runtime_.backend()) {
            case RuntimeBackend::InterpretedEvent: label = "INT EVENT"; break;
            case RuntimeBackend::InterpretedDense: label = "INT DENSE"; break;
            case RuntimeBackend::OptimizedIRDense: label = "IR DENSE"; break;
            case RuntimeBackend::OptimizedIREvent: label = "IR EVENT"; break;
            case RuntimeBackend::HybridIR: label = "HYBRID IR"; break;
            case RuntimeBackend::CompiledNative: label = "NATIVE"; break;
        }
        return std::string("BACKEND: ") + label;
    }

    std::vector<Button> buttons() const {
        constexpr float y = 10.0F;
        constexpr float h = 27.0F;
        constexpr float settings_y = 41.0F;
        if (symbol_text_mode_) {
            return {
                {{10, y, 116, h}, "APPLY SYMBOL", Button::Action::SymbolTextApply},
                {{132, y, 118, h}, "RELOAD SYMBOL", Button::Action::SymbolTextReload},
                {{256, y, 104, h}, "VISUAL VIEW", Button::Action::SymbolBack},
            };
        }
        if (symbol_designer_mode_) {
            return {
                {{8,y,58,h},"SELECT",Button::Action::SymbolSelect},
                {{70,y,48,h},"RECT",Button::Action::SymbolRect},
                {{122,y,58,h},"ROUND",Button::Action::SymbolRoundedRect},
                {{184,y,58,h},"CIRCLE",Button::Action::SymbolCircle},
                {{246,y,62,h},"ELLIPSE",Button::Action::SymbolEllipse},
                {{312,y,48,h},"LINE",Button::Action::SymbolLine},
                {{364,y,58,h},"POLYGON",Button::Action::SymbolPolygon},
                {{426,y,48,h},"PATH",Button::Action::SymbolPath},
                {{478,y,48,h},"ARC",Button::Action::SymbolArc},
                {{530,y,52,h},"TEXT",Button::Action::SymbolAddText},
                {{586,y,52,h},"+INPUT",Button::Action::SymbolAddInput},
                {{642,y,58,h},"+OUTPUT",Button::Action::SymbolAddOutput},
                {{704,y,90,h},symbol_snap_?"SNAP ON":"SNAP OFF",Button::Action::SymbolSnap},
                {{798,y,112,h},"SYMBOL TEXT",Button::Action::SymbolTextMode},
                {{914,y,82,h},"BACK",Button::Action::SymbolBack},
                {{1000,y,96,h},"SAVE DEF",Button::Action::SaveDefinition},
                {{8,settings_y,92,h},"PRESET +",Button::Action::SymbolPreset},
                {{104,settings_y,82,h},"FILL",Button::Action::SymbolFill},
                {{190,settings_y,82,h},"STROKE",Button::Action::SymbolStroke},
                {{276,settings_y,90,h},"ZOOM +",Button::Action::SymbolZoomIn},
                {{370,settings_y,90,h},"ZOOM -",Button::Action::SymbolZoomOut},
                {{464,settings_y,68,h},"WIDTH -",Button::Action::SymbolStrokeDown},
                {{536,settings_y,68,h},"WIDTH +",Button::Action::SymbolStrokeUp},
                {{608,settings_y,68,h},"SIZE -",Button::Action::SymbolTextSizeDown},
                {{680,settings_y,68,h},"SIZE +",Button::Action::SymbolTextSizeUp},
            };
        }
        if (text_mode_) {
            return {
                {{10, y, 106, h}, "APPLY TEXT", Button::Action::ApplyText},
                {{122, y, 106, h}, "RELOAD TEXT", Button::Action::ReloadText},
                {{234, y, 106, h}, "GRAPH VIEW", Button::Action::BackToGraph},
            };
        }
        if (editing_definition_) {
            return {
                {{10, y, 76, h}, "+PORT IN", Button::Action::PlaceInput},
                {{90, y, 88, h}, "+PORT OUT", Button::Action::PlaceOutput},
                {{184, y, 58, h}, "+AND", Button::Action::PlaceAnd},
                {{246, y, 54, h}, "+OR", Button::Action::PlaceOr},
                {{304, y, 58, h}, "+NOT", Button::Action::PlaceNot},
                {{366, y, 58, h}, "+XOR", Button::Action::PlaceXor},
                {{428, y, 58, h}, "+DFF", Button::Action::PlaceDff},
                {{500, y, 76, h}, controller_.running() ? "PAUSE" : "RUN", Button::Action::RunPause},
                {{580, y, 58, h}, "STEP", Button::Action::Step},
                {{642, y, 66, h}, "RESET", Button::Action::Reset},
                {{712, y, 204, h}, backend_button_label(), Button::Action::NextBackend},
                {{920, y, 124, h}, "RATE", Button::Action::NextRate},
                {{10, settings_y, 66, h}, "TEXT", Button::Action::TextEdit},
                {{82, settings_y, 96, h}, "SAVE DEF", Button::Action::SaveDefinition},
                {{184, settings_y, 108, h}, "CANCEL EDIT", Button::Action::CancelDefinitionEdit},
                {{300, settings_y, 90, h}, "LIBRARY", Button::Action::ToggleLibrary},
                {{396, settings_y, 90, h}, "SYMBOL", Button::Action::OpenSymbolDesigner},
            };
        }
        const auto& acceleration = runtime_.acceleration();
        const char* simd_label = acceleration.simd == hcs::SimdMode::Off ? "SIMD: OFF" :
                                 acceleration.simd == hcs::SimdMode::Auto ? "SIMD: AUTO" : "SIMD: AVX2";
        const char* cpu_label = acceleration.parallel == hcs::CpuParallelMode::Off ? "CPU: OFF" :
                                acceleration.parallel == hcs::CpuParallelMode::Auto ? "CPU: AUTO" : "CPU: CUSTOM";
        return {
            {{10, y, 66, h}, "+INPUT", Button::Action::PlaceInput},
            {{80, y, 66, h}, "+OUTPUT", Button::Action::PlaceOutput},
            {{150, y, 58, h}, "+AND", Button::Action::PlaceAnd},
            {{212, y, 54, h}, "+OR", Button::Action::PlaceOr},
            {{270, y, 58, h}, "+NOT", Button::Action::PlaceNot},
            {{332, y, 58, h}, "+XOR", Button::Action::PlaceXor},
            {{394, y, 58, h}, "+DFF", Button::Action::PlaceDff},
            {{470, y, 76, h}, controller_.running() ? "PAUSE" : "RUN", Button::Action::RunPause},
            {{550, y, 58, h}, "STEP", Button::Action::Step},
            {{612, y, 66, h}, "RESET", Button::Action::Reset},
            {{682, y, 204, h}, backend_button_label(), Button::Action::NextBackend},
            {{890, y, 124, h}, "RATE", Button::Action::NextRate},
            {{1018, y, 76, h}, show_grid_ ? "GRID ON" : "GRID OFF", Button::Action::ToggleGrid},
            {{1098, y, 76, h}, "DEMO", Button::Action::Demo},
            {{10, settings_y, 94, h}, simd_label, Button::Action::ToggleSimd},
            {{108, settings_y, 94, h}, cpu_label, Button::Action::ToggleParallel},
            {{206, settings_y, 94, h}, "WORKERS +", Button::Action::CycleWorkers},
            {{304, settings_y, 94, h}, "GPU: N/A", Button::Action::GpuInfo},
            {{414, settings_y, 66, h}, "NEW", Button::Action::NewCircuit},
            {{484, settings_y, 66, h}, "OPEN", Button::Action::OpenCircuit},
            {{554, settings_y, 66, h}, "SAVE", Button::Action::SaveCircuit},
            {{624, settings_y, 82, h}, "SAVE AS", Button::Action::SaveCircuitAs},
            {{710, settings_y, 66, h}, "TEXT", Button::Action::TextEdit},
            {{780, settings_y, 90, h}, "LIBRARY", Button::Action::ToggleLibrary},
            {{874, settings_y, 120, h}, "MAKE CUSTOM", Button::Action::CreateCustomFromSelection},
            {{998, settings_y, 100, h}, "NEW CUSTOM", Button::Action::NewEmptyCustom},
            {{1102, settings_y, 90, h}, "EDIT DEF", Button::Action::EditDefinition},
        };
    }

    void handle_button(Button::Action action) {
        if (symbol_text_mode_) {
            if (action == Button::Action::SymbolTextApply) (void)apply_symbol_text();
            else if (action == Button::Action::SymbolTextReload) reload_symbol_text();
            else if (action == Button::Action::SymbolBack) leave_symbol_text_mode();
            return;
        }
        if (symbol_designer_mode_) {
            switch (action) {
                case Button::Action::SymbolBack: close_symbol_designer(); break;
                case Button::Action::SymbolTextMode: enter_symbol_text_mode(); break;
                case Button::Action::SymbolSelect: symbol_tool_=SymbolTool::Select; symbol_pending_points_.clear(); break;
                case Button::Action::SymbolRect: symbol_tool_=SymbolTool::Rectangle; break;
                case Button::Action::SymbolRoundedRect: symbol_tool_=SymbolTool::RoundedRectangle; break;
                case Button::Action::SymbolCircle: symbol_tool_=SymbolTool::Circle; break;
                case Button::Action::SymbolEllipse: symbol_tool_=SymbolTool::Ellipse; break;
                case Button::Action::SymbolLine: symbol_tool_=SymbolTool::Line; break;
                case Button::Action::SymbolPolygon: symbol_tool_=SymbolTool::Polygon; symbol_pending_points_.clear(); break;
                case Button::Action::SymbolPath: symbol_tool_=SymbolTool::Path; symbol_pending_points_.clear(); break;
                case Button::Action::SymbolArc: symbol_tool_=SymbolTool::Arc; break;
                case Button::Action::SymbolAddText: symbol_tool_=SymbolTool::Text; break;
                case Button::Action::SymbolAddInput:
                    if (add_definition_port(hcs::editor::PinDirection::Input))
                        selected_symbol_port_id_=editing_definition_->ports.back().id;
                    break;
                case Button::Action::SymbolAddOutput:
                    if (add_definition_port(hcs::editor::PinDirection::Output))
                        selected_symbol_port_id_=editing_definition_->ports.back().id;
                    break;
                case Button::Action::SymbolPreset: apply_symbol_preset(); break;
                case Button::Action::SymbolSnap: symbol_snap_=!symbol_snap_; break;
                case Button::Action::SymbolFill:
                case Button::Action::SymbolStroke:
                    if (selected_symbol_object_id_) {
                        bool changed = false;
                        for (auto& object:editing_definition_->symbol.objects) {
                            std::visit([&](auto& item) {
                                using T = std::decay_t<decltype(item)>;
                                if (item.id != *selected_symbol_object_id_) return;
                                if constexpr (std::is_same_v<T, hcs::editor::SymbolShape> ||
                                              std::is_same_v<T, hcs::editor::SymbolPath>) {
                                    auto& color = action == Button::Action::SymbolFill ? item.fill : item.stroke;
                                    color = color == "#202630" ? "#365A78" : (color == "#365A78" ? "none" : "#202630");
                                    changed = true;
                                } else {
                                    item.color = item.color == "#D0D7E2" ? "#FFBE52" : "#D0D7E2";
                                    changed = true;
                                }
                            }, object);
                            if (changed) break;
                        }
                        if (changed) mark_symbol_changed();
                    }
                    break;
                case Button::Action::SymbolStrokeDown: adjust_selected_symbol_stroke(-0.5F); break;
                case Button::Action::SymbolStrokeUp: adjust_selected_symbol_stroke(0.5F); break;
                case Button::Action::SymbolTextSizeDown: adjust_selected_symbol_text_size(-1.0F); break;
                case Button::Action::SymbolTextSizeUp: adjust_selected_symbol_text_size(1.0F); break;
                case Button::Action::SymbolZoomIn: symbol_zoom_factor_=std::clamp(symbol_zoom_factor_*1.2F,0.3F,5.0F); break;
                case Button::Action::SymbolZoomOut: symbol_zoom_factor_=std::clamp(symbol_zoom_factor_/1.2F,0.3F,5.0F); break;
                case Button::Action::SaveDefinition: (void)save_definition_edit(); break;
                default: break;
            }
            return;
        }
        if (editing_definition_ && action != Button::Action::PlaceInput &&
            action != Button::Action::PlaceOutput && action != Button::Action::PlaceAnd &&
            action != Button::Action::PlaceOr && action != Button::Action::PlaceNot &&
            action != Button::Action::PlaceXor && action != Button::Action::PlaceDff &&
            action != Button::Action::RunPause && action != Button::Action::Step &&
            action != Button::Action::Reset && action != Button::Action::NextBackend &&
            action != Button::Action::NextRate && action != Button::Action::TextEdit &&
            action != Button::Action::ToggleLibrary && action != Button::Action::SaveDefinition &&
            action != Button::Action::CancelDefinitionEdit && action != Button::Action::OpenSymbolDesigner) {
            set_status("This action is unavailable while editing a component definition.");
            return;
        }
        switch (action) {
            case Button::Action::PlaceInput:
                if (editing_definition_) (void)add_definition_port(hcs::editor::PinDirection::Input);
                else placement_ = ComponentType::Input;
                break;
            case Button::Action::PlaceOutput:
                if (editing_definition_) (void)add_definition_port(hcs::editor::PinDirection::Output);
                else placement_ = ComponentType::Output;
                break;
            case Button::Action::PlaceAnd: placement_ = ComponentType::And; break;
            case Button::Action::PlaceOr: placement_ = ComponentType::Or; break;
            case Button::Action::PlaceNot: placement_ = ComponentType::Not; break;
            case Button::Action::PlaceXor: placement_ = ComponentType::Xor; break;
            case Button::Action::PlaceDff: placement_ = ComponentType::DFlipFlop; break;
            case Button::Action::RunPause: controller_.toggle_running(); break;
            case Button::Action::Step: (void)controller_.step_once(); break;
            case Button::Action::Reset: controller_.reset(circuit_, library_); break;
            case Button::Action::NextBackend:
                cycle_backend();
                break;
            case Button::Action::NextRate: {
                constexpr std::array rates{SimulationRate::Hz1, SimulationRate::Hz10,
                    SimulationRate::Hz60, SimulationRate::Hz1000, SimulationRate::Unlimited};
                const auto found = std::find(rates.begin(), rates.end(), controller_.rate());
                controller_.set_rate(found == rates.end() || std::next(found) == rates.end() ? rates.front() : *std::next(found));
                break;
            }
            case Button::Action::ToggleGrid: show_grid_ = !show_grid_; break;
            case Button::Action::Demo: request_action(DeferredAction::Demo); break;
            case Button::Action::ToggleSimd: {
                auto acceleration = runtime_.acceleration();
                const auto capabilities = hcs::detect_hardware_capabilities();
                if (acceleration.simd == hcs::SimdMode::Off) acceleration.simd = hcs::SimdMode::Auto;
                else if (acceleration.simd == hcs::SimdMode::Auto && capabilities.avx2_usable)
                    acceleration.simd = hcs::SimdMode::AVX2;
                else acceleration.simd = hcs::SimdMode::Off;
                apply_acceleration(acceleration);
                break;
            }
            case Button::Action::ToggleParallel: {
                auto acceleration = runtime_.acceleration();
                const auto capabilities = hcs::detect_hardware_capabilities();
                if (acceleration.parallel == hcs::CpuParallelMode::Off && capabilities.logical_cpu_count > 1)
                    acceleration.parallel = hcs::CpuParallelMode::Auto;
                else if (acceleration.parallel == hcs::CpuParallelMode::Auto) {
                    acceleration.parallel = hcs::CpuParallelMode::Custom;
                    acceleration.worker_threads = std::min<std::uint32_t>(4, capabilities.logical_cpu_count);
                } else {
                    acceleration.parallel = hcs::CpuParallelMode::Off;
                    acceleration.worker_threads = 0;
                }
                apply_acceleration(acceleration);
                break;
            }
            case Button::Action::CycleWorkers: {
                auto acceleration = runtime_.acceleration();
                const auto capabilities = hcs::detect_hardware_capabilities();
                const auto maximum = std::max<std::uint32_t>(1, std::min<std::uint32_t>(32, capabilities.logical_cpu_count));
                if (acceleration.parallel != hcs::CpuParallelMode::Custom) {
                    acceleration.parallel = hcs::CpuParallelMode::Custom;
                    acceleration.worker_threads = std::min<std::uint32_t>(2, maximum);
                } else {
                    acceleration.worker_threads = acceleration.worker_threads >= maximum
                        ? 1u : acceleration.worker_threads + 1u;
                }
                apply_acceleration(acceleration);
                break;
            }
            case Button::Action::GpuInfo:
                std::fprintf(stderr, "GPU compute is unavailable: HCS was built without a Vulkan compute backend.\n");
                break;
            case Button::Action::NewCircuit: request_action(DeferredAction::NewCircuit); break;
            case Button::Action::OpenCircuit: request_action(DeferredAction::OpenCircuit); break;
            case Button::Action::SaveCircuit: save_current_or_prompt(); break;
            case Button::Action::SaveCircuitAs: begin_path_entry(PathMode::SaveAs); break;
            case Button::Action::TextEdit: enter_text_mode(); break;
            case Button::Action::ApplyText: apply_text(); break;
            case Button::Action::ReloadText: reload_text_from_model(); break;
            case Button::Action::BackToGraph: leave_text_mode(); break;
            case Button::Action::ToggleLibrary:
                library_panel_ = !library_panel_;
                library_search_.clear();
                library_category_index_ = 0;
                break;
            case Button::Action::CreateCustomFromSelection: begin_path_entry(PathMode::DefinitionName); break;
            case Button::Action::NewEmptyCustom: begin_path_entry(PathMode::NewDefinitionName); break;
            case Button::Action::EditDefinition: edit_selected_definition(); break;
            case Button::Action::SaveDefinition: (void)save_definition_edit(); break;
            case Button::Action::CancelDefinitionEdit: request_action(DeferredAction::CancelDefinitionEdit); break;
            case Button::Action::OpenSymbolDesigner: open_symbol_designer(); break;
            default: break;
        }
    }

    void cycle_backend() {
        constexpr std::array choices{RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
            RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
            RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative};
        const auto previous = runtime_.backend();
        const auto found = std::find(choices.begin(), choices.end(), previous);
        const auto next = found == choices.end() || std::next(found) == choices.end()
            ? choices.front() : *std::next(found);

        runtime_.set_backend(next);
        try {
            runtime_.synchronize(circuit_, library_);
            controller_.notify_input_changed();
            set_status(std::string("Simulation backend: ") + hcs::editor::backend_name(next));
        } catch (const std::exception& error) {
            runtime_.set_backend(previous);
            bool restored = false;
            try {
                runtime_.synchronize(circuit_, library_);
                controller_.notify_input_changed();
                restored = true;
            } catch (const std::exception& restore_error) {
                controller_.pause();
                std::fprintf(stderr, "Backend rollback failed: %s\n", restore_error.what());
            }
            set_status(std::string(restored ? "Backend change failed; previous backend restored: "
                                             : "Backend change failed; runtime recovery failed: ") + error.what());
            std::fprintf(stderr, "Backend change failed: %s\n", error.what());
        }
    }

    void apply_acceleration(const hcs::AccelerationConfig& acceleration) {
        const auto previous = runtime_.acceleration();
        runtime_.set_acceleration(acceleration);
        try {
            runtime_.synchronize(circuit_, library_);
            controller_.notify_input_changed();
        } catch (const std::exception& error) {
            runtime_.set_acceleration(previous);
            try { runtime_.synchronize(circuit_, library_); } catch (...) {}
            std::fprintf(stderr, "Acceleration setting rejected: %s\n", error.what());
        }
    }

    void process_events() {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                    request_action(DeferredAction::Quit);
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    update_viewport();
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    on_mouse_down(event.button);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    on_mouse_up(event.button);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    on_mouse_motion(event.motion);
                    break;
                case SDL_EVENT_MOUSE_WHEEL: {
                    if (symbol_text_mode_ || text_mode_ || path_mode_ != PathMode::None || confirm_pending_ != DeferredAction::None || overwrite_confirmation_) break;
                    const Vec2 pointer{event.wheel.mouse_x, event.wheel.mouse_y};
                    if (symbol_designer_mode_ && inside_symbol_canvas(pointer)) {
                        symbol_zoom_factor_ = std::clamp(symbol_zoom_factor_ * (event.wheel.y > 0 ? 1.1F : 0.9F), 0.3F, 5.0F);
                    } else if (inside_viewport(pointer)) {
                        const auto direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                            ? -event.wheel.y : event.wheel.y;
                        camera_.zoom_at(pointer, direction);
                    }
                    break;
                }
                case SDL_EVENT_KEY_DOWN:
                    on_key_down(event.key);
                    break;
                case SDL_EVENT_TEXT_INPUT:
                    on_text_input(event.text.text);
                    break;
                default:
                    break;
            }
        }
    }

    bool inside_viewport(Vec2 point) const noexcept {
        const auto viewport = camera_.viewport();
        return point.x >= viewport.x && point.y >= viewport.y &&
               point.x <= viewport.x + viewport.width && point.y <= viewport.y + viewport.height;
    }

    void on_mouse_down(const SDL_MouseButtonEvent& event) {
        const Vec2 screen{event.x, event.y};
        if (symbol_designer_mode_ && !symbol_text_mode_ && path_mode_ == PathMode::None &&
            confirm_pending_ == DeferredAction::None && !overwrite_confirmation_) {
            on_symbol_mouse_down(event);
            return;
        }
        if (event.button == SDL_BUTTON_MIDDLE) {
            if (path_mode_ == PathMode::None && confirm_pending_ == DeferredAction::None &&
                !overwrite_confirmation_ && !text_mode_ && inside_viewport(screen)) {
                panning_ = true;
                pan_button_ = SDL_BUTTON_MIDDLE;
                pan_moved_ = false;
                last_mouse_ = screen;
            }
            return;
        }
        if (event.button != SDL_BUTTON_LEFT) return;
        if (path_mode_ != PathMode::None || confirm_pending_ != DeferredAction::None || overwrite_confirmation_) return;

        if (screen.y < kToolbarHeight) {
            for (const auto& button : buttons()) {
                const auto& r = button.rect;
                if (screen.x >= r.x && screen.y >= r.y && screen.x <= r.x + r.width && screen.y <= r.y + r.height) {
                    handle_button(button.action);
                    return;
                }
            }
            return;
        }
        if (text_mode_) return;
        int window_width = kInitialWidth;
        int window_height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &window_width, &window_height);
        const auto library_panel_x = static_cast<float>(window_width) - 330.0F;
        if (library_panel_ && screen.x >= library_panel_x && screen.y >= kToolbarHeight &&
            screen.y < static_cast<float>(window_height) - kStatusHeight) {
            // Select by row order from the filtered catalogue.
            const auto items = filtered_library_items();
            const auto index = screen.y < 166.0F ? items.size() : static_cast<std::size_t>((screen.y - 166.0F) / 56.0F);
            if (index < items.size()) {
                const auto& item = items[index];
                if (item.built_in) {
                    if (item.id == "builtin.input") placement_ = ComponentType::Input;
                    else if (item.id == "builtin.output") placement_ = ComponentType::Output;
                    else if (item.id == "builtin.and") placement_ = ComponentType::And;
                    else if (item.id == "builtin.or") placement_ = ComponentType::Or;
                    else if (item.id == "builtin.not") placement_ = ComponentType::Not;
                    else if (item.id == "builtin.xor") placement_ = ComponentType::Xor;
                    else if (item.id == "builtin.dff") placement_ = ComponentType::DFlipFlop;
                } else placement_custom_ = item.id;
                library_panel_ = false;
                set_status("Selected " + item.name + ". Click the canvas to place it.");
            }
            return;
        }
        if (!inside_viewport(screen)) return;

        const auto world = camera_.screen_to_world(screen);
        if (placement_ || placement_custom_) {
            if (placement_custom_) {
                const auto* definition = library_.find(*placement_custom_);
                if (definition) {
                    const auto ports = hcs::editor::custom_component_port_layouts(*definition);
                    (void)circuit_.add_custom_component(definition->id, world, ports, definition->size);
                }
                placement_custom_.reset();
            } else if (placement_) {
                (void)circuit_.add_component(*placement_, world);
                placement_.reset();
            }
            try { runtime_.synchronize(circuit_, library_); controller_.notify_input_changed(); }
            catch (const std::exception& error) { std::fprintf(stderr, "Component placement failed: %s\n", error.what()); }
            return;
        }

        if (const auto pin = circuit_.pin_at(world, 12.0F / camera_.zoom())) {
            const auto* found = circuit_.find_pin(*pin);
            if (found && found->direction == PinDirection::Output) {
                wire_start_ = *pin;
                wire_cursor_ = world;
                return;
            }
        }

        auto component = circuit_.component_at(world);
        if (component) {
            const auto* hit_component = circuit_.find_component(*component);
            if (hit_component && hit_component->type == ComponentType::Custom) {
                const auto* definition = library_.find(hit_component->definition_id);
                if (definition) {
                    const hcs::editor::SymbolTransform transform{hit_component->rotation_quadrants,
                        hit_component->flip_horizontal, hit_component->flip_vertical};
                    if (!hcs::gui::SymbolRenderer::hit_test(*definition, hit_component->position, world,
                                                            transform, 5.0F / camera_.zoom())) component.reset();
                }
            }
        }
        if (component) {
            const auto modifiers = SDL_GetModState();
            const bool extend_selection = (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) != 0;
            if (extend_selection) {
                circuit_.toggle_component_selection(*component);
                return;
            }
            circuit_.select_component(component);
            const auto* found = circuit_.find_component(*component);
            dragging_component_ = component;
            drag_offset_ = world - found->position;
            mouse_down_world_ = world;
            drag_moved_ = false;
            if (found->type == ComponentType::Input) click_toggle_candidate_ = component;
            return;
        }

        if (const auto wire = circuit_.wire_at(world, 8.0F / camera_.zoom())) {
            circuit_.select_wire(wire);
            return;
        }

        // Dragging an empty canvas with the left mouse button pans the camera.
        // A simple click still clears selection when the button is released.
        panning_ = true;
        pan_button_ = SDL_BUTTON_LEFT;
        pan_moved_ = false;
        last_mouse_ = screen;
    }

    void on_mouse_up(const SDL_MouseButtonEvent& event) {
        if (symbol_designer_mode_ && !symbol_text_mode_) { on_symbol_mouse_up(event); return; }
        if (panning_ && event.button == pan_button_) {
            const bool left_click_without_drag = pan_button_ == SDL_BUTTON_LEFT && !pan_moved_;
            panning_ = false;
            pan_button_ = 0;
            if (left_click_without_drag) circuit_.clear_selection();
            return;
        }
        if (event.button != SDL_BUTTON_LEFT || text_mode_ || path_mode_ != PathMode::None ||
            confirm_pending_ != DeferredAction::None || overwrite_confirmation_) return;

        const Vec2 screen{event.x, event.y};
        if (wire_start_ && inside_viewport(screen)) {
            const auto world = camera_.screen_to_world(screen);
            const auto target = circuit_.pin_at(world, 13.0F / camera_.zoom());
            const auto* pin = target ? circuit_.find_pin(*target) : nullptr;
            if (pin && pin->direction == PinDirection::Input) {
                if (circuit_.connect(*wire_start_, *target)) {
                    try { runtime_.synchronize(circuit_, library_); controller_.notify_input_changed(); }
                    catch (const std::exception& error) { std::fprintf(stderr, "Wire synchronization failed: %s\n", error.what()); }
                }
            }
            wire_start_.reset();
        }
        if (click_toggle_candidate_ && !drag_moved_) {
            if (circuit_.toggle_input(*click_toggle_candidate_)) {
                runtime_.synchronize(circuit_, library_);
                controller_.notify_input_changed();
            }
        }
        click_toggle_candidate_.reset();
        dragging_component_.reset();
        drag_moved_ = false;
    }

    void on_mouse_motion(const SDL_MouseMotionEvent& event) {
        if (symbol_designer_mode_ && !symbol_text_mode_ && path_mode_ == PathMode::None &&
            confirm_pending_ == DeferredAction::None && !overwrite_confirmation_) {
            on_symbol_mouse_motion(event);
            return;
        }
        if (symbol_text_mode_ || text_mode_ || path_mode_ != PathMode::None || confirm_pending_ != DeferredAction::None || overwrite_confirmation_) return;
        const Vec2 screen{event.x, event.y};
        if (panning_) {
            const auto delta = screen - last_mouse_;
            if (delta.x != 0.0F || delta.y != 0.0F) {
                camera_.pan_screen(delta);
                pan_moved_ = true;
            }
            last_mouse_ = screen;
            return;
        }
        if (wire_start_) wire_cursor_ = camera_.screen_to_world(screen);
        if (dragging_component_ && inside_viewport(screen)) {
            const auto world = camera_.screen_to_world(screen);
            const auto delta = world - mouse_down_world_;
            if (delta.x * delta.x + delta.y * delta.y > 9.0F) {
                drag_moved_ = true;
                click_toggle_candidate_.reset();
            }
            (void)circuit_.move_component(*dragging_component_, world - drag_offset_);
        }
    }

    void on_key_down(const SDL_KeyboardEvent& event) {
        if (event.repeat) return;
        const bool control = (event.mod & SDL_KMOD_CTRL) != 0;
        const bool shift = (event.mod & SDL_KMOD_SHIFT) != 0;

        if (overwrite_confirmation_) {
            if (event.key == SDLK_ESCAPE || event.key == SDLK_N) {
                overwrite_confirmation_ = false;
                after_save_action_ = DeferredAction::None;
            } else if (event.key == SDLK_Y || event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) {
                overwrite_confirmation_ = false;
                complete_save_as(overwrite_path_);
            }
            return;
        }
        if (confirm_pending_ != DeferredAction::None) {
            if (event.key == SDLK_ESCAPE) cancel_confirmation();
            else if (event.key == SDLK_Y) confirm_save_and_continue();
            else if (event.key == SDLK_N) confirm_discard_and_continue();
            return;
        }
        if (path_mode_ != PathMode::None) {
            if (event.key == SDLK_ESCAPE) cancel_path_entry();
            else if (event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) finish_path_entry();
            else if (event.key == SDLK_BACKSPACE) erase_path_character();
            else if (control && (event.key == SDLK_A)) path_buffer_.clear();
            return;
        }
        if (symbol_text_mode_) {
            handle_symbol_text_key(event, control, shift);
            return;
        }
        if (symbol_designer_mode_) {
            handle_symbol_designer_key(event, control);
            return;
        }
        if (text_mode_) {
            handle_text_key(event, control, shift);
            return;
        }

        if (!control && !shift && !library_panel_ && !editing_definition_ && event.key == SDLK_B) cycle_backend();
        else if (control && (event.key == SDLK_N)) request_action(DeferredAction::NewCircuit);
        else if (control && (event.key == SDLK_O)) request_action(DeferredAction::OpenCircuit);
        else if (control && (event.key == SDLK_S)) {
            if (editing_definition_) (void)save_definition_edit();
            else if (shift) begin_path_entry(PathMode::SaveAs);
            else save_current_or_prompt();
        } else if (control && (event.key == SDLK_T)) enter_text_mode();
        else if (library_panel_ && event.key == SDLK_ESCAPE) {
            library_panel_ = false;
        } else if (library_panel_ && event.key == SDLK_TAB) {
            library_category_index_ = (library_category_index_ + 1U) % library_categories().size();
        } else if (editing_definition_ && event.key == SDLK_ESCAPE) {
            if (placement_ || placement_custom_ || wire_start_) {
                placement_.reset();
                placement_custom_.reset();
                wire_start_.reset();
            } else {
                request_action(DeferredAction::CancelDefinitionEdit);
            }
        } else if (event.key == SDLK_ESCAPE) {
            if (placement_ || placement_custom_ || wire_start_) {
                placement_.reset();
                placement_custom_.reset();
                wire_start_.reset();
            } else {
                circuit_.clear_selection();
            }
        } else if (library_panel_ && event.key == SDLK_BACKSPACE) {
            if (!library_search_.empty()) {
                std::size_t position = library_search_.size() - 1;
                while (position > 0 && (static_cast<unsigned char>(library_search_[position]) & 0xC0U) == 0x80U) --position;
                library_search_.erase(position);
            }
        } else if (library_panel_ && event.key == SDLK_RETURN) {
            place_selected_library_item();
        } else if (editing_definition_ && !control && event.key == SDLK_P) {
            begin_selected_port_rename();
        } else if (editing_definition_ && !control && event.key == SDLK_N) {
            begin_path_entry(PathMode::EditDefinitionName);
        } else if (editing_definition_ && !control && event.key == SDLK_D) {
            begin_path_entry(PathMode::EditDefinitionDescription);
        } else if (editing_definition_ && !control && event.key == SDLK_C) {
            begin_path_entry(PathMode::EditDefinitionCategory);
        } else if (editing_definition_ && !control && event.key == SDLK_I) {
            (void)add_definition_port(hcs::editor::PinDirection::Input);
        } else if (editing_definition_ && !control && event.key == SDLK_O) {
            (void)add_definition_port(hcs::editor::PinDirection::Output);
        } else if (library_panel_ && event.key == SDLK_E) {
            const auto items = filtered_library_items();
            if (!items.empty() && !items.front().built_in) begin_definition_edit(items.front().id);
            else set_status("Select a custom component entry to edit its definition.");
        } else if (event.key == SDLK_DELETE || event.key == SDLK_BACKSPACE) {
            if (editing_definition_) {
                remove_definition_selection();
            } else {
                bool changed = false;
                const auto selected = circuit_.selected_components();
                if (!selected.empty()) {
                    for (const auto id : selected) changed = circuit_.remove_component(id) || changed;
                } else if (circuit_.selection().wire) changed = circuit_.remove_wire(*circuit_.selection().wire);
                if (changed) {
                    runtime_.synchronize(circuit_, library_);
                    controller_.notify_input_changed();
                }
            }
        } else if (!editing_definition_ && !circuit_.selected_components().empty() && !control) {
            const auto id = circuit_.selected_components().front();
            const auto* selected = circuit_.find_component(id);
            if (selected && selected->type == ComponentType::Custom) {
                if (event.key == SDLK_R) (void)circuit_.rotate_component(id, 1);
                else if (event.key == SDLK_H) (void)circuit_.flip_component(id, true);
                else if (event.key == SDLK_V) (void)circuit_.flip_component(id, false);
            }
        } else if (event.key == SDLK_SPACE) {
            controller_.toggle_running();
        }
    }

    void place_selected_library_item() {
        const auto items = filtered_library_items();
        if (items.empty()) return;
        const auto& item = items.front();
        if (item.built_in) {
            if (item.id == "builtin.input") placement_ = ComponentType::Input;
            else if (item.id == "builtin.output") placement_ = ComponentType::Output;
            else if (item.id == "builtin.and") placement_ = ComponentType::And;
            else if (item.id == "builtin.or") placement_ = ComponentType::Or;
            else if (item.id == "builtin.not") placement_ = ComponentType::Not;
            else if (item.id == "builtin.xor") placement_ = ComponentType::Xor;
            else if (item.id == "builtin.dff") placement_ = ComponentType::DFlipFlop;
        } else {
            placement_custom_ = item.id;
        }
        library_panel_ = false;
        set_status("Selected " + item.name + ". Click the canvas to place it.");
    }

    static const std::vector<std::string>& library_categories() {
        static const std::vector<std::string> categories{
            "All", "Logic Gates", "Arithmetic", "Sequential", "Memory", "Control", "Custom"};
        return categories;
    }

    std::vector<hcs::editor::ComponentMetadata> filtered_library_items() const {
        auto items = library_.catalog();
        const auto& categories = library_categories();
        const auto& category = categories[std::min(library_category_index_, categories.size() - 1)];
        if (category != "All") {
            items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& item) {
                return item.category != category;
            }), items.end());
        }
        if (library_search_.empty()) return items;
        std::string query = library_search_;
        std::transform(query.begin(), query.end(), query.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        items.erase(std::remove_if(items.begin(), items.end(), [&](const auto& item) {
            std::string haystack = item.name + " " + item.category + " " + item.description + " " + item.id;
            std::transform(haystack.begin(), haystack.end(), haystack.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            return haystack.find(query) == std::string::npos;
        }), items.end());
        return items;
    }

    void render_library_panel(int width, int height) {
        if (!library_panel_) return;
        const float panel_x = static_cast<float>(width) - 330.0F;
        CircuitRenderer::fill_rect(renderer_, panel_x, kToolbarHeight, 330.0F,
                                   static_cast<float>(height) - kToolbarHeight - kStatusHeight,
                                   SDL_Color{25, 34, 47, 248});
        CircuitRenderer::outline_rect(renderer_, panel_x, kToolbarHeight, 330.0F,
                                      static_cast<float>(height) - kToolbarHeight - kStatusHeight, kButtonBorder);
        CircuitRenderer::text(renderer_, panel_x + 14.0F, 88.0F, "COMPONENT LIBRARY", 1.0F, kText);
        CircuitRenderer::text(renderer_, panel_x + 14.0F, 110.0F, ("SEARCH: " + library_search_).c_str(), 0.85F, kLogicOne);
        const auto& categories = library_categories();
        CircuitRenderer::text(renderer_, panel_x + 14.0F, 130.0F,
            ("CATEGORY: " + categories[std::min(library_category_index_, categories.size() - 1)] + "  (TAB)").c_str(),
            0.72F, kLogicOne);
        CircuitRenderer::text(renderer_, panel_x + 14.0F, 146.0F, "TYPE TO SEARCH  |  ENTER SELECTS FIRST", 0.58F, kMuted);
        float y = 166.0F;
        const auto items = filtered_library_items();
        for (const auto& item : items) {
            if (y > static_cast<float>(height) - 76.0F) break;
            CircuitRenderer::text(renderer_, panel_x + 14.0F, y, item.name.c_str(), 0.92F, item.built_in ? kText : kLogicOne);
            const auto detail = item.category + "  " + std::to_string(item.input_count) + " IN / " +
                                std::to_string(item.output_count) + " OUT";
            const auto joined = [](const std::vector<std::string>& values) {
                std::string result;
                for (const auto& value : values) {
                    if (!result.empty()) result += ",";
                    result += value;
                }
                return result;
            };
            const auto preview = "IN: " + joined(item.input_names) + "  OUT: " + joined(item.output_names);
            CircuitRenderer::text(renderer_, panel_x + 14.0F, y + 14.0F, detail.c_str(), 0.68F, kMuted);
            CircuitRenderer::text(renderer_, panel_x + 14.0F, y + 25.0F, preview.c_str(), 0.58F, kLogicOne);
            CircuitRenderer::text(renderer_, panel_x + 14.0F, y + 36.0F, item.description.c_str(), 0.50F, kMuted);
            if (!item.built_in) {
                if (const auto* definition = library_.find(item.id))
                    hcs::gui::SymbolRenderer::draw(renderer_, *definition, {panel_x + 232.0F, y + 4.0F}, 0.24F, {}, false);
            }
            y += 56.0F;
        }
        if (items.empty()) CircuitRenderer::text(renderer_, panel_x + 14.0F, y, "NO MATCHING COMPONENTS", 0.8F, kMuted);
    }

    void render_button(const Button& button) {
        const auto active = (button.action == Button::Action::RunPause && controller_.running()) ||
                            (button.action == Button::Action::ToggleGrid && show_grid_);
        const auto color = active ? kButtonActive : kButton;
        CircuitRenderer::fill_rect(renderer_, button.rect.x, button.rect.y, button.rect.width, button.rect.height, color);
        CircuitRenderer::outline_rect(renderer_, button.rect.x, button.rect.y, button.rect.width, button.rect.height, kButtonBorder);
        constexpr float label_scale = 0.96F;
        const auto label_size = CircuitRenderer::text_size(button.label.c_str(), label_scale);
        const float label_x = button.rect.x + (button.rect.width - label_size.x) * 0.5F;
        const float label_y = button.rect.y + (button.rect.height - label_size.y) * 0.5F;
        CircuitRenderer::text(renderer_, label_x, label_y, button.label.c_str(), label_scale, kText);
    }

    void render() {
        if (symbol_text_mode_) { render_symbol_text_editor(); return; }
        if (symbol_designer_mode_) { render_symbol_designer(); return; }
        if (text_mode_) {
            render_text_mode();
            return;
        }
        CircuitRenderer renderer;
        renderer.render(renderer_, circuit_, runtime_, camera_, show_grid_, library_,
            wire_start_ ? std::optional<hcs::gui::WirePreview>{hcs::gui::WirePreview{circuit_.pin_world_position(*wire_start_), wire_cursor_}} : std::nullopt);

        int width = kInitialWidth;
        int height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        CircuitRenderer::fill_rect(renderer_, 0.0F, 0.0F, static_cast<float>(width), kToolbarHeight, kToolbar);
        CircuitRenderer::line(renderer_, {0.0F, kToolbarHeight}, {static_cast<float>(width), kToolbarHeight}, kButtonBorder);
        for (const auto& button : buttons()) render_button(button);

        if (placement_ || placement_custom_) CircuitRenderer::text(renderer_, 1210.0F, 18.0F, "CLICK CANVAS TO PLACE", 0.9F, kLogicOne);
        if (wire_start_) CircuitRenderer::text(renderer_, 1210.0F, 48.0F, "CONNECT TO INPUT PIN", 0.9F, kSelection);

        CircuitRenderer::fill_rect(renderer_, 0.0F, static_cast<float>(height) - kStatusHeight,
                                   static_cast<float>(width), kStatusHeight, kToolbar);
        CircuitRenderer::line(renderer_, {0.0F, static_cast<float>(height) - kStatusHeight},
                              {static_cast<float>(width), static_cast<float>(height) - kStatusHeight}, kButtonBorder);
        char status[384]{};
        std::snprintf(status, sizeof(status), "%s%s   %s   TICK: %llu   COMPONENTS: %zu   WIRES: %zu   ZOOM: %.2FX   %s",
                      dirty() || text_buffer_dirty_ ? "* " : "", current_filename().c_str(),
                      controller_.running() ? "RUNNING" : "PAUSED",
                      static_cast<unsigned long long>(runtime_.tick_count()), circuit_.components().size(),
                      circuit_.wires().size(), static_cast<double>(camera_.zoom()),
                      editing_definition_ ? "I/O PORTS  P RENAME PORT  N/D/C METADATA  CTRL+S SAVE DEF  B BACKEND" : "CTRL+S SAVE  CTRL+T TEXT  B BACKEND");
        CircuitRenderer::text(renderer_, 12.0F, static_cast<float>(height) - 39.0F,
                              status, 0.70F, kMuted);

        const auto hardware = hcs::detect_hardware_capabilities();
        const auto* stats = runtime_.acceleration_stats();
        char system_info[420]{};
        std::snprintf(system_info, sizeof(system_info),
            "SYSTEM: %s | AVX2 %s (kernel %s) | SIMD %s | WORKERS %u | GPU N/A | %s",
            hardware.architecture.c_str(), hardware.avx2_cpu_supported ? "YES" : "NO",
            hardware.avx2_kernel_compiled ? "YES" : "NO",
            hcs::simd_implementation_name(runtime_.simd_implementation()),
            runtime_.worker_thread_count(), hcs::editor::rate_name(controller_.rate()));
        std::string system_line{system_info};
        if (stats && (stats->simd_batches != 0 || stats->parallel_region_batches != 0)) {
            char counters[112]{};
            std::snprintf(counters, sizeof(counters), " | SIMD %llu | PAR %llu",
                static_cast<unsigned long long>(stats->simd_batches),
                static_cast<unsigned long long>(stats->parallel_region_batches));
            system_line += counters;
        }
        CircuitRenderer::text(renderer_, 12.0F, static_cast<float>(height) - 20.0F,
                              system_line.c_str(), 0.60F, kMuted);
        render_library_panel(width, height);
        render_overlays(renderer_, width, height);
        SDL_RenderPresent(renderer_);
    }

    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    bool running_{true};
    bool show_grid_{true};
    bool panning_{false};
    std::uint8_t pan_button_{0};
    bool pan_moved_{false};
    bool drag_moved_{false};
    std::optional<ComponentType> placement_;
    std::optional<std::string> placement_custom_;
    std::optional<PinId> wire_start_;
    std::optional<ComponentId> dragging_component_;
    std::optional<ComponentId> click_toggle_candidate_;
    Vec2 wire_cursor_{};
    Vec2 last_mouse_{};
    Vec2 drag_offset_{};
    Vec2 mouse_down_world_{};
    CircuitEditor circuit_;
    hcs::editor::ComponentLibrary library_;
    std::optional<hcs::editor::CustomComponentDefinition> editing_definition_;
    std::optional<CircuitEditor> root_before_definition_edit_;
    std::string root_document_name_before_definition_edit_;
    std::filesystem::path root_current_path_before_definition_edit_;
    std::uint64_t root_saved_revision_before_definition_edit_{0};
    std::string root_text_buffer_before_definition_edit_;
    bool root_text_buffer_dirty_before_definition_edit_{false};
    bool definition_metadata_dirty_{false};
    bool symbol_designer_mode_{false};
    bool symbol_text_mode_{false};
    bool symbol_text_buffer_dirty_{false};
    bool symbol_snap_{true};
    bool symbol_vertex_edit_mode_{false};
    SymbolTool symbol_tool_{SymbolTool::Select};
    SymbolDrag symbol_drag_{SymbolDrag::None};
    float symbol_zoom_factor_{1.0F};
    Vec2 symbol_pan_offset_{};
    Vec2 symbol_drag_start_{};
    Vec2 symbol_drag_last_{};
    Vec2 symbol_drag_screen_{};
    std::vector<hcs::editor::SymbolPoint> symbol_pending_points_;
    Vec2 symbol_cursor_local_{};
    bool symbol_cursor_valid_{false};
    std::optional<std::string> selected_symbol_object_id_;
    std::optional<std::string> selected_symbol_port_id_;
    std::optional<std::size_t> selected_symbol_vertex_index_;
    std::size_t symbol_preset_index_{0};
    std::size_t next_input_port_serial_{1};
    std::size_t next_output_port_serial_{1};
    std::string symbol_text_buffer_;
    std::size_t symbol_text_cursor_{0};
    std::optional<std::size_t> symbol_text_selection_anchor_;
    std::string symbol_text_error_;
    std::string pending_port_id_;
    bool library_panel_{false};
    std::string library_search_;
    std::size_t library_category_index_{0};
    CircuitRuntime runtime_{RuntimeBackend::OptimizedIREvent};
    SimulationController controller_{runtime_};
    Camera2D camera_;

    PathMode path_mode_{PathMode::None};
    DeferredAction confirm_pending_{DeferredAction::None};
    DeferredAction after_save_action_{DeferredAction::None};
    bool overwrite_confirmation_{false};
    std::filesystem::path overwrite_path_;
    bool text_mode_{false};
    bool text_buffer_dirty_{false};
    std::string text_buffer_;
    std::size_t text_cursor_{0};
    std::optional<std::size_t> text_selection_anchor_;
    std::uint64_t text_base_revision_{0};
    std::string text_error_;
    std::string path_buffer_;
    std::string status_message_;
    std::filesystem::path current_path_;
    std::string document_name_{"Untitled"};
    std::uint64_t saved_document_revision_{0};
    std::string last_window_title_;
};
} // namespace

int main() {
    Application app;
    if (!app.initialize()) return 1;
    return app.run();
}
