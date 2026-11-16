#include "hcs/editor/custom_components.hpp"
#include "hcs/editor/persistence.hpp"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
void usage() {
    std::cout << "HCS circuit file tool\n"
                 "Usage:\n"
                 "  hcs_circuit validate <file.hcs> [--library-dir <directory>]\n"
                 "  hcs_circuit format <file.hcs> [output.hcs] [--library-dir <directory>]\n"
                 "  hcs_circuit print <file.hcs> [--library-dir <directory>]\n";
}

bool parse_library_directory(int argc, char** argv, int start,
                             std::filesystem::path& directory) {
    for (int i = start; i < argc; ++i) {
        if (std::string_view(argv[i]) != "--library-dir" || i + 1 >= argc) return false;
        directory = argv[++i];
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const std::string command = argv[1];
    const std::filesystem::path input_path = argv[2];

    int option_start = 3;
    std::filesystem::path output_path;
    if (command == "format" && argc >= 4 && std::string_view(argv[3]) != "--library-dir") {
        output_path = argv[3];
        option_start = 4;
    }
    if (command != "validate" && command != "format" && command != "print") {
        usage();
        return 2;
    }
    if ((command == "validate" || command == "print") && argc != 3 && argc != 5) {
        usage();
        return 2;
    }
    if (command == "format" && ((output_path.empty() && argc != 3 && argc != 5) ||
                                  (!output_path.empty() && argc != option_start && argc != option_start + 2))) {
        usage();
        return 2;
    }

    auto library_directory = hcs::editor::ComponentLibrary::default_directory();
    if (!parse_library_directory(argc, argv, option_start, library_directory)) {
        usage();
        return 2;
    }

    hcs::editor::ComponentLibrary library;
    std::vector<std::string> library_diagnostics;
    if (!library.load_directory(library_directory, library_diagnostics)) {
        for (const auto& diagnostic : library_diagnostics)
            std::cerr << "Component library error: " << diagnostic << '\n';
        return 1;
    }

    hcs::editor::CircuitEditor editor;
    std::string name;
    hcs::editor::PersistenceError error;
    if (!hcs::editor::load_circuit_file(input_path, editor, name, library, error)) {
        std::cerr << "Validation failed: " << error.describe() << '\n';
        return 1;
    }

    if (command == "validate") {
        std::cout << "Valid HCS circuit: " << input_path.string() << " ("
                  << editor.components().size() << " components, " << editor.wires().size() << " wires)\n";
        return 0;
    }

    std::string canonical;
    if (!hcs::editor::serialize_circuit(editor, name, library, canonical, error)) {
        std::cerr << "Formatting failed: " << error.describe() << '\n';
        return 1;
    }
    if (command == "print" || (command == "format" && output_path.empty())) {
        std::cout << canonical;
        return std::cout.good() ? 0 : 1;
    }
    if (command == "format" && !output_path.empty()) {
        if (!hcs::editor::save_circuit_file(output_path, editor, name, library, error)) {
            std::cerr << "Formatting failed: " << error.describe() << '\n';
            return 1;
        }
        std::cout << "Formatted circuit written to " << output_path.string() << '\n';
        return 0;
    }
    usage();
    return 2;
}
