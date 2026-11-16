cmake_minimum_required(VERSION 3.20)
project(HybridCircuitSim VERSION 0.1.0 LANGUAGES CXX)

option(HCS_BUILD_TESTS "Build correctness and editor integration tests" ON)
option(HCS_BUILD_BENCHMARKS "Build headless benchmarks" ON)
option(HCS_BUILD_GUI "Build the SDL3 circuit editor when SDL3 is available" ON)
option(HCS_REQUIRE_SDL3 "Fail configuration when HCS_BUILD_GUI is enabled but SDL3 is unavailable" OFF)
option(HCS_ENABLE_AVX2 "Build an isolated runtime-detected AVX2 kernel when supported by the toolchain" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(hcs_core
    src/circuit.cpp
    src/ir/ir.cpp
    src/ir/analysis.cpp
    src/ir/optimizer.cpp
    src/ir/compiler_pipeline.cpp
    src/native_compiler.cpp
    src/simulator.cpp
    src/hardware/cpu_features.cpp
    src/hardware/worker_pool.cpp
)
add_library(hcs ALIAS hcs_core)
target_include_directories(hcs_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hcs_core PUBLIC cxx_std_20)
target_compile_options(hcs_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
target_link_libraries(hcs_core PRIVATE ${CMAKE_DL_LIBS})
find_package(Threads REQUIRED)
target_link_libraries(hcs_core PRIVATE Threads::Threads)

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" HCS_SYSTEM_PROCESSOR)
if(HCS_ENABLE_AVX2 AND HCS_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|i.86)$" AND
   CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_sources(hcs_core PRIVATE src/hardware/simd_avx2.cpp)
    set_source_files_properties(src/hardware/simd_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
    target_compile_definitions(hcs_core PRIVATE HCS_HAVE_AVX2_KERNEL=1)
    message(STATUS "HCS: isolated AVX2 kernel enabled; runtime feature detection remains mandatory")
else()
    message(STATUS "HCS: AVX2 kernel disabled; scalar implementation will be used")
endif()

add_library(hcs_editor
    src/editor/model.cpp
    src/editor/persistence.cpp
    src/editor/custom_components.cpp
    src/editor/runtime.cpp
    src/editor/simulation_controller.cpp
    src/editor/camera.cpp
)
target_include_directories(hcs_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(hcs_editor PUBLIC cxx_std_20)
target_compile_options(hcs_editor PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
target_link_libraries(hcs_editor PUBLIC hcs)

add_executable(hcs_demo src/main.cpp)
target_link_libraries(hcs_demo PRIVATE hcs)

add_executable(hcs_inspect src/inspect.cpp)
target_link_libraries(hcs_inspect PRIVATE hcs)

add_executable(hcs_circuit src/circuit_tool.cpp)
target_link_libraries(hcs_circuit PRIVATE hcs_editor)

if(HCS_BUILD_TESTS)
    enable_testing()
    add_executable(hcs_tests tests/test_sim.cpp)
    target_link_libraries(hcs_tests PRIVATE hcs)
    add_test(NAME hcs_tests COMMAND hcs_tests)

    add_executable(hcs_editor_tests tests/test_editor.cpp)
    target_link_libraries(hcs_editor_tests PRIVATE hcs_editor)
    add_test(NAME hcs_editor_tests COMMAND hcs_editor_tests)

    add_executable(hcs_persistence_tests tests/test_persistence.cpp)
    target_link_libraries(hcs_persistence_tests PRIVATE hcs_editor)
    add_test(NAME hcs_persistence_tests COMMAND hcs_persistence_tests)

    add_executable(hcs_custom_component_tests tests/test_custom_components.cpp)
    target_link_libraries(hcs_custom_component_tests PRIVATE hcs_editor)
    target_compile_definitions(hcs_custom_component_tests PRIVATE HCS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME hcs_custom_component_tests COMMAND hcs_custom_component_tests)
endif()

if(HCS_BUILD_BENCHMARKS)
    add_executable(hcs_bench bench/bench.cpp)
    target_link_libraries(hcs_bench PRIVATE hcs)

    add_executable(hcs_hardware_bench bench/bench_hardware.cpp)
    target_link_libraries(hcs_hardware_bench PRIVATE hcs)
endif()

if(HCS_BUILD_GUI)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(SDL3_FOUND)
        find_package(SDL3_ttf CONFIG QUIET)
        add_executable(hcs_gui
            gui/application.cpp
            gui/renderer.cpp
            gui/font_manager.cpp
        )
        target_include_directories(hcs_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gui)
        target_compile_features(hcs_gui PRIVATE cxx_std_20)
        target_compile_options(hcs_gui PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
        )
        target_link_libraries(hcs_gui PRIVATE hcs_editor SDL3::SDL3)
        if(SDL3_ttf_FOUND)
            target_link_libraries(hcs_gui PRIVATE SDL3_ttf::SDL3_ttf)
            target_compile_definitions(hcs_gui PRIVATE HCS_HAVE_SDL3_TTF=1)
            message(STATUS "HCS GUI: SDL3_ttf font rendering enabled")
        else()
            message(WARNING "SDL3_ttf was not found: the GUI will use a pixel-aligned ASCII fallback. Install SDL3_ttf and system UI/code fonts for proportional, antialiased typography.")
        endif()
    elseif(HCS_REQUIRE_SDL3)
        message(FATAL_ERROR "HCS_BUILD_GUI is enabled, but SDL3 was not found. Install SDL3 development files or set HCS_BUILD_GUI=OFF.")
    else()
        message(WARNING "SDL3 was not found: hcs_gui will not be built. Install SDL3 development files and reconfigure to enable the GUI.")
    endif()
endif()
