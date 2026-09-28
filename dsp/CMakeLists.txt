# chipdsp: hardware chip emulation cores, performance modules and resampling.
# No dependency on JUCE. Sources are globbed so that each engine lives in its
# own folder without touching this file.

file(GLOB_RECURSE CHIPDSP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE CHIPDSP_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

add_library(chipdsp STATIC ${CHIPDSP_SOURCES} ${CHIPDSP_HEADERS})
add_library(chipdsp::chipdsp ALIAS chipdsp)

target_include_directories(chipdsp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(chipdsp PUBLIC cxx_std_20)
target_link_libraries(chipdsp PRIVATE rcv_warnings)

if(MSVC)
    # Deterministic, no fast-math: hardware-exact integer/fixed-point paths.
    target_compile_options(chipdsp PRIVATE /fp:precise)
endif()

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${CHIPDSP_SOURCES} ${CHIPDSP_HEADERS})
