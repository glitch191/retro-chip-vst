# ui_preview: offscreen render of the chip panels with the ENGINE_SPECS parameter sets
# (tools/presetgen/params/*.json). Links the plugin's shared-code library like plugin_tests.

add_executable(ui_preview UiPreviewMain.cpp)

target_include_directories(ui_preview PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src"
    $<TARGET_PROPERTY:RetroChip,INCLUDE_DIRECTORIES>)

target_compile_definitions(ui_preview PRIVATE
    $<TARGET_PROPERTY:RetroChip,COMPILE_DEFINITIONS>
    RCV_PARAMS_DIR="${CMAKE_SOURCE_DIR}/tools/presetgen/params")

target_link_libraries(ui_preview PRIVATE RetroChip)
