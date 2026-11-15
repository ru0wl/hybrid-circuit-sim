#include "hcs/editor/persistence.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
void usage() {
    std::cout << "HCS circuit file tool\n"
                 "Usage:\n"
                 "  hcs_circuit validate <file.hcs>\n"
                 "  hcs_circuit format <file.hcs> [output.hcs]\n"
                 "  hcs_circuit print <file.hcs>\n";
}
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    const std::string command = argv[1];
    const std::filesystem::path input_path = argv[2];

    hcs::editor::CircuitEditor editor;
    std::string name;
    hcs::editor::PersistenceError error;
    if (!hcs::editor::load_circuit_file(input_path, editor, name, error)) {
        std::cerr << "Validation failed: " << error.describe() << '\n';
        return 1;
    }

    if (command == "validate") {
        if (argc != 3) { usage(); return 2; }
        std::cout << "Valid HCS circuit: " << input_path.string() << " ("
                  << editor.components().size() << " components, " << editor.wires().size() << " wires)\n";
        return 0;
    }

    std::string canonical;
    if (!hcs::editor::serialize_circuit(editor, name, canonical, error)) {
        std::cerr << "Formatting failed: " << error.describe() << '\n';
        return 1;
    }
    if (command == "print" || (command == "format" && argc == 3)) {
        if (command == "print" && argc != 3) { usage(); return 2; }
        std::cout << canonical;
        return std::cout.good() ? 0 : 1;
    }
    if (command == "format" && argc == 4) {
        const std::filesystem::path output_path = argv[3];
        if (!hcs::editor::save_circuit_file(output_path, editor, name, error)) {
            std::cerr << "Formatting failed: " << error.describe() << '\n';
            return 1;
        }
        std::cout << "Formatted circuit written to " << output_path.string() << '\n';
        return 0;
    }
    usage();
    return 2;
}
