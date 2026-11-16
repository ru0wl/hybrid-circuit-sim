#pragma once

#include "hcs/editor/model.hpp"
#include "hcs/editor/persistence.hpp"
#include "hcs/editor/symbol.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hcs::editor {

inline constexpr unsigned kCustomComponentFormatVersion = 1;
inline constexpr std::string_view kCustomComponentFileExtension = ".hcscomponent";

struct CustomPort {
    std::string id;
    std::string name;
    PinDirection direction{PinDirection::Input};
    ComponentId terminal_component{kInvalidComponent};
};

struct CustomComponentDefinition {
    std::string id;
    std::string name;
    std::string description;
    std::string category{"Custom"};
    unsigned version{1};
    Vec2 size{150.0F, 84.0F};
    std::vector<CustomPort> ports;
    CircuitEditor implementation;
    SymbolDefinition symbol;
};

struct ComponentMetadata {
    std::string id;
    std::string name;
    std::string category;
    std::string description;
    bool built_in{false};
    std::size_t input_count{0};
    std::size_t output_count{0};
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
};

class ComponentLibrary final {
public:
    [[nodiscard]] bool add(CustomComponentDefinition definition, PersistenceError& error);
    [[nodiscard]] bool replace(CustomComponentDefinition definition, PersistenceError& error);
    [[nodiscard]] bool erase(std::string_view id);
    [[nodiscard]] const CustomComponentDefinition* find(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<const CustomComponentDefinition*> definitions() const;
    [[nodiscard]] std::vector<ComponentMetadata> catalog() const;
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] std::uint64_t logic_revision() const noexcept { return logic_revision_; }

    [[nodiscard]] static std::filesystem::path default_directory();
    [[nodiscard]] bool load_directory(const std::filesystem::path& directory,
                                      std::vector<std::string>& diagnostics);
    [[nodiscard]] bool save_definition(const std::filesystem::path& directory,
                                       std::string_view id, PersistenceError& error) const;

private:
    std::map<std::string, CustomComponentDefinition, std::less<>> definitions_;
    std::uint64_t revision_{1};
    std::uint64_t logic_revision_{1};
};

struct HierarchyLimits {
    std::size_t maximum_depth{16};
    std::size_t maximum_expanded_components{100'000};
    std::size_t maximum_dependencies{4096};
};

struct HierarchyError {
    std::string definition_id;
    ComponentId instance{kInvalidComponent};
    std::string message;

    [[nodiscard]] std::string describe() const;
};

struct ExpandedCircuit {
    CircuitEditor circuit;
    std::map<PinId, PinId> root_pin_to_expanded_pin;
    std::map<ComponentId, ComponentId> root_input_to_expanded_input;
};

[[nodiscard]] bool validate_component_hierarchy(const CircuitEditor& root,
                                                const ComponentLibrary& library,
                                                const HierarchyLimits& limits,
                                                HierarchyError& error);
[[nodiscard]] bool expand_component_hierarchy(const CircuitEditor& root,
                                              const ComponentLibrary& library,
                                              ExpandedCircuit& expanded,
                                              HierarchyError& error,
                                              const HierarchyLimits& limits = {});

[[nodiscard]] bool serialize_component_definition(const CustomComponentDefinition& definition,
                                                  const ComponentLibrary& library,
                                                  std::string& text, PersistenceError& error);
[[nodiscard]] bool parse_component_definition(std::string_view text,
                                              const ComponentLibrary& library,
                                              CustomComponentDefinition& destination,
                                              PersistenceError& error);
[[nodiscard]] bool save_component_definition_file(const std::filesystem::path& path,
                                                  const CustomComponentDefinition& definition,
                                                  const ComponentLibrary& library,
                                                  PersistenceError& error);
[[nodiscard]] bool load_component_definition_file(const std::filesystem::path& path,
                                                  const ComponentLibrary& library,
                                                  CustomComponentDefinition& destination,
                                                  PersistenceError& error);

// Extracts a region and replaces it with one library-backed instance. The caller
// supplies the selected component IDs; the source editor is changed only on success.
[[nodiscard]] bool create_custom_component_from_selection(
    CircuitEditor& root, const std::vector<ComponentId>& selection,
    CustomComponentDefinition definition, ComponentLibrary& library,
    ComponentId& created_instance, PersistenceError& error);

// Create/edit stable external port records. Port IDs are independent from display names;
// renaming a port never changes its connectivity identifier.
[[nodiscard]] bool create_empty_component_definition(std::string id, std::string name,
                                                      std::string category,
                                                      CustomComponentDefinition& definition,
                                                      PersistenceError& error);
[[nodiscard]] bool add_custom_component_port(CustomComponentDefinition& definition,
                                              std::string id, std::string name,
                                              PinDirection direction, PersistenceError& error);
[[nodiscard]] bool rename_custom_component_port(CustomComponentDefinition& definition,
                                                 std::string_view id, std::string name,
                                                 PersistenceError& error);
[[nodiscard]] bool remove_custom_component_port(CustomComponentDefinition& definition,
                                                 std::string_view id, PersistenceError& error);

// Atomically applies a new or edited definition and migrates every root-level instance
// by stable port ID. Connected removed/retyped ports and invalid dependency graphs are rejected.
[[nodiscard]] bool apply_component_definition_transactionally(
    ComponentLibrary& library, CircuitEditor& root,
    CustomComponentDefinition definition, PersistenceError& error);

[[nodiscard]] std::string make_definition_id(std::string_view name);
[[nodiscard]] std::vector<CustomPortLayout> custom_component_port_layouts(const CustomComponentDefinition& definition);

} // namespace hcs::editor
