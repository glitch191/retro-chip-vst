# chipdsp: hardware chip emulation cores, performance modules and resampling.
# No dependency on JUCE.
#
# One static library per area so that each can be built and tested on its own:
#   chipdsp_core     util/ (resampling, filters)          -> chipdsp::core
#   chipdsp_nes      nes/                                 -> chipdsp::nes
#   chipdsp_snes     snes/                                -> chipdsp::snes
#   chipdsp_genesis  genesis/                             -> chipdsp::genesis
#   chipdsp_perf     perf/ (arpeggiator, glide, allocator)-> chipdsp::perf
#   chipdsp          umbrella INTERFACE target            -> chipdsp::chipdsp
# Sources are globbed per folder so new files need no CMake edits.

set(CHIPDSP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

function(chipdsp_add_component name folder)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/${folder}/*.cpp")
    file(GLOB_RECURSE _headers CONFIGURE_DEPENDS "${CHIPDSP_INCLUDE_DIR}/chipdsp/${folder}/*.h")
    if(NOT _sources)
        # Header-only or not written yet: create an interface target so dependents still link.
        add_library(${name} INTERFACE)
        target_include_directories(${name} INTERFACE "${CHIPDSP_INCLUDE_DIR}")
        target_compile_features(${name} INTERFACE cxx_std_20)
    else()
        add_library(${name} STATIC ${_sources} ${_headers})
        target_include_directories(${name} PUBLIC "${CHIPDSP_INCLUDE_DIR}")
        target_compile_features(${name} PUBLIC cxx_std_20)
        target_link_libraries(${name} PRIVATE rcv_warnings)
        if(MSVC)
            target_compile_options(${name} PRIVATE /fp:precise)
        endif()
        source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${_sources} ${_headers})
    endif()
endfunction()

chipdsp_add_component(chipdsp_core util)
chipdsp_add_component(chipdsp_nes nes)
chipdsp_add_component(chipdsp_snes snes)
chipdsp_add_component(chipdsp_genesis genesis)
chipdsp_add_component(chipdsp_perf perf)
chipdsp_add_component(chipdsp_factory factory)

# Shared headers at the root of include/chipdsp (IChipEngine.h, ChipTypes.h) come with core.
foreach(component chipdsp_nes chipdsp_snes chipdsp_genesis chipdsp_perf chipdsp_factory)
    get_target_property(_type ${component} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        target_link_libraries(${component} INTERFACE chipdsp_core)
    else()
        target_link_libraries(${component} PUBLIC chipdsp_core)
    endif()
endforeach()

# The factory links each real engine once its sources exist; until then (or when
# RCV_USE_STUB_ENGINES is ON) it hands out StubEngine for that chip.
option(RCV_USE_STUB_ENGINES "Use the placeholder StubEngine for every chip (plugin-layer development)" OFF)
foreach(pair "chipdsp_nes;NES" "chipdsp_snes;SNES" "chipdsp_genesis;GENESIS")
    list(GET pair 0 _target)
    list(GET pair 1 _chip)
    get_target_property(_type ${_target} TYPE)
    if(NOT RCV_USE_STUB_ENGINES AND NOT _type STREQUAL "INTERFACE_LIBRARY")
        target_link_libraries(chipdsp_factory PUBLIC ${_target})
        target_compile_definitions(chipdsp_factory PRIVATE RCV_HAVE_${_chip}_ENGINE=1)
        message(STATUS "chipdsp: ${_chip} engine linked")
    else()
        message(STATUS "chipdsp: ${_chip} engine not available, StubEngine will be used")
    endif()
endforeach()

add_library(chipdsp::core ALIAS chipdsp_core)
add_library(chipdsp::nes ALIAS chipdsp_nes)
add_library(chipdsp::snes ALIAS chipdsp_snes)
add_library(chipdsp::genesis ALIAS chipdsp_genesis)
add_library(chipdsp::perf ALIAS chipdsp_perf)
add_library(chipdsp::factory ALIAS chipdsp_factory)

add_library(chipdsp INTERFACE)
target_link_libraries(chipdsp INTERFACE chipdsp_core chipdsp_nes chipdsp_snes chipdsp_genesis chipdsp_perf chipdsp_factory)
add_library(chipdsp::chipdsp ALIAS chipdsp)
