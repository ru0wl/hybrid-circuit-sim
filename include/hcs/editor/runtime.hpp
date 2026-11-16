#pragma once

#include "hcs/editor/model.hpp"
#include "hcs/simulator.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace hcs::editor {

class ComponentLibrary;

enum class RuntimeBackend : std::uint8_t {
    InterpretedEvent,
    InterpretedDense,
    OptimizedIRDense,
    OptimizedIREvent,
    HybridIR,
    CompiledNative,
};

class CircuitRuntime final {
public:
    CircuitRuntime();
    explicit CircuitRuntime(RuntimeBackend backend);

    // Rebuilds the immutable HCS circuit only when the editor's structural revision
    // or selected backend changes. Geometry, selection and input values do not rebuild it.
    void synchronize(CircuitEditor& editor);
    void synchronize(CircuitEditor& editor, const ComponentLibrary& library);
    void set_backend(RuntimeBackend backend) noexcept;
    void invalidate() noexcept;
    [[nodiscard]] RuntimeBackend backend() const noexcept { return backend_; }
    void set_acceleration(const AccelerationConfig& config) noexcept;
    [[nodiscard]] const AccelerationConfig& acceleration() const noexcept { return acceleration_; }
    [[nodiscard]] SimdImplementation simd_implementation() const noexcept;
    [[nodiscard]] std::uint32_t worker_thread_count() const noexcept;
    [[nodiscard]] const AccelerationStats* acceleration_stats() const noexcept;

    [[nodiscard]] StepResult step();
    void reset(CircuitEditor& editor);
    void reset(CircuitEditor& editor, const ComponentLibrary& library);

    [[nodiscard]] SignalState signal_for_pin(PinId pin) const noexcept;
    [[nodiscard]] bool binary_signal_for_pin(PinId pin) const noexcept;
    [[nodiscard]] std::uint64_t tick_count() const noexcept;
    [[nodiscard]] bool ready() const noexcept { return simulator_ != nullptr; }
    [[nodiscard]] const StepResult& last_result() const noexcept { return last_result_; }
    [[nodiscard]] const Circuit* circuit() const noexcept { return circuit_.get(); }

private:
    struct InputBinding {
        ComponentId component{kInvalidComponent};
        SignalId signal{kInvalidSignal};
        bool last_value{false};
        bool initialized{false};
    };

    [[nodiscard]] SimulatorConfig make_config() const;
    void rebuild(CircuitEditor& editor);
    void synchronize_inputs(const CircuitEditor& editor);

    RuntimeBackend backend_{RuntimeBackend::OptimizedIREvent};
    AccelerationConfig acceleration_{};
    std::unique_ptr<Circuit> circuit_;
    std::unique_ptr<Simulator> simulator_;
    std::uint64_t synchronized_revision_{0};
    std::uint64_t synchronized_input_revision_{0};
    std::uint64_t synchronized_library_revision_{0};
    std::unordered_map<PinId, SignalId> pin_signals_;
    std::vector<InputBinding> input_bindings_;
    StepResult last_result_{};
};

[[nodiscard]] const char* backend_name(RuntimeBackend backend) noexcept;

} // namespace hcs::editor
