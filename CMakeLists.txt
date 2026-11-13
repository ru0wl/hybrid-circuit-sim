cmake_minimum_required(VERSION 3.20)
project(HybridCircuitSim VERSION 0.1.0 LANGUAGES CXX)

option(HCS_BUILD_TESTS "Build correctness tests" ON)
option(HCS_BUILD_BENCHMARKS "Build headless benchmarks" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hcs
    src/circuit.cpp
    src/ir/ir.cpp
    src/ir/analysis.cpp
    src/ir/optimizer.cpp
    src/ir/compiler_pipeline.cpp
    src/native_compiler.cpp
    src/simulator.cpp
)
target_include_directories(hcs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hcs PUBLIC cxx_std_20)
target_compile_options(hcs PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
target_link_libraries(hcs PRIVATE ${CMAKE_DL_LIBS})

add_executable(hcs_demo src/main.cpp)
target_link_libraries(hcs_demo PRIVATE hcs)

add_executable(hcs_inspect src/inspect.cpp)
target_link_libraries(hcs_inspect PRIVATE hcs)

if(HCS_BUILD_TESTS)
    enable_testing()
    add_executable(hcs_tests tests/test_sim.cpp)
    target_link_libraries(hcs_tests PRIVATE hcs)
    add_test(NAME hcs_tests COMMAND hcs_tests)
endif()

if(HCS_BUILD_BENCHMARKS)
    add_executable(hcs_bench bench/bench.cpp)
    target_link_libraries(hcs_bench PRIVATE hcs)
endif()
