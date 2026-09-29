# JUCE VST3 plugin target. Sources are globbed from plugin/src.

file(GLOB_RECURSE RCV_PLUGIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# The standalone app is a development aid (editor screenshots via RCV_SCREENSHOT, see
# plugin/src/PluginEditor.h); release builds ship the VST3 only.
option(RCV_BUILD_STANDALONE "Also build the JUCE Standalone app (development and UI screenshots)" OFF)
set(RCV_PLUGIN_FORMATS VST3)
if(RCV_BUILD_STANDALONE)
    list(APPEND RCV_PLUGIN_FORMATS Standalone)
endif()

juce_add_plugin(RetroChip
    PRODUCT_NAME "Retro Chip"
    COMPANY_NAME "retro-chip-vst"
    BUNDLE_ID "com.retro-chip-vst.retrochip"
    PLUGIN_MANUFACTURER_CODE Rcvs
    PLUGIN_CODE Rchp
    FORMATS ${RCV_PLUGIN_FORMATS}
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

# Embedded assets: preset banks, procedurally generated samples and the sample index.
# BinaryData only keeps a file's bare name, and every chip folder has its own clap.wav,
# kick.wav... so the WAVs are staged as "<chip>_<name>.wav" before embedding
# (PresetManager looks them up under that name). Presets stay "<chip>.json"; the sample
# index stays "index.json". Missing folders or files simply embed nothing.
file(GLOB RCV_PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/presets/*.json")
file(GLOB RCV_SAMPLE_INDEX CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/samples/index.json")
file(GLOB RCV_SAMPLE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/samples/*/*.wav")

set(RCV_ASSET_STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/staged_assets")
set(RCV_ASSET_FILES ${RCV_PRESET_FILES} ${RCV_SAMPLE_INDEX})
foreach(_wav IN LISTS RCV_SAMPLE_FILES)
    get_filename_component(_dir "${_wav}" DIRECTORY)
    get_filename_component(_chip "${_dir}" NAME)
    get_filename_component(_name "${_wav}" NAME)
    set(_staged "${RCV_ASSET_STAGE_DIR}/${_chip}_${_name}")
    configure_file("${_wav}" "${_staged}" COPYONLY)
    list(APPEND RCV_ASSET_FILES "${_staged}")
endforeach()

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
        # The factory links whichever real engines are enabled (none with
        # RCV_USE_STUB_ENGINES=ON), so engine sources that are mid-edit do not get compiled.
        chipdsp::factory
        chipdsp::perf
        chipdsp::core
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RCV_PLUGIN_SOURCES})

# Offscreen render of the chip panels with the ENGINE_SPECS parameter sets (layout checks
# while the engines are placeholders). Development tool, off by default.
option(RCV_BUILD_UI_PREVIEW "Build ui_preview (offscreen render of the chip panels)" OFF)
if(RCV_BUILD_UI_PREVIEW)
    add_subdirectory(uipreview)
endif()

# Plugin-core tests (plugin/tests, Catch2). Catch2 is only fetched when RCV_BUILD_TESTS=ON.
if(RCV_BUILD_TESTS)
    set(_rcv_plugin_tests_default ON)
else()
    set(_rcv_plugin_tests_default OFF)
endif()
option(RCV_BUILD_PLUGIN_TESTS "Build the plugin-core tests (needs RCV_BUILD_TESTS=ON for Catch2)" ${_rcv_plugin_tests_default})

if(RCV_BUILD_PLUGIN_TESTS)
    if(TARGET Catch2::Catch2WithMain)
        add_subdirectory(tests)
    else()
        message(WARNING "RCV_BUILD_PLUGIN_TESTS is ON but Catch2 is not available (set RCV_BUILD_TESTS=ON); plugin_tests skipped")
    endif()
endif()
