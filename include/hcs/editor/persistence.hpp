#pragma once

#include "hcs/editor/model.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace hcs::editor {

inline constexpr unsigned kCircuitFormatVersion = 1;
inline constexpr std::string_view kCircuitFileExtension = ".hcs";

struct PersistenceError {
    std::size_t line{0};
    std::string message;

    [[nodiscard]] std::string describe() const;
};

// Convert an editor document to the canonical, deterministic HCS text format.
[[nodiscard]] bool serialize_circuit(const CircuitEditor& circuit, std::string_view circuit_name,
                                     std::string& text, PersistenceError& error);

// Parse and validate into a temporary model. The destination is replaced only after
// the complete document has passed parsing, reference validation and model creation.
[[nodiscard]] bool parse_circuit(std::string_view text, CircuitEditor& destination,
                                 std::string& circuit_name, PersistenceError& error);

[[nodiscard]] bool save_circuit_file(const std::filesystem::path& path,
                                     const CircuitEditor& circuit, std::string_view circuit_name,
                                     PersistenceError& error);
[[nodiscard]] bool load_circuit_file(const std::filesystem::path& path,
                                     CircuitEditor& destination, std::string& circuit_name,
                                     PersistenceError& error);

} // namespace hcs::editor
