# Third-party dependencies, fetched as release archives (no git needed).
#
#   JUCE   9.0.3   https://github.com/juce-framework/JUCE/releases/tag/9.0.3
#   Catch2 v3.16.0 https://github.com/catchorg/Catch2/releases/tag/v3.16.0
#
# Archives are cached in ${CMAKE_BINARY_DIR}/_deps. To use a pre-downloaded
# copy, set FETCHCONTENT_SOURCE_DIR_JUCE / FETCHCONTENT_SOURCE_DIR_CATCH2.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

if(RCV_BUILD_TESTS)
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

if(RCV_BUILD_PLUGIN)
    # Keep JUCE's own targets quiet and lean: we only need the plugin client and GUI.
    set(JUCE_MODULES_ONLY OFF CACHE BOOL "" FORCE)
    set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JUCE_ENABLE_MODULE_SOURCE_GROUPS ON CACHE BOOL "" FORCE)

    FetchContent_Declare(JUCE
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.3.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()
