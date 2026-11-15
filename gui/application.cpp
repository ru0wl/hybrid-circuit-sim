#include "renderer.hpp"

#include "hcs/editor/simulation_controller.hpp"
#include "hcs/editor/persistence.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>

namespace {
using hcs::editor::Camera2D;
using hcs::editor::CircuitEditor;
using hcs::editor::CircuitRuntime;
using hcs::editor::ComponentId;
using hcs::editor::ComponentType;
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
constexpr float kStatusHeight = 28.0F;
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
    const char* label;
    enum class Action : std::uint8_t {
        PlaceInput, PlaceOutput, PlaceAnd, PlaceOr, PlaceNot, PlaceXor, PlaceDff,
        RunPause, Step, Reset, NextBackend, NextRate, ToggleGrid, Demo,
        ToggleSimd, ToggleParallel, CycleWorkers, GpuInfo,
        NewCircuit, OpenCircuit, SaveCircuit, SaveCircuitAs, TextEdit, ApplyText, ReloadText, BackToGraph,
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
        SDL_SetWindowMinimumSize(window_, 960, 600);
        update_viewport();
        load_demo();
        update_window_title();
        return true;
    }

    ~Application() {
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
                runtime_.synchronize(circuit_);
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
    enum class PathMode : std::uint8_t { None, Open, SaveAs };
    enum class DeferredAction : std::uint8_t { None, NewCircuit, OpenCircuit, Demo, Quit };

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
        runtime_.synchronize(circuit_);
        controller_.reset(circuit_);
        controller_.notify_input_changed();
        document_name_ = "HCS Demo";
        current_path_.clear();
        text_buffer_dirty_ = false;
        saved_document_revision_ = circuit_.document_revision();
        text_buffer_.clear();
        update_window_title();
    }

    bool dirty() const noexcept {
        return circuit_.document_revision() != saved_document_revision_;
    }

    std::string current_filename() const {
        return current_path_.empty() ? document_name_ : current_path_.filename().string();
    }

    void update_window_title() {
        if (!window_) return;
        std::string title = "HCS - " + current_filename();
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
                runtime_.synchronize(circuit_);
                controller_.reset(circuit_);
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
            case DeferredAction::None:
                break;
        }
    }

    void request_action(DeferredAction action) {
        if (action == DeferredAction::None) return;
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
        if (text_buffer_dirty_) {
            const bool was_text_mode = text_mode_;
            if (!apply_text()) return;
            if (was_text_mode) text_mode_ = true;
        }
        const auto action = confirm_pending_;
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
        path_buffer_ = (mode == PathMode::SaveAs && !current_path_.empty()) ? current_path_.string() : std::string{};
        status_message_.clear();
        SDL_StartTextInput(window_);
    }

    void cancel_path_entry() {
        path_mode_ = PathMode::None;
        SDL_StopTextInput(window_);
        path_buffer_.clear();
        after_save_action_ = DeferredAction::None;
    }

    void erase_path_character() {
        if (path_buffer_.empty()) return;
        std::size_t position = path_buffer_.size() - 1;
        while (position > 0 && (static_cast<unsigned char>(path_buffer_[position]) & 0xC0U) == 0x80U) --position;
        path_buffer_.erase(position);
    }

    bool save_to_path(std::filesystem::path path) {
        if (path.extension().empty()) path += std::string(hcs::editor::kCircuitFileExtension);
        hcs::editor::PersistenceError error;
        if (!hcs::editor::save_circuit_file(path, circuit_, document_name_, error)) {
            set_status("Save failed: " + error.describe());
            return false;
        }
        current_path_ = std::move(path);
        saved_document_revision_ = circuit_.document_revision();
        set_status("Saved " + current_path_.string());
        return true;
    }

    void save_current_or_prompt() {
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
        if (mode == PathMode::Open) {
            hcs::editor::PersistenceError error;
            std::string loaded_name;
            if (!hcs::editor::load_circuit_file(path, circuit_, loaded_name, error)) {
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
            runtime_.synchronize(circuit_);
            controller_.reset(circuit_);
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
        if (!hcs::editor::serialize_circuit(circuit_, document_name_, generated, error)) {
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
        if (!hcs::editor::parse_circuit(text_buffer_, circuit_, new_name, error)) {
            text_error_ = "Apply failed: " + error.describe();
            return false;
        }
        document_name_ = std::move(new_name);
        runtime_.synchronize(circuit_);
        controller_.reset(circuit_);
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
        } else if (text_mode_ && confirm_pending_ == DeferredAction::None) {
            insert_text(value);
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
        if (control && (event.key == SDLK_s)) {
            if (apply_text()) save_current_or_prompt();
            return;
        }
        if (control && (event.key == SDLK_r)) { reload_text_from_model(); return; }
        if (control && (event.key == SDLK_t)) { leave_text_mode(); return; }
        if (control && (event.key == SDLK_a)) {
            text_selection_anchor_ = 0;
            text_cursor_ = text_buffer_.size();
            return;
        }
        if (control && (event.key == SDLK_c || event.key == SDLK_x)) {
            if (text_selection_anchor_) {
                const auto first = std::min(*text_selection_anchor_, text_cursor_);
                const auto last = std::max(*text_selection_anchor_, text_cursor_);
                const auto selected = text_buffer_.substr(first, last - first);
                (void)SDL_SetClipboardText(selected.c_str());
                if (event.key == SDLK_x) { erase_text_selection(); text_buffer_dirty_ = true; }
            }
            return;
        }
        if (control && (event.key == SDLK_v)) {
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
        std::size_t cursor_column = 0;
        for (std::size_t i = 0; i < text_cursor_ && i < text_buffer_.size(); ++i) {
            if (text_buffer_[i] == '\n') { ++cursor_line; cursor_column = 0; }
            else if ((static_cast<unsigned char>(text_buffer_[i]) & 0xC0U) != 0x80U) ++cursor_column;
        }
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
                const auto max_chars = width > 100 ? static_cast<std::size_t>((width - 110) / 6) : 1U;
                const auto visible = source.substr(0, max_chars);
                CircuitRenderer::text(renderer_, 64.0F, y, visible.c_str(), 0.9F, kText);
                if (line_number == cursor_line && cursor_column <= max_chars) {
                    const float cursor_x = 64.0F + static_cast<float>(cursor_column) * 5.4F;
                    CircuitRenderer::line(renderer_, {cursor_x, y - 1.0F}, {cursor_x, y + 9.0F}, kLogicOne);
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
                path_mode_ == PathMode::Open ? "OPEN HCS FILE - ENTER PATH" : "SAVE HCS FILE AS - ENTER PATH", 1.0F, kText);
            CircuitRenderer::text(target, 145.0F, static_cast<float>(height) * 0.5F - 8.0F,
                                  path_buffer_.c_str(), 1.0F, kLogicOne);
            CircuitRenderer::text(target, 145.0F, static_cast<float>(height) * 0.5F + 35.0F,
                                  "ENTER CONFIRM  ESC CANCEL  EXTENSION .HCS IS ADDED WHEN MISSING", 0.8F, kMuted);
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

    std::vector<Button> buttons() const {
        constexpr float y = 10.0F;
        constexpr float h = 27.0F;
        constexpr float settings_y = 41.0F;
        if (text_mode_) {
            return {
                {{10, y, 106, h}, "APPLY TEXT", Button::Action::ApplyText},
                {{122, y, 106, h}, "RELOAD TEXT", Button::Action::ReloadText},
                {{234, y, 106, h}, "GRAPH VIEW", Button::Action::BackToGraph},
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
            {{682, y, 204, h}, "BACKEND", Button::Action::NextBackend},
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
        };
    }

    void handle_button(Button::Action action) {
        switch (action) {
            case Button::Action::PlaceInput: placement_ = ComponentType::Input; break;
            case Button::Action::PlaceOutput: placement_ = ComponentType::Output; break;
            case Button::Action::PlaceAnd: placement_ = ComponentType::And; break;
            case Button::Action::PlaceOr: placement_ = ComponentType::Or; break;
            case Button::Action::PlaceNot: placement_ = ComponentType::Not; break;
            case Button::Action::PlaceXor: placement_ = ComponentType::Xor; break;
            case Button::Action::PlaceDff: placement_ = ComponentType::DFlipFlop; break;
            case Button::Action::RunPause: controller_.toggle_running(); break;
            case Button::Action::Step: (void)controller_.step_once(); break;
            case Button::Action::Reset: controller_.reset(circuit_); break;
            case Button::Action::NextBackend: {
                constexpr std::array choices{RuntimeBackend::InterpretedEvent, RuntimeBackend::InterpretedDense,
                    RuntimeBackend::OptimizedIRDense, RuntimeBackend::OptimizedIREvent,
                    RuntimeBackend::HybridIR, RuntimeBackend::CompiledNative};
                const auto found = std::find(choices.begin(), choices.end(), runtime_.backend());
                const auto next = found == choices.end() || std::next(found) == choices.end() ? choices.front() : *std::next(found);
                runtime_.set_backend(next);
                try { runtime_.synchronize(circuit_); controller_.notify_input_changed(); }
                catch (const std::exception& error) { std::fprintf(stderr, "Backend change failed: %s\n", error.what()); }
                break;
            }
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
        }
    }

    void apply_acceleration(const hcs::AccelerationConfig& acceleration) {
        const auto previous = runtime_.acceleration();
        runtime_.set_acceleration(acceleration);
        try {
            runtime_.synchronize(circuit_);
            controller_.notify_input_changed();
        } catch (const std::exception& error) {
            runtime_.set_acceleration(previous);
            try { runtime_.synchronize(circuit_); } catch (...) {}
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
                    if (text_mode_ || path_mode_ != PathMode::None || confirm_pending_ != DeferredAction::None || overwrite_confirmation_) break;
                    const Vec2 pointer{event.wheel.mouse_x, event.wheel.mouse_y};
                    if (inside_viewport(pointer)) {
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
        if (event.button == SDL_BUTTON_MIDDLE) {
            if (path_mode_ == PathMode::None && confirm_pending_ == DeferredAction::None &&
                !overwrite_confirmation_ && !text_mode_ && inside_viewport(screen)) {
                panning_ = true;
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
        if (text_mode_ || !inside_viewport(screen)) return;

        const auto world = camera_.screen_to_world(screen);
        if (placement_) {
            (void)circuit_.add_component(*placement_, world);
            placement_.reset();
            try { runtime_.synchronize(circuit_); controller_.notify_input_changed(); }
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

        if (const auto component = circuit_.component_at(world)) {
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
        circuit_.clear_selection();
    }

    void on_mouse_up(const SDL_MouseButtonEvent& event) {
        if (event.button == SDL_BUTTON_MIDDLE) {
            panning_ = false;
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
                    try { runtime_.synchronize(circuit_); controller_.notify_input_changed(); }
                    catch (const std::exception& error) { std::fprintf(stderr, "Wire synchronization failed: %s\n", error.what()); }
                }
            }
            wire_start_.reset();
        }
        if (click_toggle_candidate_ && !drag_moved_) {
            if (circuit_.toggle_input(*click_toggle_candidate_)) {
                runtime_.synchronize(circuit_);
                controller_.notify_input_changed();
            }
        }
        click_toggle_candidate_.reset();
        dragging_component_.reset();
        drag_moved_ = false;
    }

    void on_mouse_motion(const SDL_MouseMotionEvent& event) {
        if (text_mode_ || path_mode_ != PathMode::None || confirm_pending_ != DeferredAction::None || overwrite_confirmation_) return;
        const Vec2 screen{event.x, event.y};
        if (panning_) {
            camera_.pan_screen(screen - last_mouse_);
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
            if (event.key == SDLK_ESCAPE || event.key == SDLK_n) {
                overwrite_confirmation_ = false;
                after_save_action_ = DeferredAction::None;
            } else if (event.key == SDLK_y || event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) {
                overwrite_confirmation_ = false;
                complete_save_as(overwrite_path_);
            }
            return;
        }
        if (confirm_pending_ != DeferredAction::None) {
            if (event.key == SDLK_ESCAPE) cancel_confirmation();
            else if (event.key == SDLK_y) confirm_save_and_continue();
            else if (event.key == SDLK_n) confirm_discard_and_continue();
            return;
        }
        if (path_mode_ != PathMode::None) {
            if (event.key == SDLK_ESCAPE) cancel_path_entry();
            else if (event.key == SDLK_RETURN || event.key == SDLK_KP_ENTER) finish_path_entry();
            else if (event.key == SDLK_BACKSPACE) erase_path_character();
            else if (control && (event.key == SDLK_a)) path_buffer_.clear();
            return;
        }
        if (text_mode_) {
            handle_text_key(event, control, shift);
            return;
        }

        if (control && (event.key == SDLK_n)) request_action(DeferredAction::NewCircuit);
        else if (control && (event.key == SDLK_o)) request_action(DeferredAction::OpenCircuit);
        else if (control && (event.key == SDLK_s)) {
            if (shift) begin_path_entry(PathMode::SaveAs);
            else save_current_or_prompt();
        } else if (control && (event.key == SDLK_t)) enter_text_mode();
        else if (event.key == SDLK_ESCAPE) {
            if (placement_ || wire_start_) {
                placement_.reset();
                wire_start_.reset();
            } else {
                circuit_.clear_selection();
            }
        } else if (event.key == SDLK_DELETE || event.key == SDLK_BACKSPACE) {
            bool changed = false;
            if (circuit_.selection().component) changed = circuit_.remove_component(*circuit_.selection().component);
            else if (circuit_.selection().wire) changed = circuit_.remove_wire(*circuit_.selection().wire);
            if (changed) {
                runtime_.synchronize(circuit_);
                controller_.notify_input_changed();
            }
        } else if (event.key == SDLK_SPACE) {
            controller_.toggle_running();
        }
    }

    void render_button(const Button& button) {
        const auto active = (button.action == Button::Action::RunPause && controller_.running()) ||
                            (button.action == Button::Action::ToggleGrid && show_grid_);
        const auto color = active ? kButtonActive : kButton;
        CircuitRenderer::fill_rect(renderer_, button.rect.x, button.rect.y, button.rect.width, button.rect.height, color);
        CircuitRenderer::outline_rect(renderer_, button.rect.x, button.rect.y, button.rect.width, button.rect.height, kButtonBorder);
        CircuitRenderer::text(renderer_, button.rect.x + 7.0F, button.rect.y + 9.0F, button.label, 1.05F, kText);
    }

    void render() {
        if (text_mode_) {
            render_text_mode();
            return;
        }
        CircuitRenderer renderer;
        renderer.render(renderer_, circuit_, runtime_, camera_, show_grid_,
            wire_start_ ? std::optional<hcs::gui::WirePreview>{hcs::gui::WirePreview{circuit_.pin_world_position(*wire_start_), wire_cursor_}} : std::nullopt);

        int width = kInitialWidth;
        int height = kInitialHeight;
        (void)SDL_GetWindowSize(window_, &width, &height);
        CircuitRenderer::fill_rect(renderer_, 0.0F, 0.0F, static_cast<float>(width), kToolbarHeight, kToolbar);
        CircuitRenderer::line(renderer_, {0.0F, kToolbarHeight}, {static_cast<float>(width), kToolbarHeight}, kButtonBorder);
        for (const auto& button : buttons()) render_button(button);

        const auto hardware = hcs::detect_hardware_capabilities();
        char acceleration_label[640]{};
        const auto* stats = runtime_.acceleration_stats();
        std::snprintf(acceleration_label, sizeof(acceleration_label),
            "CPU %s | AVX2 CPU %s / kernel %s (active %s) | workers %u | GPU unavailable | backend %s | %s",
            hardware.architecture.c_str(), hardware.avx2_cpu_supported ? "yes" : "no",
            hardware.avx2_kernel_compiled ? "yes" : "no",
            hcs::simd_implementation_name(runtime_.simd_implementation()),
            runtime_.worker_thread_count(), hcs::editor::backend_name(runtime_.backend()),
            hcs::editor::rate_name(controller_.rate()));
        CircuitRenderer::text(renderer_, 795.0F, 49.0F, acceleration_label, 0.58F, kMuted);
        if (stats && (stats->simd_batches != 0 || stats->parallel_region_batches != 0)) {
            char acceleration_stats[180]{};
            std::snprintf(acceleration_stats, sizeof(acceleration_stats), "SIMD batches %llu | parallel regions %llu",
                static_cast<unsigned long long>(stats->simd_batches),
                static_cast<unsigned long long>(stats->parallel_regions));
            CircuitRenderer::text(renderer_, 795.0F, 64.0F, acceleration_stats, 0.68F, kMuted);
        }
        if (placement_) CircuitRenderer::text(renderer_, 1210.0F, 18.0F, "CLICK CANVAS TO PLACE", 0.9F, kLogicOne);
        if (wire_start_) CircuitRenderer::text(renderer_, 1210.0F, 48.0F, "CONNECT TO INPUT PIN", 0.9F, kSelection);

        CircuitRenderer::fill_rect(renderer_, 0.0F, static_cast<float>(height) - kStatusHeight,
                                   static_cast<float>(width), kStatusHeight, kToolbar);
        CircuitRenderer::line(renderer_, {0.0F, static_cast<float>(height) - kStatusHeight},
                              {static_cast<float>(width), static_cast<float>(height) - kStatusHeight}, kButtonBorder);
        char status[384]{};
        std::snprintf(status, sizeof(status), "%s%s   %s   TICK: %llu   COMPONENTS: %zu   WIRES: %zu   ZOOM: %.2FX   CTRL+S SAVE   CTRL+T TEXT",
                      dirty() || text_buffer_dirty_ ? "* " : "", current_filename().c_str(),
                      controller_.running() ? "RUNNING" : "PAUSED",
                      static_cast<unsigned long long>(runtime_.tick_count()), circuit_.components().size(),
                      circuit_.wires().size(), static_cast<double>(camera_.zoom()));
        CircuitRenderer::text(renderer_, 12.0F, static_cast<float>(height) - 19.0F, status, 0.8F, kMuted);
        render_overlays(renderer_, width, height);
        SDL_RenderPresent(renderer_);
    }

    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    bool running_{true};
    bool show_grid_{true};
    bool panning_{false};
    bool drag_moved_{false};
    std::optional<ComponentType> placement_;
    std::optional<PinId> wire_start_;
    std::optional<ComponentId> dragging_component_;
    std::optional<ComponentId> click_toggle_candidate_;
    Vec2 wire_cursor_{};
    Vec2 last_mouse_{};
    Vec2 drag_offset_{};
    Vec2 mouse_down_world_{};
    CircuitEditor circuit_;
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
