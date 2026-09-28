# chipdsp unit tests (Catch2 v3). One executable per area, named by file prefix:
#   test_core_*.cpp    -> chipdsp_tests_core     (resampler, filters)
#   test_nes_*.cpp     -> chipdsp_tests_nes
#   test_snes_*.cpp    -> chipdsp_tests_snes
#   test_genesis_*.cpp -> chipdsp_tests_genesis
#   test_perf_*.cpp    -> chipdsp_tests_perf     (arpeggiator, glide, allocator)
# helpers/ holds shared test utilities (WAV writer, spectrum helpers).

include(Catch)

file(GLOB CHIPDSP_TEST_HELPERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.cpp")

function(chipdsp_add_test_exe name prefix)
    file(GLOB _sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_${prefix}_*.cpp")
    if(NOT _sources)
        return()
    endif()
    add_executable(${name} ${_sources} ${CHIPDSP_TEST_HELPERS})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/helpers")
    target_link_libraries(${name} PRIVATE ${ARGN} Catch2::Catch2WithMain rcv_warnings)
    catch_discover_tests(${name}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DISCOVERY_MODE PRE_TEST)
endfunction()

chipdsp_add_test_exe(chipdsp_tests_core    core    chipdsp::core)
chipdsp_add_test_exe(chipdsp_tests_nes     nes     chipdsp::nes)
chipdsp_add_test_exe(chipdsp_tests_snes    snes    chipdsp::snes)
chipdsp_add_test_exe(chipdsp_tests_genesis genesis chipdsp::genesis)
chipdsp_add_test_exe(chipdsp_tests_perf    perf    chipdsp::perf)
