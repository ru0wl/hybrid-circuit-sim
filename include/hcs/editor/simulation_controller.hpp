#pragma once

#include "hcs/editor/runtime.hpp"

#include <cstddef>
#include <cstdint>

namespace hcs::editor {

enum class SimulationRate : std::uint8_t { Hz1, Hz10, Hz60, Hz1000, Unlimited };

class SimulationController final {
public:
    explicit SimulationController(CircuitRuntime& runtime) noexcept;

    void update(double delta_seconds);
    [[nodiscard]] StepResult step_once();
    void pause() noexcept;
    void resume() noexcept;
    void toggle_running() noexcept;
    void reset(CircuitEditor& editor);
    void notify_input_changed() noexcept;

    void set_rate(SimulationRate rate) noexcept;
    [[nodiscard]] SimulationRate rate() const noexcept { return rate_; }
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::uint64_t total_steps() const noexcept { return total_steps_; }
    void set_unlimited_time_slice(double seconds) noexcept;

private:
    [[nodiscard]] static double rate_hz(SimulationRate rate) noexcept;
    void perform_step();

    CircuitRuntime& runtime_;
    SimulationRate rate_{SimulationRate::Hz60};
    bool running_{false};
    bool pending_input_settle_{false};
    double accumulator_{0.0};
    double unlimited_time_slice_seconds_{0.002};
    std::uint64_t total_steps_{0};
};

[[nodiscard]] const char* rate_name(SimulationRate rate) noexcept;

} // namespace hcs::editor
