# Hybrid Circuit Simulator build and benchmark helpers.

CMAKE ?= cmake
BUILD_DIR ?= build
BUILD_TYPE ?= Release
GENERATOR ?=
JOBS ?= $(shell nproc 2>/dev/null || echo 2)

BENCH_SCENARIO ?= all
BENCH_SIZE ?= 1000
BENCH_ITERATIONS ?= 1000
BENCH_RUNS ?= 20
BENCH_COLD_RUNS ?= 3
BENCH_WARMUP ?= 4
BENCH_MIN_SAMPLE_MS ?= 20
BENCH_MAX_CALIBRATION_ITERATIONS ?= 100000000
BENCH_COMPILE_MODE ?= both
BENCH_CACHE ?= .bench-cache
BENCH_OUTPUT_DIR ?= benchmark-results
BENCH_OUTPUT_PREFIX ?= $(BENCH_OUTPUT_DIR)/benchmark-results
BENCH_PROFILE ?= 0
HCS_BUILD_GUI ?= ON
HCS_ENABLE_AVX2 ?= ON
BENCH_NATIVE_COMPILER ?= c++
BENCH_OPTIMIZATION_FLAG ?= -O2
BENCH_ARGS ?= --scenario $(BENCH_SCENARIO) --size $(BENCH_SIZE) --iterations $(BENCH_ITERATIONS) --runs $(BENCH_RUNS) --cold-runs $(BENCH_COLD_RUNS) --warmup $(BENCH_WARMUP) --min-sample-ms $(BENCH_MIN_SAMPLE_MS) --max-calibration-iterations $(BENCH_MAX_CALIBRATION_ITERATIONS) --compile-mode $(BENCH_COMPILE_MODE) --cache $(BENCH_CACHE) --output-prefix $(BENCH_OUTPUT_PREFIX) --native-compiler $(BENCH_NATIVE_COMPILER) --optimization-flag $(BENCH_OPTIMIZATION_FLAG) $(if $(filter 1,$(BENCH_PROFILE)),--profile,)

CMAKE_GENERATOR_ARG := $(if $(strip $(GENERATOR)),-G "$(GENERATOR)",)

.PHONY: all configure build test run gui bench bench-hardware clean rebuild help

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_GENERATOR_ARG) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DHCS_BUILD_TESTS=ON -DHCS_BUILD_BENCHMARKS=ON -DHCS_BUILD_GUI=$(HCS_BUILD_GUI) -DHCS_ENABLE_AVX2=$(HCS_ENABLE_AVX2)

build: configure
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure

run: build
	./$(BUILD_DIR)/hcs_demo

gui:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_GENERATOR_ARG) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DHCS_BUILD_TESTS=ON -DHCS_BUILD_BENCHMARKS=ON -DHCS_BUILD_GUI=ON -DHCS_REQUIRE_SDL3=ON -DHCS_ENABLE_AVX2=$(HCS_ENABLE_AVX2)
	$(CMAKE) --build $(BUILD_DIR) --target hcs_gui --parallel $(JOBS)
	./$(BUILD_DIR)/hcs_gui

bench: build
	./$(BUILD_DIR)/hcs_bench $(BENCH_ARGS)

bench-hardware: build
	./$(BUILD_DIR)/hcs_hardware_bench $(BENCH_RUNS) --output-dir $(BENCH_OUTPUT_DIR)

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

rebuild: clean build

help:
	@printf '%s\n' \
	  'Hybrid Circuit Simulator' \
	  '' \
	  'Targets:' \
	  '  make              Configure and build Release' \
	  '  make test         Build and run tests' \
	  '  make run          Build and run CLI demo' \
	  '  hcs_circuit validate FILE.hcs  Validate a circuit document' \
	  '  hcs_circuit format FILE.hcs   Print canonical circuit text' \
	  '  make gui          Build and run SDL3 circuit editor' \
	  '  make bench        Run the complete benchmark suite' \
	  '  make bench-hardware Run SIMD/multicore A/B microbenchmarks' \
	  '  make clean        Remove the selected build directory' \
	  '  make rebuild      Clean and rebuild' \
	  '  make help         Show this help' \
	  '' \
	  'Benchmark variables:' \
	  '  BENCH_SCENARIO=all       all, sparse, counter, or one scenario name' \
	  '  BENCH_SIZE=1000          Circuit size parameter' \
	  '  BENCH_ITERATIONS=1000    Minimum iterations before calibration' \
	  '  BENCH_RUNS=20            Measured repetitions for interpreted/warm' \
	  '  BENCH_COLD_RUNS=3        Cold native compilation repetitions' \
	  '  BENCH_WARMUP=4           Warmup iterations' \
	  '  BENCH_MIN_SAMPLE_MS=20   Adaptive minimum sample duration; 0 disables' \
	  '  BENCH_MAX_CALIBRATION_ITERATIONS=100000000 Maximum common calibration iterations' \
	  '  BENCH_COMPILE_MODE=both  both, warm, or cold' \
	  '  BENCH_PROFILE=1          Enable intrusive runtime profiling' \
	  '  BENCH_NATIVE_COMPILER=c++ Native compiler executable' \
	  '  HCS_ENABLE_AVX2=ON       Build isolated runtime-detected AVX2 kernel' \
	  '  BENCH_OPTIMIZATION_FLAG=-O2 Native optimization flag' \
	  '  BENCH_CACHE=.bench-cache Native cache directory' \
	  '  BENCH_OUTPUT_DIR=benchmark-results Directory for all benchmark result files' \
	  '  BENCH_OUTPUT_PREFIX=benchmark-results/benchmark-results Output file prefix' \
	  '' \
	  'Example:' \
	  '  make bench BENCH_SCENARIO=sparse-toggle BENCH_SIZE=256 BENCH_RUNS=20' \
	  '  make bench BENCH_PROFILE=1 BENCH_COMPILE_MODE=warm'
