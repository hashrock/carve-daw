cmake_minimum_required(VERSION 3.24)

project(CarveDAW VERSION 0.1.0 LANGUAGES C CXX)

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
    # Local fixes, each explained in its own patch header:
    #   * fixed-seed oscillator phase, so two renders of a song are identical
    #   * pending live MIDI (note previews) survives the graph rebuild that
    #     every MIDI note edit triggers
    # Idempotent: CMake re-runs this on reconfigure, so an already-patched
    # tree has to pass. "Reverse would apply" means the patch is in place.
    # (No semicolons in here: CMake would split the command on them.)
    PATCH_COMMAND  bash -c "(git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-deterministic-osc-phase.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-deterministic-osc-phase.patch') && (git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-live-midi-survives-note-edit-rebuild.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-live-midi-survives-note-edit-rebuild.patch')"
    # Keep tracktion's bundled examples (TestRunner etc.) out of the default
    # build: they compile with -Werror and fail on newer clang.
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(tracktion)

set(CARVE_CORE_SOURCES
    src/model/ModelIds.h
    src/model/SongModel.h
    src/model/SongModel.cpp
    src/model/MidiPatternIO.h
    src/model/MidiPatternIO.cpp
    src/model/DemoSong.h
    src/model/DemoSong.cpp
    src/sync/EditSync.h
    src/sync/EditSync.cpp
    src/plugins/DistortionPlugin.h
    src/plugins/DistortionPlugin.cpp
    src/plugins/DrumSynthPlugin.h
    src/plugins/DrumSynthPlugin.cpp)

function(carve_configure_target target)
    target_sources(${target} PRIVATE ${CARVE_CORE_SOURCES})
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
juce_add_gui_app(carve
    PRODUCT_NAME "Carve DAW"
    BUNDLE_ID    "com.hashrock.carve"
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png"
    MICROPHONE_PERMISSION_ENABLED TRUE)
target_sources(carve PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.h
    src/app/MainComponent.cpp
    src/app/TransportBar.h
    src/app/TransportBar.cpp
    src/app/ClipPropertiesPanel.h
    src/app/ClipPropertiesPanel.cpp
    src/app/EffectParameterWindow.h
    src/app/EffectSlotList.h
    src/app/EffectSlotList.cpp
    src/app/ExportComponent.h
    src/app/ExportComponent.cpp
    src/app/ExportWindow.h
    src/app/DrumPadGrid.h
    src/app/DrumPadGrid.cpp
    src/app/FourOscEditor.h
    src/app/FourOscEditor.cpp
    src/app/GeneratorController.h
    src/app/GeneratorController.cpp
    src/app/GeneratorWindow.h
    src/app/MixerComponent.h
    src/app/MixerComponent.cpp
    src/app/MixerWindow.h
    src/app/ParameterRows.h
    src/app/PianoRollComponent.h
    src/app/PianoRollComponent.cpp
    src/app/PianoRollWindow.h
    src/app/PluginWindows.h
    src/app/PresetManager.h
    src/app/PresetManager.cpp
    src/app/PlaylistComponent.h
    src/app/PlaylistComponent.cpp
    src/app/ShortcutHelpBar.h
    src/app/TimelineView.h
    src/app/TimeSigSupport.h)
carve_configure_target(carve)

# --- Headless renderer CLI ---
juce_add_console_app(carve-render
    PRODUCT_NAME "carve-render")
target_sources(carve-render PRIVATE src/RenderMain.cpp)
carve_configure_target(carve-render)


# --- Tests ---
#
# On by default. It was off to begin with, on the grounds that a plain build of
# the app has no use for Catch2 or RapidCheck -- but a test suite nobody runs
# without knowing to ask for it is a test suite nobody runs, and the two are a
# rounding error next to the ~500MB tracktion checkout that every configure
# already pays for. Turn it off with -DCARVE_BUILD_TESTS=OFF.
#
#   ./run.sh --test                   builds and runs them
#
# ...or by hand, against the tracktion checkout the app build already fetched:
#
#   cmake -B build-tests -DCMAKE_BUILD_TYPE=Debug \
#         -DFETCHCONTENT_SOURCE_DIR_TRACKTION=$PWD/build/_deps/tracktion-src
#   cmake --build build-tests --target carve-tests --parallel
#   ./build-tests/carve-tests_artefacts/Debug/carve-tests
#
# The tracktion source directory is the same reuse run.sh does for --asan: the
# tests need JUCE, which only arrives inside that checkout, and fetching a
# second copy costs ~500MB.
#
# Run the binary rather than ctest: enable_testing() here would also pull in
# the tests tracktion registers for its own bundled examples, which this build
# excludes, so every one of them would report as not run.
#
# The properties run 100 cases each by default. To lean on them harder, or to
# replay a failure, RapidCheck reads its settings from the environment:
#
#   RC_PARAMS="max_success=20000 max_size=200 seed=1" ...carve-tests
option(CARVE_BUILD_TESTS "Fetch Catch2/RapidCheck and build the test target" ON)

if(CARVE_BUILD_TESTS)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.9.1
        GIT_SHALLOW    TRUE)

    # RapidCheck has no tagged releases, hence a commit. Its own Catch
    # integration stays off: that one targets Catch2 v2, and rc::check() works
    # from inside a v3 TEST_CASE without it.
    set(RC_ENABLE_CATCH OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(rapidcheck
        GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
        GIT_TAG        6e8dadfdafa3a74eabb52ead87f8787f72eccd0b)

    FetchContent_MakeAvailable(Catch2 rapidcheck)

    # Deliberately not carve_configure_target: what is under test is model-layer
    # arithmetic, document edits, and the piano roll's gesture maths -- none of
    # which reaches the engine, and linking tracktion_engine would recompile the
    # whole of it for this one binary. src/app/NoteGestures.h is header-only and
    # free of JUCE, which is what lets a src/app header be tested here at all.
    juce_add_console_app(carve-tests PRODUCT_NAME "carve-tests")
    target_sources(carve-tests PRIVATE
        src/model/SongModel.cpp
        tests/NoteGestureProperties.cpp
        tests/SongStateProperties.cpp
        tests/SongTimeProperties.cpp)
    target_include_directories(carve-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(carve-tests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(carve-tests PRIVATE
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_data_structures
        Catch2::Catch2WithMain
        rapidcheck
        juce::juce_recommended_config_flags)
endif()
