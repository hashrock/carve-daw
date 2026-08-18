cmake_minimum_required(VERSION 3.24)

project(OrionishTracktion VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

include(FetchContent)

# tracktion_engine bundles JUCE as a git submodule, so no separate JUCE fetch.
FetchContent_Declare(tracktion
    GIT_REPOSITORY https://github.com/Tracktion/tracktion_engine.git
    GIT_TAG        v3.2.0
    GIT_SHALLOW    TRUE
    # Keep tracktion's bundled examples (TestRunner etc.) out of the default
    # build: they compile with -Werror and fail on newer clang.
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(tracktion)

set(ORIONISH_CORE_SOURCES
    src/model/ModelIds.h
    src/model/SongModel.h
    src/model/SongModel.cpp
    src/model/DemoSong.h
    src/model/DemoSong.cpp
    src/sync/EditSync.h
    src/sync/EditSync.cpp)

function(orionish_configure_target target)
    target_sources(${target} PRIVATE ${ORIONISH_CORE_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>")
    target_link_libraries(${target} PRIVATE
        tracktion::tracktion_engine
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endfunction()

# --- GUI app ---
juce_add_gui_app(orionish-te
    PRODUCT_NAME "Orionish TE"
    BUNDLE_ID    "com.hashrock.orionish-te"
    MICROPHONE_PERMISSION_ENABLED TRUE)
target_sources(orionish-te PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.h
    src/app/MainComponent.cpp
    src/app/TransportBar.h
    src/app/TransportBar.cpp
    src/app/ClipPropertiesPanel.h
    src/app/ClipPropertiesPanel.cpp
    src/app/GeneratorPanel.h
    src/app/GeneratorPanel.cpp
    src/app/MixerComponent.h
    src/app/MixerComponent.cpp
    src/app/MixerWindow.h
    src/app/PianoRollComponent.h
    src/app/PianoRollComponent.cpp
    src/app/PianoRollWindow.h
    src/app/PluginWindows.h
    src/app/PlaylistComponent.h
    src/app/PlaylistComponent.cpp
    src/app/ShortcutHelpBar.h)
orionish_configure_target(orionish-te)

# --- Headless renderer CLI ---
juce_add_console_app(orionish-te-render
    PRODUCT_NAME "orionish-te-render")
target_sources(orionish-te-render PRIVATE src/RenderMain.cpp)
orionish_configure_target(orionish-te-render)
