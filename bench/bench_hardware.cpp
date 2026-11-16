#include "hcs/hardware.hpp"
#include "hcs/simulator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <utility>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Workload {
    std::string name;
    hcs::Circuit circuit;
    std::vector<hcs::SignalId> inputs;
    std::uint64_t ticks{256};
    bool toggle_all_inputs{true};
};

Workload make_independent_regions(std::uint32_t lanes, std::uint32_t depth) {
    hcs::CircuitBuilder builder;
    Workload workload;
    workload.name = "independent-regions";
    const auto shared_control = builder.add_input();
    workload.inputs.push_back(shared_control);
    workload.toggle_all_inputs = false;
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        auto signal = builder.add_input();
        for (std::uint32_t stage = 0; stage < depth; ++stage) {
            signal = builder.add_binary((stage & 1u) == 0 ? hcs::LogicOp::Xor : hcs::LogicOp::And,
                                        signal, shared_control);
        }
    }
    workload.circuit = builder.build();
    return workload;
}

Workload make_wide_layer(std::uint32_t lanes) {
    hcs::CircuitBuilder builder;
    Workload workload;
    workload.name = "wide-independent-level";
    std::vector<hcs::SignalId> layer;
    layer.reserve(lanes);
    for (std::uint32_t lane = 0; lane < lanes; ++lane) {
        const auto a = builder.add_input();
        const auto b = builder.add_input();
        workload.inputs.push_back(a);
        workload.inputs.push_back(b);
        layer.push_back(builder.add_binary(hcs::LogicOp::And, a, b));
    }
    while (layer.size() > 1) {
        std::vector<hcs::SignalId> next;
        next.reserve((layer.size() + 1) / 2);
        for (std::size_t i = 0; i < layer.size(); i += 2) {
            if (i + 1 < layer.size()) next.push_back(builder.add_binary(hcs::LogicOp::Or, layer[i], layer[i + 1]));
            else next.push_back(layer[i]);
        }
        layer = std::move(next);
    }
    workload.circuit = builder.build();
    return workload;
}

Workload make_hybrid_wide_regions(std::uint32_t region_count, std::uint32_t width) {
    hcs::CircuitBuilder builder;
    Workload workload;
    workload.name = "hybrid-wide-independent-regions";
    const auto shared_control = builder.add_input();
    workload.inputs.push_back(shared_control);
    workload.toggle_all_inputs = false;
    for (std::uint32_t region = 0; region < region_count; ++region) {
        const auto root_input = builder.add_input();
        const auto root = builder.add_binary(hcs::LogicOp::Or, shared_control, root_input);
        std::vector<hcs::SignalId> layer;
        layer.reserve(width);
        for (std::uint32_t lane = 0; lane < width; ++lane) {
            const auto leaf_input = builder.add_input();
            layer.push_back(builder.add_binary(hcs::LogicOp::Xor, root, leaf_input));
        }
        while (layer.size() > 1) {
            std::vector<hcs::SignalId> next;
            next.reserve((layer.size() + 1) / 2);
            for (std::size_t i = 0; i < layer.size(); i += 2) {
                if (i + 1 < layer.size()) next.push_back(builder.add_binary(hcs::LogicOp::Or, layer[i], layer[i + 1]));
                else next.push_back(layer[i]);
            }
            layer = std::move(next);
        }
    }
    workload.circuit = builder.build();
    workload.ticks = 8;
    return workload;
}

struct Configuration {
    const char* name;
    hcs::SimdMode simd;
    hcs::CpuParallelMode parallel;
    std::uint32_t workers;
};

struct Sample {
    std::string scenario;
    std::string configuration;
    std::uint32_t run{};
    double setup_ms{};
    double elapsed_ms{};
    double ticks_per_second{};
    double process_cpu_ms{};
    double process_cpu_percent{};
    std::uint64_t operations{};
    double operations_per_second{};
    std::uint64_t checksum{};
    std::uint64_t simd_batches{};
    std::uint64_t simd_operations{};
    std::uint64_t parallel_batches{};
    std::uint64_t parallel_regions{};
    std::uint32_t workers{};
};

std::uint64_t checksum(const hcs::Simulator& simulator) {
    std::uint64_t value = 14695981039346656037ull;
    for (hcs::SignalId id = 0; id < simulator.signal_count(); ++id) {
        value ^= simulator.signal(id) ? 1u : 0u;
        value *= 1099511628211ull;
    }
    return value;
}

std::uint64_t execute(hcs::Simulator& simulator, const Workload& workload, std::uint64_t start_tick) {
    std::uint64_t operation_count = 0;
    for (std::uint64_t tick = 0; tick < workload.ticks; ++tick) {
        const auto input_count = workload.toggle_all_inputs ? workload.inputs.size() : std::min<std::size_t>(1, workload.inputs.size());
        for (std::size_t i = 0; i < input_count; ++i) {
            const bool value = ((tick + start_tick + i * 3u) & 1u) != 0;
            simulator.set_input(workload.inputs[i], value);
        }
        const auto result = simulator.step();
        if (!result.converged) throw std::runtime_error("Benchmark workload did not converge");
        operation_count += result.operations;
    }
    return operation_count;
}

Sample run(const Workload& workload, const Configuration& configuration, std::uint32_t run_index) {
    hcs::SimulatorConfig config;
    config.backend = hcs::Backend::HybridIR;
    config.acceleration.simd = configuration.simd;
    config.acceleration.parallel = configuration.parallel;
    config.acceleration.worker_threads = configuration.workers;

    {
        hcs::Simulator warm(workload.circuit, config);
        (void)execute(warm, workload, 0);
    }

    const auto setup_begin = Clock::now();
    hcs::Simulator simulator(workload.circuit, config);
    const auto setup_end = Clock::now();
    const auto cpu_begin = std::clock();
    const auto begin = Clock::now();
    const auto operation_count = execute(simulator, workload, 0);
    const auto end = Clock::now();
    const auto cpu_end = std::clock();

    Sample sample;
    sample.scenario = workload.name;
    sample.configuration = configuration.name;
    sample.run = run_index;
    sample.setup_ms = std::chrono::duration<double, std::milli>(setup_end - setup_begin).count();
    sample.elapsed_ms = std::chrono::duration<double, std::milli>(end - begin).count();
    sample.ticks_per_second = static_cast<double>(workload.ticks) / (sample.elapsed_ms / 1000.0);
    if (cpu_begin != static_cast<std::clock_t>(-1) && cpu_end != static_cast<std::clock_t>(-1)) {
        sample.process_cpu_ms = 1000.0 * static_cast<double>(cpu_end - cpu_begin) / CLOCKS_PER_SEC;
        sample.process_cpu_percent = sample.elapsed_ms > 0.0
            ? sample.process_cpu_ms * 100.0 / sample.elapsed_ms : 0.0;
    }
    sample.operations = operation_count;
    sample.operations_per_second = sample.elapsed_ms > 0.0
        ? static_cast<double>(operation_count) / (sample.elapsed_ms / 1000.0) : 0.0;
    sample.checksum = checksum(simulator);
    const auto& stats = simulator.acceleration_stats();
    sample.simd_batches = stats.simd_batches;
    sample.simd_operations = stats.simd_operations;
    sample.parallel_batches = stats.parallel_region_batches;
    sample.parallel_regions = stats.parallel_regions;
    sample.workers = simulator.worker_thread_count();
    return sample;
}

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

void write_results(const std::vector<Sample>& samples, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    std::ofstream raw(directory / "hardware-bench-results-phase5.2-raw.csv");
    raw << "scenario,configuration,run,setup_ms,elapsed_ms,ticks_per_second,process_cpu_ms,process_cpu_percent,operations,operations_per_second,checksum,simd_batches,simd_operations,parallel_batches,parallel_regions,workers\n";
    for (const auto& sample : samples) {
        raw << sample.scenario << ',' << sample.configuration << ',' << sample.run << ','
            << std::setprecision(12) << sample.setup_ms << ',' << sample.elapsed_ms << ','
            << sample.ticks_per_second << ',' << sample.process_cpu_ms << ',' << sample.process_cpu_percent << ','
            << sample.operations << ',' << sample.operations_per_second << ',' << sample.checksum << ',' << sample.simd_batches << ','
            << sample.simd_operations << ',' << sample.parallel_batches << ','
            << sample.parallel_regions << ',' << sample.workers << '\n';
    }

    std::ofstream summary(directory / "hardware-bench-results-phase5.2-summary.csv");
    summary << "scenario,configuration,runs,median_ms,mean_ms,min_ms,max_ms,stddev_ms,p95_ms,median_ticks_per_second,median_setup_ms,median_process_cpu_ms,median_process_cpu_percent,median_operations,median_operations_per_second,median_simd_batches,median_parallel_regions,checksum_valid\n";
    std::vector<std::string> scenarios;
    std::vector<std::string> configurations;
    for (const auto& sample : samples) {
        if (std::find(scenarios.begin(), scenarios.end(), sample.scenario) == scenarios.end()) scenarios.push_back(sample.scenario);
        if (std::find(configurations.begin(), configurations.end(), sample.configuration) == configurations.end()) configurations.push_back(sample.configuration);
    }
    for (const auto& scenario : scenarios) {
        std::uint64_t reference_checksum = 0;
        bool first = true;
        for (const auto& configuration : configurations) {
            std::vector<double> times, rates, setups, process_cpu_ms, process_cpu_percent, operation_counts, operation_rates, simd_batches, parallel_regions;
            std::vector<std::uint64_t> checksums;
            for (const auto& sample : samples) {
                if (sample.scenario != scenario || sample.configuration != configuration) continue;
                times.push_back(sample.elapsed_ms);
                rates.push_back(sample.ticks_per_second);
                setups.push_back(sample.setup_ms);
                process_cpu_ms.push_back(sample.process_cpu_ms);
                process_cpu_percent.push_back(sample.process_cpu_percent);
                operation_counts.push_back(static_cast<double>(sample.operations));
                operation_rates.push_back(sample.operations_per_second);
                simd_batches.push_back(static_cast<double>(sample.simd_batches));
                parallel_regions.push_back(static_cast<double>(sample.parallel_regions));
                checksums.push_back(sample.checksum);
            }
            if (times.empty()) continue;
            if (first) { reference_checksum = checksums.front(); first = false; }
            const bool checksum_valid = std::all_of(checksums.begin(), checksums.end(), [&](auto value) { return value == reference_checksum; });
            const double mean = std::accumulate(times.begin(), times.end(), 0.0) / static_cast<double>(times.size());
            double variance = 0.0;
            for (const auto time : times) variance += (time - mean) * (time - mean);
            variance /= static_cast<double>(times.size());
            const auto minmax = std::minmax_element(times.begin(), times.end());
            summary << scenario << ',' << configuration << ',' << times.size() << ','
                    << percentile(times, 0.50) << ',' << mean << ',' << *minmax.first << ',' << *minmax.second << ','
                    << std::sqrt(variance) << ',' << percentile(times, 0.95) << ','
                    << percentile(rates, 0.50) << ',' << percentile(setups, 0.50) << ','
                    << percentile(process_cpu_ms, 0.50) << ',' << percentile(process_cpu_percent, 0.50) << ','
                    << percentile(operation_counts, 0.50) << ',' << percentile(operation_rates, 0.50) << ','
                    << percentile(simd_batches, 0.50) << ',' << percentile(parallel_regions, 0.50) << ','
                    << (checksum_valid ? "true" : "false") << '\n';
        }
    }

    std::ofstream paired(directory / "hardware-bench-results-phase5.2-paired.csv");
    paired << "scenario,configuration,runs,paired_median_throughput_change_pct,paired_mean_throughput_change_pct,min_change_pct,max_change_pct,stddev_change_pct,wins_vs_scalar,median_simd_batches,median_parallel_regions\n";
    for (const auto& scenario : scenarios) {
        std::vector<const Sample*> scalar_samples;
        for (const auto& sample : samples)
            if (sample.scenario == scenario && sample.configuration == "scalar") scalar_samples.push_back(&sample);
        for (const auto& configuration : configurations) {
            if (configuration == "scalar") continue;
            std::vector<double> changes, simd_batches, parallel_regions;
            std::uint64_t wins = 0;
            for (const auto* scalar : scalar_samples) {
                const auto found = std::find_if(samples.begin(), samples.end(), [&](const Sample& candidate) {
                    return candidate.scenario == scenario && candidate.configuration == configuration &&
                           candidate.run == scalar->run;
                });
                if (found == samples.end()) continue;
                const double change = found->elapsed_ms > 0.0
                    ? (scalar->elapsed_ms / found->elapsed_ms - 1.0) * 100.0 : 0.0;
                changes.push_back(change);
                if (change > 0.0) ++wins;
                simd_batches.push_back(static_cast<double>(found->simd_batches));
                parallel_regions.push_back(static_cast<double>(found->parallel_regions));
            }
            if (changes.empty()) continue;
            const double mean = std::accumulate(changes.begin(), changes.end(), 0.0) / static_cast<double>(changes.size());
            double variance = 0.0;
            for (const auto change : changes) variance += (change - mean) * (change - mean);
            variance /= static_cast<double>(changes.size());
            const auto minmax = std::minmax_element(changes.begin(), changes.end());
            paired << scenario << ',' << configuration << ',' << changes.size() << ','
                   << percentile(changes, 0.50) << ',' << mean << ',' << *minmax.first << ',' << *minmax.second << ','
                   << std::sqrt(variance) << ',' << wins << ','
                   << percentile(simd_batches, 0.50) << ',' << percentile(parallel_regions, 0.50) << '\n';
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::uint32_t runs = 20;
        std::filesystem::path output_directory{"benchmark-results"};
        bool positional_runs_seen = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&](const char* option) -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument(std::string("Missing value for ") + option);
                return argv[++i];
            };
            if (arg == "--runs") {
                runs = static_cast<std::uint32_t>(std::stoul(value("--runs")));
            } else if (arg == "--output-dir") {
                output_directory = value("--output-dir");
            } else if (!arg.empty() && arg.front() != '-' && !positional_runs_seen) {
                runs = static_cast<std::uint32_t>(std::stoul(arg));
                positional_runs_seen = true;
            } else if (arg == "--help") {
                std::cout << "hcs_hardware_bench [runs] [--runs N] [--output-dir DIR]\n";
                return 0;
            } else {
                throw std::invalid_argument("Unknown argument: " + arg);
            }
        }
        if (runs < 3 || runs > 100) throw std::invalid_argument("runs must be in [3, 100]");
        const auto capabilities = hcs::detect_hardware_capabilities();
        const auto workers = std::max<std::uint32_t>(1, std::min<std::uint32_t>(2, capabilities.logical_cpu_count));
        const std::vector<Configuration> configurations{
            {"scalar", hcs::SimdMode::Off, hcs::CpuParallelMode::Off, 0},
            {"avx2-explicit", hcs::SimdMode::AVX2, hcs::CpuParallelMode::Off, 0},
            {"threads", hcs::SimdMode::Off, hcs::CpuParallelMode::Custom, workers},
            {"avx2-plus-threads", hcs::SimdMode::AVX2, hcs::CpuParallelMode::Custom, workers},
        };
        auto independent = make_independent_regions(256, 128);
        independent.ticks = 12;
        auto wide = make_wide_layer(4096);
        wide.ticks = 4;
        auto hybrid_wide = make_hybrid_wide_regions(16, 256);
        auto hybrid_large = make_hybrid_wide_regions(8, 4096);
        hybrid_large.name = "hybrid-large-independent-regions";
        hybrid_large.ticks = 6;
        const std::vector<Workload> workloads{std::move(independent), std::move(wide), std::move(hybrid_wide), std::move(hybrid_large)};
        std::vector<Sample> samples;
        std::map<std::pair<std::string, std::uint32_t>, std::uint64_t> expected_operations;
        for (const auto& workload : workloads) {
            std::cout << "Benchmarking " << workload.name << " with " << runs << " runs\n";
            std::uint64_t expected_checksum = 0;
            bool have_checksum = false;
            for (std::uint32_t run_index = 0; run_index < runs; ++run_index) {
                // Rotate deterministic configuration order to reduce time/load drift.
                for (std::size_t order = 0; order < configurations.size(); ++order) {
                    const auto& configuration = configurations[(order + run_index) % configurations.size()];
                    if (configuration.simd == hcs::SimdMode::AVX2 && !capabilities.avx2_usable &&
                        std::string(configuration.name).find("avx2") != std::string::npos) {
                        std::cout << "  skipping " << configuration.name << " (AVX2 unavailable)\n";
                        continue;
                    }
                    auto sample = run(workload, configuration, run_index);
                    if (!have_checksum) { expected_checksum = sample.checksum; have_checksum = true; }
                    if (sample.checksum != expected_checksum)
                        throw std::runtime_error("Accelerated benchmark checksum differs from scalar reference");
                    const auto key = std::make_pair(sample.scenario, sample.run);
                    const auto [expected, inserted] = expected_operations.emplace(key, sample.operations);
                    if (!inserted && expected->second != sample.operations)
                        throw std::runtime_error("Acceleration configuration changed operation count");
                    samples.push_back(std::move(sample));
                }
            }
        }
        write_results(samples, output_directory);
        std::cout << "Wrote hardware-bench-results-phase5.2-{raw,summary,paired}.csv in "
                  << output_directory << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Hardware benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
