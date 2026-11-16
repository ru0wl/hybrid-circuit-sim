#include "hcs/editor/simulation_controller.hpp"
#include "hcs/editor/custom_components.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace hcs::editor {

SimulationController::SimulationController(CircuitRuntime& runtime) noexcept : runtime_(runtime) {}

void SimulationController::perform_step() {
    (void)runtime_.step();
    ++total_steps_;
    pending_input_settle_ = false;
}

void SimulationController::update(double delta_seconds) {
    const auto dt = std::clamp(delta_seconds, 0.0, 1.0);
    if (!running_) {
        if (pending_input_settle_) perform_step();
        return;
    }

    if (rate_ == SimulationRate::Unlimited) {
        const auto start = std::chrono::steady_clock::now();
        const auto budget = std::chrono::duration<double>(unlimited_time_slice_seconds_);
        do {
            perform_step();
        } while (std::chrono::steady_clock::now() - start < budget);
        return;
    }

    const auto hz = rate_hz(rate_);
    const auto interval = 1.0 / hz;
    accumulator_ += dt;
    constexpr std::size_t max_catch_up_steps = 10000;
    std::size_t steps = 0;
    while (accumulator_ >= interval && steps < max_catch_up_steps) {
        perform_step();
        accumulator_ -= interval;
        ++steps;
    }
    if (steps == max_catch_up_steps) accumulator_ = std::fmod(accumulator_, interval);
}

StepResult SimulationController::step_once() {
    const auto result = runtime_.step();
    ++total_steps_;
    pending_input_settle_ = false;
    return result;
}

void SimulationController::pause() noexcept { running_ = false; }
void SimulationController::resume() noexcept { running_ = true; }
void SimulationController::toggle_running() noexcept { running_ = !running_; }

void SimulationController::reset(CircuitEditor& editor) {
    running_ = false;
    accumulator_ = 0.0;
    total_steps_ = 0;
    pending_input_settle_ = false;
    runtime_.reset(editor);
    pending_input_settle_ = true;
}

void SimulationController::reset(CircuitEditor& editor, const ComponentLibrary& library) {
    running_ = false;
    accumulator_ = 0.0;
    total_steps_ = 0;
    pending_input_settle_ = false;
    runtime_.reset(editor, library);
    pending_input_settle_ = true;
}

void SimulationController::notify_input_changed() noexcept { pending_input_settle_ = true; }

void SimulationController::set_rate(SimulationRate rate) noexcept {
    rate_ = rate;
    accumulator_ = 0.0;
}

void SimulationController::set_unlimited_time_slice(double seconds) noexcept {
    unlimited_time_slice_seconds_ = std::clamp(seconds, 0.0001, 0.05);
}

double SimulationController::rate_hz(SimulationRate rate) noexcept {
    switch (rate) {
        case SimulationRate::Hz1: return 1.0;
        case SimulationRate::Hz10: return 10.0;
        case SimulationRate::Hz60: return 60.0;
        case SimulationRate::Hz1000: return 1000.0;
        case SimulationRate::Unlimited: return 0.0;
    }
    return 60.0;
}

const char* rate_name(SimulationRate rate) noexcept {
    switch (rate) {
        case SimulationRate::Hz1: return "1 HZ";
        case SimulationRate::Hz10: return "10 HZ";
        case SimulationRate::Hz60: return "60 HZ";
        case SimulationRate::Hz1000: return "1 KHZ";
        case SimulationRate::Unlimited: return "UNLIMITED";
    }
    return "UNKNOWN";
}

} // namespace hcs::editor
