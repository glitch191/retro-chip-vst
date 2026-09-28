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

# Shared headers at the root of include/chipdsp (IChipEngine.h, ChipTypes.h) come with core.
foreach(component chipdsp_nes chipdsp_snes chipdsp_genesis chipdsp_perf)
    get_target_property(_type ${component} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        target_link_libraries(${component} INTERFACE chipdsp_core)
    else()
        target_link_libraries(${component} PUBLIC chipdsp_core)
    endif()
endforeach()

add_library(chipdsp::core ALIAS chipdsp_core)
add_library(chipdsp::nes ALIAS chipdsp_nes)
add_library(chipdsp::snes ALIAS chipdsp_snes)
add_library(chipdsp::genesis ALIAS chipdsp_genesis)
add_library(chipdsp::perf ALIAS chipdsp_perf)

add_library(chipdsp INTERFACE)
target_link_libraries(chipdsp INTERFACE chipdsp_core chipdsp_nes chipdsp_snes chipdsp_genesis chipdsp_perf)
add_library(chipdsp::chipdsp ALIAS chipdsp)
