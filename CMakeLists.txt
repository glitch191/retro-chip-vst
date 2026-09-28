cmake_minimum_required(VERSION 3.22)

project(RetroChipVst
    VERSION 0.1.0
    DESCRIPTION "Hardware-faithful NES, SNES and Genesis chip synthesis VST3"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(RCV_BUILD_PLUGIN "Build the JUCE VST3 plugin (fetches JUCE)" ON)
option(RCV_BUILD_TESTS  "Build the chipdsp unit tests (fetches Catch2)" ON)
option(RCV_BUILD_BENCH  "Build the chipdsp CPU-cost benchmark" ON)
option(RCV_BUILD_TOOLS  "Build chiptool (preset rendering and parameter dump CLI)" ON)
option(RCV_COPY_PLUGIN_AFTER_BUILD "Copy the .vst3 to the user VST3 folder after build" OFF)
option(RCV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Static MSVC runtime keeps the .vst3 self-contained (no VC redist dependency).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Dependencies)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(dsp)

if(RCV_BUILD_TESTS)
    enable_testing()
    add_subdirectory(dsp/tests)
endif()

if(RCV_BUILD_BENCH)
    add_subdirectory(dsp/bench)
endif()

if(RCV_BUILD_TOOLS)
    add_subdirectory(dsp/tools)
endif()

if(RCV_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
