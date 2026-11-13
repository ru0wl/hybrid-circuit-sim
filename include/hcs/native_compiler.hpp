#pragma once

#include "hcs/circuit.hpp"
#include "hcs/ir.hpp"
#include "hcs/compiler_pipeline.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hcs {

struct NativeEvalResult {
    std::uint64_t operations{0};
    std::uint32_t passes{0};
    std::uint8_t converged{1};
};

struct NativeCompileTimings {
    double code_generation_ms{};
    double cache_lookup_ms{};
    double compiler_invocation_ms{};
    double library_load_ms{};
    double total_preparation_ms{};
    std::uint64_t generated_source_bytes{};
    std::uint64_t shared_library_bytes{};
};

using NativeEvalFunction = NativeEvalResult (*)(std::uint8_t* signals);

class NativeModule final {
public:
    NativeModule(void* library_handle, NativeEvalFunction eval,
                 bool cache_hit, std::filesystem::path library_path,
                 NativeCompileTimings timings, std::vector<GateId> source_map,
                 CompilerStatistics compiler_statistics);
    ~NativeModule();

    NativeModule(const NativeModule&) = delete;
    NativeModule& operator=(const NativeModule&) = delete;

    [[nodiscard]] NativeEvalResult evaluate(std::vector<std::uint8_t>& signals) const;
    [[nodiscard]] bool cache_hit() const noexcept;
    [[nodiscard]] const std::filesystem::path& library_path() const noexcept;
    [[nodiscard]] const NativeCompileTimings& timings() const noexcept;
    [[nodiscard]] const std::vector<GateId>& source_map() const noexcept;
    [[nodiscard]] const CompilerStatistics& compiler_statistics() const noexcept;

private:
    void* library_handle_{nullptr};
    NativeEvalFunction eval_{nullptr};
    bool cache_hit_{false};
    std::filesystem::path library_path_;
    NativeCompileTimings timings_;
    std::vector<GateId> source_map_;
    CompilerStatistics compiler_statistics_;
};

struct NativeCompileOptions {
    std::filesystem::path cache_directory;
    std::string compiler{"c++"};
    std::string optimization_flag{"-O2"};
    std::uint32_t max_dense_passes{64};
};

class NativeCompiler final {
public:
    [[nodiscard]] static std::shared_ptr<NativeModule>
    load_or_compile(const Circuit& circuit, const NativeCompileOptions& options = {});

    [[nodiscard]] static std::string generate_source(const Circuit& circuit,
                                                     std::uint32_t max_dense_passes);
    [[nodiscard]] static std::string generate_source(const ir::CircuitIR& circuit,
                                                     std::uint32_t max_dense_passes);
};

} // namespace hcs
