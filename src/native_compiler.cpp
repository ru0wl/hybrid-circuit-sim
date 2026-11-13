#include "hcs/native_compiler.hpp"
#include "hcs/compiler_pipeline.hpp"

#include <algorithm>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace hcs {
namespace {

constexpr std::uint32_t kNativeAbiVersion = 1;
using SteadyClock = std::chrono::steady_clock;
double milliseconds(SteadyClock::time_point begin, SteadyClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string hex_hash(std::uint64_t hash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

std::filesystem::path default_cache_directory() {
    if (const char* override_path = std::getenv("HCS_CACHE_DIR")) return override_path;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME")) {
        return std::filesystem::path(xdg) / "hybrid-circuit-sim";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".cache" / "hybrid-circuit-sim";
    }
    return std::filesystem::temp_directory_path() / "hybrid-circuit-sim-cache";
}

std::string architecture_tag() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__i386__)
    return "i386";
#else
    return "unknown-arch";
#endif
}

std::string get_compiler_version(const std::string& compiler) {
    int descriptors[2]{};
    if (pipe(descriptors) != 0) {
        throw std::system_error(errno, std::generic_category(), "pipe failed");
    }
    const pid_t pid = fork();
    if (pid < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        throw std::system_error(errno, std::generic_category(), "fork failed");
    }
    if (pid == 0) {
        close(descriptors[0]);
        (void)dup2(descriptors[1], STDOUT_FILENO);
        (void)dup2(descriptors[1], STDERR_FILENO);
        close(descriptors[1]);
        execlp(compiler.c_str(), compiler.c_str(), "--version", static_cast<char*>(nullptr));
        _exit(127);
    }
    close(descriptors[1]);
    std::string output;
    char buffer[512];
    for (;;) {
        const auto count = read(descriptors[0], buffer, sizeof(buffer));
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            close(descriptors[0]);
            throw std::system_error(errno, std::generic_category(), "compiler version read failed");
        }
        if (output.size() < 4096) output.append(buffer, static_cast<std::size_t>(count));
    }
    close(descriptors[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid failed");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || output.empty()) {
        throw std::runtime_error("Cannot query compiler version for '" + compiler + "'");
    }
    const auto newline = output.find_first_of("\r\n");
    return output.substr(0, newline);
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("Cannot write file: " + path.string());
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) throw std::runtime_error("Failed writing file: " + path.string());
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    std::ostringstream data;
    data << file.rdbuf();
    return data.str();
}

std::uint64_t hash_file(const std::filesystem::path& path) {
    return fnv1a(read_file(path));
}

void run_compiler(const NativeCompileOptions& options,
                  const std::filesystem::path& source,
                  const std::filesystem::path& output) {
    const pid_t pid = fork();
    if (pid < 0) throw std::system_error(errno, std::generic_category(), "fork failed");
    if (pid == 0) {
        execlp(options.compiler.c_str(), options.compiler.c_str(),
               "-std=c++20", options.optimization_flag.c_str(), "-fPIC", "-shared",
               source.c_str(), "-o", output.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "waitpid failed");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("Native compilation failed using '" + options.compiler +
                                 "'. Check that a C++ compiler is installed.");
    }
}

std::string cache_manifest(const std::string& fingerprint,
                           const NativeCompileOptions& options,
                           const std::string& compiler_version,
                           std::uint64_t binary_hash) {
    std::ostringstream out;
    out << "HCS_NATIVE_CACHE\n"
        << "abi=" << kNativeAbiVersion << '\n'
        << "fingerprint=" << fingerprint << '\n'
        << "compiler=" << options.compiler << '\n'
        << "compiler_version=" << compiler_version << '\n'
        << "optimization=" << options.optimization_flag << '\n'
        << "architecture=" << architecture_tag() << '\n'
        << "binary_hash=" << hex_hash(binary_hash) << '\n';
    return out.str();
}

std::shared_ptr<NativeModule> open_module(const std::filesystem::path& path,
                                         bool cache_hit, NativeCompileTimings timings,
                                         SteadyClock::time_point total_start,
                                         std::vector<GateId> source_map,
                                         CompilerStatistics compiler_statistics) {
    const auto load_start = SteadyClock::now();
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* error = dlerror();
        throw std::runtime_error(std::string("dlopen failed: ") + (error ? error : "unknown error"));
    }
    dlerror();
    void* symbol = dlsym(handle, "hcs_eval");
    const char* error = dlerror();
    if (error != nullptr || symbol == nullptr) {
        dlclose(handle);
        throw std::runtime_error(std::string("Native module is missing hcs_eval: ") +
                                 (error ? error : "unknown error"));
    }
    timings.library_load_ms = milliseconds(load_start, SteadyClock::now());
    std::error_code size_error;
    timings.shared_library_bytes = std::filesystem::file_size(path, size_error);
    if (size_error) timings.shared_library_bytes = 0;
    timings.total_preparation_ms = milliseconds(total_start, SteadyClock::now());
    return std::make_shared<NativeModule>(handle,
        reinterpret_cast<NativeEvalFunction>(symbol), cache_hit, path, timings,
        std::move(source_map), compiler_statistics);
}

} // namespace

NativeModule::NativeModule(void* library_handle, NativeEvalFunction eval,
                           bool cache_hit, std::filesystem::path library_path,
                           NativeCompileTimings timings, std::vector<GateId> source_map,
                           CompilerStatistics compiler_statistics)
    : library_handle_(library_handle), eval_(eval), cache_hit_(cache_hit),
      library_path_(std::move(library_path)), timings_(timings),
      source_map_(std::move(source_map)), compiler_statistics_(compiler_statistics) {}

NativeModule::~NativeModule() {
    if (library_handle_ != nullptr) dlclose(library_handle_);
}

NativeEvalResult NativeModule::evaluate(std::vector<std::uint8_t>& signals) const {
    if (signals.empty()) return {};
    return eval_(signals.data());
}

bool NativeModule::cache_hit() const noexcept { return cache_hit_; }
const std::filesystem::path& NativeModule::library_path() const noexcept { return library_path_; }
const NativeCompileTimings& NativeModule::timings() const noexcept { return timings_; }
const std::vector<GateId>& NativeModule::source_map() const noexcept { return source_map_; }
const CompilerStatistics& NativeModule::compiler_statistics() const noexcept { return compiler_statistics_; }

std::string NativeCompiler::generate_source(const Circuit& circuit,
                                            std::uint32_t max_dense_passes) {
    const auto prepared = prepare_circuit(circuit);
    return generate_source(prepared.optimized_ir, max_dense_passes);
}

std::string NativeCompiler::generate_source(const ir::CircuitIR& circuit,
                                            std::uint32_t max_dense_passes) {
    if (max_dense_passes == 0) throw std::invalid_argument("max_dense_passes must be greater than zero");
    constexpr std::size_t kGatesPerBlock = 64;
    const auto& gates = circuit.operations;
    const auto order_size = gates.size();
    const auto block_count = (order_size + kGatesPerBlock - 1) / kGatesPerBlock;

    std::ostringstream out;
    out << "#include <cstdint>\n"
        << "struct HcsNativeEvalResult { std::uint64_t operations; std::uint32_t passes; std::uint8_t converged; };\n"
        << "#if defined(__GNUC__) || defined(__clang__)\n"
        << "#define HCS_NOINLINE __attribute__((noinline))\n"
        << "#else\n#define HCS_NOINLINE\n#endif\n";

    for (std::size_t block = 0; block < block_count; ++block) {
        const auto begin = block * kGatesPerBlock;
        const auto finish = std::min(order_size, begin + kGatesPerBlock);
        out << "static HCS_NOINLINE void hcs_block_" << block
            << "(std::uint8_t* s, bool* changed) noexcept {\n";
        for (std::size_t index = begin; index < finish; ++index) {
            const auto& gate = gates[index];
            const std::string a = gate.a == kInvalidSignal ? "0" : "s[" + std::to_string(gate.a) + "]";
            const std::string b = gate.b == kInvalidSignal ? "0" : "s[" + std::to_string(gate.b) + "]";
            std::string expression;
            switch (gate.opcode) {
                case LogicOp::Constant0: expression = "0"; break;
                case LogicOp::Constant1: expression = "1"; break;
                case LogicOp::Buffer: expression = a; break;
                case LogicOp::Not: expression = "(" + a + " == 0)"; break;
                case LogicOp::And: expression = "(" + a + " != 0 && " + b + " != 0)"; break;
                case LogicOp::Or: expression = "(" + a + " != 0 || " + b + " != 0)"; break;
                case LogicOp::Xor: expression = "((" + a + " != 0) != (" + b + " != 0))"; break;
                case LogicOp::Nand: expression = "!(" + a + " != 0 && " + b + " != 0)"; break;
                case LogicOp::Nor: expression = "!(" + a + " != 0 || " + b + " != 0)"; break;
                case LogicOp::Xnor: expression = "((" + a + " != 0) == (" + b + " != 0))"; break;
            }
            out << "  { const std::uint8_t v = static_cast<std::uint8_t>(" << expression
                << "); if (s[" << gate.output << "u] != v) { s[" << gate.output
                << "u] = v; *changed = true; } }\n";
        }
        out << "}\n";
    }

    out << "extern \"C\" HcsNativeEvalResult hcs_eval(std::uint8_t* s) noexcept {\n"
        << "  HcsNativeEvalResult r{0, 0, 1};\n"
        << "  for (std::uint32_t pass = 0; pass < " << max_dense_passes << "u; ++pass) {\n"
        << "    bool changed = false;\n"
        << "    ++r.passes;\n"
        << "    r.operations += " << order_size << "ull;\n";
    for (std::size_t block = 0; block < block_count; ++block) {
        out << "    hcs_block_" << block << "(s, &changed);\n";
    }
    out << "    if (!changed) return r;\n"
        << "  }\n"
        << "  r.converged = 0;\n"
        << "  return r;\n"
        << "}\n";
    return out.str();
}

std::shared_ptr<NativeModule>
NativeCompiler::load_or_compile(const Circuit& circuit, const NativeCompileOptions& supplied) {
    const auto total_start = SteadyClock::now();
    NativeCompileOptions options = supplied;
    if (options.cache_directory.empty()) options.cache_directory = default_cache_directory();
    const auto prepared = prepare_circuit(circuit);
    const auto codegen_start = SteadyClock::now();
    const auto source = generate_source(prepared.optimized_ir, options.max_dense_passes);
    std::vector<GateId> source_map;
    source_map.reserve(prepared.optimized_ir.operations.size());
    for (const auto& operation : prepared.optimized_ir.operations) {
        source_map.push_back(operation.source_component);
    }
    const auto codegen_end = SteadyClock::now();
    const auto compiler_version = get_compiler_version(options.compiler);
    const std::string fingerprint_input = source + "\ncompiler=" + options.compiler +
        "\ncompiler_version=" + compiler_version + "\noptimization=" + options.optimization_flag +
        "\narchitecture=" + architecture_tag() +
        "\nabi=" + std::to_string(kNativeAbiVersion);
    const auto fingerprint = hex_hash(fnv1a(fingerprint_input));

    std::filesystem::create_directories(options.cache_directory);
    const auto cache_start = SteadyClock::now();
    const auto library_path = options.cache_directory / (fingerprint + ".so");
    const auto source_path = options.cache_directory / (fingerprint + ".cpp");
    const auto manifest_path = options.cache_directory / (fingerprint + ".meta");
    const auto temporary_library = options.cache_directory / (fingerprint + ".tmp.so");

    const auto expected_prefix = "HCS_NATIVE_CACHE\nabi=" + std::to_string(kNativeAbiVersion) +
        "\nfingerprint=" + fingerprint + "\ncompiler=" + options.compiler +
        "\ncompiler_version=" + compiler_version + "\noptimization=" + options.optimization_flag +
        "\narchitecture=" + architecture_tag() +
        "\nbinary_hash=";
    const auto manifest = read_file(manifest_path);
    if (std::filesystem::exists(library_path) && manifest.starts_with(expected_prefix)) {
        const auto binary_hash_text = manifest.substr(expected_prefix.size());
        const auto expected_binary_hash = binary_hash_text.substr(0, binary_hash_text.find('\n'));
        if (expected_binary_hash == hex_hash(hash_file(library_path))) {
            try {
                NativeCompileTimings timings;
                timings.code_generation_ms = milliseconds(codegen_start, codegen_end);
                timings.generated_source_bytes = source.size();
                timings.cache_lookup_ms = milliseconds(cache_start, SteadyClock::now());
                if (!std::filesystem::exists(source_path)) write_file(source_path, source);
                return open_module(library_path, true, timings, total_start, source_map,
                                    prepared.statistics);
            } catch (...) {
                // Invalid cached modules are rebuilt.
            }
        }
    }

    const auto cache_end = SteadyClock::now();
    write_file(source_path, source);
    std::error_code ignored;
    std::filesystem::remove(temporary_library, ignored);
    const auto compiler_start = SteadyClock::now();
    run_compiler(options, source_path, temporary_library);
    const auto compiler_end = SteadyClock::now();
    std::filesystem::rename(temporary_library, library_path);
    const auto binary_hash = hash_file(library_path);
    write_file(manifest_path, cache_manifest(fingerprint, options, compiler_version, binary_hash));
    NativeCompileTimings timings;
    timings.code_generation_ms = milliseconds(codegen_start, codegen_end);
    timings.generated_source_bytes = source.size();
    timings.cache_lookup_ms = milliseconds(cache_start, cache_end);
    timings.compiler_invocation_ms = milliseconds(compiler_start, compiler_end);
    return open_module(library_path, false, timings, total_start, std::move(source_map),
                        prepared.statistics);
}

} // namespace hcs
