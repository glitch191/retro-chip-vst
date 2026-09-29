# plugin_tests: Catch2 tests of the plugin core (parameters, state, MIDI learn, randomizer,
# engine host routing, buses, allocation-free processBlock).
#
# The executable links the plugin's shared-code static library (RetroChip, created by
# juce_add_plugin), which already contains the compiled plugin sources and JUCE modules.
# The test sources are compiled with the same include directories and compile definitions
# as that library (like JUCE's own plugin wrapper targets do), so the JUCE headers see the
# exact configuration the library was built with.

include(Catch)

file(GLOB RCV_PLUGIN_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

add_executable(plugin_tests ${RCV_PLUGIN_TEST_SOURCES})

target_include_directories(plugin_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    $<TARGET_PROPERTY:RetroChip,INCLUDE_DIRECTORIES>)

target_compile_definitions(plugin_tests PRIVATE
    $<TARGET_PROPERTY:RetroChip,COMPILE_DEFINITIONS>)

target_link_libraries(plugin_tests PRIVATE
    RetroChip
    Catch2::Catch2WithMain)

catch_discover_tests(plugin_tests
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    DISCOVERY_MODE PRE_TEST)
