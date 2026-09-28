# JUCE VST3 plugin target. Sources are globbed from plugin/src.

file(GLOB_RECURSE RCV_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

juce_add_plugin(RetroChip
    PRODUCT_NAME "Retro Chip"
    COMPANY_NAME "retro-chip-vst"
    BUNDLE_ID "com.retro-chip-vst.retrochip"
    PLUGIN_MANUFACTURER_CODE Rcvs
    PLUGIN_CODE Rchp
    FORMATS VST3
    VST3_CATEGORIES Instrument Synth
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${RCV_COPY_PLUGIN_AFTER_BUILD}
    VST3_COPY_DIR "$ENV{LOCALAPPDATA}/Programs/Common/VST3"
    VERSION ${PROJECT_VERSION})

target_sources(RetroChip PRIVATE ${RCV_PLUGIN_SOURCES})
target_include_directories(RetroChip PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Embedded assets: preset banks and procedurally generated samples.
file(GLOB_RECURSE RCV_ASSET_FILES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/presets/*.json"
    "${CMAKE_SOURCE_DIR}/assets/samples/*.wav")
if(RCV_ASSET_FILES)
    juce_add_binary_data(RetroChipAssets
        HEADER_NAME "RetroChipAssets.h"
        NAMESPACE RetroChipAssets
        SOURCES ${RCV_ASSET_FILES})
    target_link_libraries(RetroChip PRIVATE RetroChipAssets)
    target_compile_definitions(RetroChip PRIVATE RCV_HAS_ASSETS=1)
else()
    target_compile_definitions(RetroChip PRIVATE RCV_HAS_ASSETS=0)
endif()

target_compile_definitions(RetroChip PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(RetroChip
    PRIVATE
        chipdsp::chipdsp
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RCV_PLUGIN_SOURCES})
