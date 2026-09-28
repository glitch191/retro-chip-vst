# chipdsp unit tests (Catch2 v3). One executable, all test_*.cpp files globbed.

file(GLOB CHIPDSP_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.cpp")

add_executable(chipdsp_tests ${CHIPDSP_TEST_SOURCES})
target_include_directories(chipdsp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/helpers")
target_link_libraries(chipdsp_tests PRIVATE chipdsp::chipdsp Catch2::Catch2WithMain rcv_warnings)

include(Catch)
catch_discover_tests(chipdsp_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST)
