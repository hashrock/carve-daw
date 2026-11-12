cmake_minimum_required(VERSION 3.24)

project(CarveDAW VERSION 0.18.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The iPad build. scripts/appstore-ios.sh makes the store's .ipa; for the
# simulator, configure it with the Xcode generator:
#
#   cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DFETCHCONTENT_SOURCE_DIR_TRACKTION=$PWD/build/_deps/tracktion-src
#   cmake --build build-ios --config Debug --target carve -- -sdk iphonesimulator
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(CARVE_IOS TRUE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS version")
elseif(APPLE)
    set(CARVE_IOS FALSE)
    # A plain variable, not a cache entry. project() has already made the
    # cache entry by this line (empty), so a set(... CACHE ...) here changed
    # nothing, and JUCE then filled it in with its own 10.13: the Intel half
    # of the app was being built for a macOS the app has never been run on,
    # while the website said 11. This shadows the cache for everything below.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0")
else()
    set(CARVE_IOS FALSE)
endif()

# The default build is the AGPL binary described in LICENSE. The App Store
# binary is a different licence (JUCE 8 Starter and Tracktion Engine Personal):
# its About text says so, and it hosts no third-party plugins at all. VST3 is
# out because JUCE 8.0.6 ships that SDK under Steinberg's proprietary terms or
# GPL-3.0 only, and this tree has no Steinberg SDK agreement; Audio Units are
# out because the App Sandbox will not load other vendors' code into the
# process. See LICENSING.md.
#
# Do not define JUCE_DISPLAY_SPLASH_SCREEN or JUCE_USE_DARK_SPLASH_SCREEN.
# JUCE 8.0.6 ignores both and warns if they are defined. JUCE_REPORT_APP_USAGE
# is not a macro in this JUCE; there is no usage report left to switch off.
option(CARVE_APP_STORE
    "Mac App Store build: commercial JUCE/Tracktion notices, no plugin hosting"
    OFF)

# An iOS app reaches its users through the App Store and no other way, so
# there is no AGPL iPad binary to build: it is always the store's licences.
if(CARVE_IOS)
    set(CARVE_APP_STORE ON CACHE BOOL "" FORCE)
endif()

if(CARVE_APP_STORE)
    set(CARVE_PLUGIN_HOSTING 0)
else()
    set(CARVE_PLUGIN_HOSTING 1)
endif()

include(FetchContent)

# tracktion_engine bundles JUCE as a git submodule, so no separate JUCE fetch.
# The submodule URL is git@github.com:juce-framework/JUCE.git, which clones
# only on a machine with an SSH key. `git submodule update` does not honour a
# repo-local url.insteadOf, so the rewrite has to be git config in the
# environment of the fetch. Set it for this configure's child processes and
# leave the user's global git config alone. The CI workflow sets the same
# rewrite globally, which is what a runner with no key needs too.
if(DEFINED ENV{GIT_CONFIG_COUNT} AND NOT "$ENV{GIT_CONFIG_COUNT}" STREQUAL "")
    set(_carve_git_config_index "$ENV{GIT_CONFIG_COUNT}")
else()
    set(_carve_git_config_index 0)
endif()
set(ENV{GIT_CONFIG_KEY_${_carve_git_config_index}} "url.https://github.com/.insteadof")
set(ENV{GIT_CONFIG_VALUE_${_carve_git_config_index}} "git@github.com:")
math(EXPR _carve_git_config_next "${_carve_git_config_index} + 1")
set(ENV{GIT_CONFIG_COUNT} "${_carve_git_config_next}")
unset(_carve_git_config_index)
unset(_carve_git_config_next)

FetchContent_Declare(tracktion
    GIT_REPOSITORY https://github.com/Tracktion/tracktion_engine.git
    GIT_TAG        v3.2.0
    GIT_SHALLOW    TRUE
    # Local fixes, each explained in its own patch header:
    #   * fixed-seed oscillator phase, so two renders of a song are identical
    #   * pending live MIDI (note previews) survives the graph rebuild that
    #     every MIDI note edit triggers
    #   * SoundTouch (LGPL) is compiled only when its stretcher is asked
    #     for, which here it never is
    #   * an audio clip already at the output rate skips the sinc resampler,
    #     so it plays bit-for-bit rather than band-limited
    #   * a constant-power pan law whose centre is unity
    # JUCE itself is not patched, and the last step makes sure of it: a
    # checkout from when this tree did patch JUCE (the Ogg writer) is put
    # back as JUCE shipped it.
    # Idempotent: CMake re-runs this on reconfigure, so an already-patched
    # tree has to pass. "Reverse would apply" means the patch is in place.
    # (No semicolons in here: CMake would split the command on them.)
    PATCH_COMMAND  bash -c "(git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-deterministic-osc-phase.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-deterministic-osc-phase.patch') && (git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-live-midi-survives-note-edit-rebuild.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-live-midi-survives-note-edit-rebuild.patch') && (git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-soundtouch-optional.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-soundtouch-optional.patch') && (git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-wave-clip-sinc-only-when-resampling.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-wave-clip-sinc-only-when-resampling.patch') && (git apply --reverse --check '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-pan-law-3db-unity-centre.patch' 2>/dev/null || git apply '${CMAKE_CURRENT_SOURCE_DIR}/patches/tracktion-pan-law-3db-unity-centre.patch') && git -C modules/juce checkout -- ."
    # Keep tracktion's bundled examples (TestRunner etc.) out of the default
    # build: they compile with -Werror and fail on newer clang.
    EXCLUDE_FROM_ALL)

FetchContent_MakeAvailable(tracktion)

# TinySoundFont: the SF2 player behind the SoundFont generator. One header,
# MIT, and no CMakeLists of its own, so making it available only fetches it;
# its implementation is compiled into SoundFontPlugin.cpp alone.
FetchContent_Declare(tinysoundfont
    GIT_REPOSITORY https://github.com/schellingb/TinySoundFont.git
    GIT_TAG        853a0a171759f1ddba0de1442133a75912bbeffa)
FetchContent_MakeAvailable(tinysoundfont)

# WORLD: the vocoder the UTAU singer re-pitches its recordings with (modified
# BSD). Its own CMakeLists builds a static library, `world`; its examples are
# switched off.
set(WORLD_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WORLD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(world
    GIT_REPOSITORY https://github.com/mmorise/World.git
    GIT_TAG        c794856b537758a3d300757db2e5e31685a822c2)
FetchContent_MakeAvailable(world)

# Signalsmith Stretch: the phase vocoder behind the Pitch Shifter (MIT, as is
# the signalsmith-linear it fetches for its FFT). Header-only. It is here
# instead of the SoundTouch that ships inside tracktion_engine because
# SoundTouch is LGPL, and a statically linked App Store binary cannot offer
# the relinking the LGPL asks for.
#
# signalsmith-linear is declared here, ahead of the stretcher that would
# otherwise fetch it itself: that pins it by commit, and it puts
# signalsmith-linear_SOURCE_DIR in this scope, where the include path below
# needs it.
FetchContent_Declare(signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG        de55e6a50ffcf6f8f43f649692d94691c7025151)   # 0.6.4
FetchContent_Declare(signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        a670068d9aeb64913331d5cc29337b19a457a7df)   # 1.4.0
FetchContent_MakeAvailable(signalsmith-linear signalsmith-stretch)

set(CARVE_CORE_SOURCES
    src/model/ModelIds.h
    src/model/SongModel.h
    src/model/SongModel.cpp
    src/model/MidiPatternIO.h
    src/model/MidiPatternIO.cpp
    src/model/Capture.h
    src/model/Capture.cpp
    src/model/DemoSong.h
    src/model/DemoSong.cpp
    src/model/SampleSongs.h
    src/model/SampleSongs.cpp
    src/sync/EditSync.h
    src/sync/EditSync.cpp
    src/sync/EffectChains.h
    src/sync/EffectChains.cpp
    src/sync/RenderPreroll.h
    src/sync/RenderPreroll.cpp
    src/sync/VorbisComments.h
    src/sync/VorbisComments.cpp
    src/plugins/DistortionPlugin.h
    src/plugins/DistortionPlugin.cpp
    src/plugins/SaturationPlugin.h
    src/plugins/SaturationPlugin.cpp
    src/plugins/BitcrusherPlugin.h
    src/plugins/BitcrusherPlugin.cpp
    src/plugins/MeteredCompressorPlugin.h
    src/plugins/MeteredCompressorPlugin.cpp
    src/plugins/DelayPlugin.h
    src/plugins/DelayPlugin.cpp
    src/plugins/GainReduction.h
    src/plugins/OvertopPlugin.h
    src/plugins/OvertopPlugin.cpp
    src/plugins/PitchShifterPlugin.h
    src/plugins/PitchShifterPlugin.cpp
    src/plugins/LimiterPlugin.h
    src/plugins/LimiterPlugin.cpp
    src/plugins/BusCompressorPlugin.h
    src/plugins/BusCompressorPlugin.cpp
    src/plugins/Loudness.h
    src/plugins/ReferencePlayerPlugin.h
    src/plugins/ReferencePlayerPlugin.cpp
    src/plugins/MasterAnalyzerPlugin.h
    src/plugins/MasterAnalyzerPlugin.cpp
    src/plugins/StereoImagerPlugin.h
    src/plugins/StereoImagerPlugin.cpp
    src/plugins/DrumSynthPlugin.h
    src/plugins/DrumSynthPlugin.cpp
    src/plugins/GabberKickDsp.h
    src/plugins/GabberKickDsp.cpp
    src/plugins/GabberKickPlugin.h
    src/plugins/GabberKickPlugin.cpp
    src/plugins/InstrumentData.h
    src/plugins/CtdDecoder.h
    src/plugins/AcousticDrumsPlugin.h
    src/plugins/AcousticDrumsPlugin.cpp
    src/plugins/BassDecoder.h
    src/plugins/BassDecoder.cpp
    src/plugins/ElectricBassPlugin.h
    src/plugins/ElectricBassPlugin.cpp
    src/plugins/SoundFontPlugin.h
    src/plugins/SoundFontPlugin.cpp
    src/plugins/BreakPlugin.h
    src/plugins/BreakPlugin.cpp
    src/plugins/BreakAnalysis.h
    src/plugins/BreakAnalysis.cpp
    src/plugins/UtauPlugin.h
    src/plugins/UtauPlugin.cpp
    src/voice/Voicebank.h
    src/voice/Voicebank.cpp
    src/voice/VoiceSynth.h
    src/voice/VoiceSynth.cpp
    src/plugins/NoteMonitorPlugin.h
    src/plugins/NoteMonitorPlugin.cpp)

# Shift_JIS, for UTAU voicebanks written by Windows UTAU (src/voice). glibc
# has iconv built in; macOS keeps it in a library of its own.
function(carve_link_iconv target)
    if(APPLE)
        target_link_libraries(${target} PRIVATE iconv)
    endif()
endfunction()

# The analysed instruments (Acoustic Drums, Electric Bass), compiled in: see
# src/plugins/InstrumentData.h for why not as content. The tools that made
# them are the web-acoustic-drum and web-bass prototypes; the recordings they
# were analysed from are CC0 (see THIRD_PARTY_NOTICES.txt).
juce_add_binary_data(carve-instrument-data
    HEADER_NAME CarveInstrumentData.h
    NAMESPACE CarveInstrumentData
    SOURCES
        assets/instruments/acoustic-drums.ctd
        assets/instruments/electric-bass.bas)

function(carve_configure_target target)
    target_sources(${target} PRIVATE ${CARVE_CORE_SOURCES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # SYSTEM, and by path rather than by linking the interface targets, so
    # the headers' own warnings stay out of a build with JUCE's flags on.
    target_include_directories(${target} SYSTEM PRIVATE
        ${tinysoundfont_SOURCE_DIR}
        ${signalsmith-stretch_SOURCE_DIR}/include
        ${signalsmith-linear_SOURCE_DIR}/include)
    target_compile_definitions(${target} PRIVATE
        # No tracktion time-stretcher is compiled in. Its only user here was
        # the engine's Pitch Shifter, which src/plugins/PitchShifterPlugin
        # replaces; SoundTouch (LGPL) stays out of every binary. Spelled out
        # as 0 so nobody reads its absence as an oversight.
        TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=${CARVE_PLUGIN_HOSTING}
        $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=${CARVE_PLUGIN_HOSTING}>
        # Always defined, including as 0: the sources branch on them with #if,
        # and an undefined macro would trip -Wundef under JUCE's warning flags.
        CARVE_PLUGIN_HOSTING=${CARVE_PLUGIN_HOSTING}
        CARVE_APP_STORE=$<BOOL:${CARVE_APP_STORE}>
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>")
    target_link_libraries(${target} PRIVATE
        tracktion::tracktion_engine
        world
        carve-instrument-data
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
    carve_link_iconv(${target})
endfunction()

# --- GUI app ---
# JUCE generates platform icons at configure time; regenerate when the PNG changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png")

# CFBundleVersion. The App Store wants a number that goes up with every upload
# of the same version, which the version itself cannot do; scripts/appstore.sh
# passes one. Left alone, it is the version, as it always was.
set(CARVE_BUILD_NUMBER "${PROJECT_VERSION}" CACHE STRING "CFBundleVersion of the app")

# What JUCE's Info.plist does not say by itself, and the App Store reads:
# the category the app is filed under, that it has no encryption to declare
# (it has no network code of its own to encrypt anything for), and the one
# language the interface is in. The first macOS it runs on is here because a
# Makefile build of JUCE leaves it out, and the store then assumes the oldest
# it knows of.
set(CARVE_PLIST_TO_MERGE "<plist><dict>
<key>ITSAppUsesNonExemptEncryption</key><false/>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleLocalizations</key><array><string>en</string></array>")
if(APPLE AND NOT CARVE_IOS)
    string(APPEND CARVE_PLIST_TO_MERGE "
<key>LSApplicationCategoryType</key><string>public.app-category.music</string>
<key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>")
endif()
string(APPEND CARVE_PLIST_TO_MERGE "
</dict></plist>")
juce_add_gui_app(carve
    PRODUCT_NAME "Carve DAW"
    BUNDLE_ID    "com.hashrock.carve"
    COMPANY_NAME "hashrock"
    # Finder's Get Info, as well as the About window. The Personal licence
    # wants the phrase somewhere the binary itself carries.
    COMPANY_COPYRIGHT "Copyright (C) 2026 hashrock. Powered by Tracktion Engine."
    COMPANY_WEBSITE "https://github.com/hashrock/carve-daw"
    ICON_BIG     "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon-1024.png"
    BUILD_VERSION "${CARVE_BUILD_NUMBER}"
    PLIST_TO_MERGE "${CARVE_PLIST_TO_MERGE}"
    # Never: the app opens no audio input on either system (see
    # EngineSetup.h), and an Info.plist that says why it wants the microphone
    # is a claim that it does -- one App Review holds an app to.
    MICROPHONE_PERMISSION_ENABLED FALSE
    # iOS only. iPad alone, any way up, and the app's Documents folder shows
    # in Files so songs can get in and out. The two that would otherwise
    # write a UI... key into the Mac's Info.plist are switched by platform.
    TARGETED_DEVICE_FAMILY "2"
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    REQUIRES_FULL_SCREEN TRUE
    STATUS_BAR_HIDDEN TRUE
    FILE_SHARING_ENABLED ${CARVE_IOS}
    DOCUMENT_BROWSER_ENABLED ${CARVE_IOS}
    BACKGROUND_AUDIO_ENABLED TRUE)
target_sources(carve PRIVATE
    src/app/Main.cpp
    src/app/LegalNotices.h
    src/app/LongPress.h
    src/app/MainComponent.h
    src/app/MainComponent.cpp
    src/app/TransportBar.h
    src/app/TransportBar.cpp
    src/app/ClipPropertiesPanel.h
    src/app/ClipPropertiesPanel.cpp
    src/app/MissingMediaWindow.h
    src/app/MissingMediaWindow.cpp
    src/app/EffectGraphs.h
    src/app/EffectGraphs.cpp
    src/app/EffectParameterWindow.h
    src/app/EffectSlotList.h
    src/app/EffectSlotList.cpp
    src/app/ExportComponent.h
    src/app/ExportComponent.cpp
    src/app/ExportWindow.h
    src/app/FactoryContent.h
    src/app/FaderLookAndFeel.h
    src/app/DrumPadGrid.h
    src/app/DrumPadGrid.cpp
    src/app/SlateEditor.h
    src/app/SlateEditor.cpp
    src/app/SoundFontEditor.h
    src/app/UtauEditor.h
    src/app/RecentMediaList.h
    src/app/GeneratorController.h
    src/app/GeneratorController.cpp
    src/app/GeneratorWindow.h
    src/app/KnobPanel.h
    src/app/MidiInputController.h
    src/app/MidiInputController.cpp
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
    src/app/SelectionModifiers.h
    src/app/SettingsComponent.h
    src/app/SettingsComponent.cpp
    src/app/SettingsWindow.h
    src/app/SampleBrowser.h
    src/app/SampleBrowser.cpp
    src/app/PlaylistComponent.h
    src/app/PlaylistComponent.cpp
    src/app/ShortcutHelpBar.h
    src/app/TimelineView.h
    src/app/TypingKeyboard.h
    src/app/TypingKeyboard.cpp
    src/app/TimeSigSupport.h
    src/app/TouchUi.h
    src/app/SandboxAccess.h
    src/app/AppFiles.h)
# Security-scoped bookmarks are Foundation, so the real thing is Objective-C++.
# The Mac's App Sandbox and the iPad, which keeps a file picked in Files
# readable the same way (see claimChosen).
if(APPLE)
    target_sources(carve PRIVATE src/app/SandboxAccess.mm)
else()
    target_sources(carve PRIVATE src/app/SandboxAccess.cpp)
endif()
carve_configure_target(carve)

# --- Factory content ---
#
# The drum kit in content/ ships with the app as files rather than as binary
# data: the sample browser browses it, and a pad takes a sample by being
# dragged one, so what these have to be is real files in a real folder.
# Copied on every build -- copy_directory only rewrites what changed -- so
# regenerating the kit (scripts/generate-drums.py) is enough to see it.
#
# The source path is a fallback for anything that runs without the copy having
# happened, which in practice means a scratch program linked against these
# objects; see src/app/FactoryContent.h.
target_compile_definitions(carve PRIVATE
    CARVE_SOURCE_CONTENT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/content"
    # Where About looks when the bundle copy below has not happened yet.
    CARVE_SOURCE_NOTICES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
    CARVE_SOURCE_TRACKTION_LOGO="${CMAKE_CURRENT_SOURCE_DIR}/assets/powered-by-tracktion-engine.png")

if(CARVE_IOS)
    # An iOS bundle is flat: no Contents/, the executable at the top.
    set(CARVE_CONTENT_DESTINATION "$<TARGET_BUNDLE_DIR:carve>/Content")
elseif(APPLE)
    set(CARVE_CONTENT_DESTINATION "$<TARGET_BUNDLE_CONTENT_DIR:carve>/Resources/Content")
else()
    set(CARVE_CONTENT_DESTINATION "$<TARGET_FILE_DIR:carve>/Content")
endif()

add_custom_command(TARGET carve POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/content" "${CARVE_CONTENT_DESTINATION}"
    COMMENT "Copying the factory content into the app")

# Beside the factory content: the notices a review of the binary can open,
# and the Tracktion lockup the About window draws. Same two places
# FactoryContent.h searches, plus the source-tree fallback compiled in above.
if(CARVE_IOS)
    set(CARVE_RESOURCE_DIR "$<TARGET_BUNDLE_DIR:carve>")
elseif(APPLE)
    set(CARVE_RESOURCE_DIR "$<TARGET_BUNDLE_CONTENT_DIR:carve>/Resources")
else()
    set(CARVE_RESOURCE_DIR "$<TARGET_FILE_DIR:carve>")
endif()

add_custom_command(TARGET carve POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
            "${CARVE_RESOURCE_DIR}/THIRD_PARTY_NOTICES.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/powered-by-tracktion-engine.png"
            "${CARVE_RESOURCE_DIR}/powered-by-tracktion-engine.png"
    COMMENT "Copying third-party notices into the app")

# The privacy manifest: no tracking, nothing collected, and the reasons for
# the handful of system calls Apple asks a reason for. In every Apple build,
# since it is as true of the disk image as of the store's copy.
if(APPLE)
    add_custom_command(TARGET carve POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/PrivacyInfo.xcprivacy"
                "${CARVE_RESOURCE_DIR}/PrivacyInfo.xcprivacy"
        COMMENT "Copying the privacy manifest into the app")
endif()

# --- Headless renderer CLI ---
# Console programs, here and in the tests below, are for the desktop only.
if(NOT CARVE_IOS)
juce_add_console_app(carve-render
    PRODUCT_NAME "carve-render")
# NoteOffPlayback and SignalPathChecks are tests, but they need a playing
# engine, and carve-render is the console target that already links one -- a
# target of their own would compile the whole of tracktion_engine a third time.
target_sources(carve-render PRIVATE src/RenderMain.cpp tests/NoteOffPlayback.cpp tests/SignalPathChecks.cpp)
target_include_directories(carve-render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
carve_configure_target(carve-render)
endif()


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

if(CARVE_BUILD_TESTS AND NOT CARVE_IOS)
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

    # Their headers as system headers, so JUCE's warning flags (which the sync
    # suite builds with) are not turned on someone else's templates. The
    # SYSTEM option of FetchContent_Declare would say this, but needs 3.25.
    foreach(_carve_dep rapidcheck Catch2)
        get_target_property(_carve_dep_includes ${_carve_dep} INTERFACE_INCLUDE_DIRECTORIES)
        set_target_properties(${_carve_dep} PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_carve_dep_includes}")
    endforeach()
    unset(_carve_dep)
    unset(_carve_dep_includes)

    # Deliberately not carve_configure_target: what is under test is model-layer
    # arithmetic, document edits, and the piano roll's gesture maths -- none of
    # which reaches the engine, and linking tracktion_engine would recompile the
    # whole of it for this one binary. src/app/NoteGestures.h is header-only and
    # free of JUCE, which is what lets a src/app header be tested here at all.
    juce_add_console_app(carve-tests PRODUCT_NAME "carve-tests")
    target_sources(carve-tests PRIVATE
        src/model/SongModel.cpp
        src/model/Capture.cpp
        src/voice/Voicebank.cpp
        src/plugins/BreakAnalysis.cpp
        src/plugins/GabberKickDsp.cpp
        src/sync/VorbisComments.cpp
        tests/BreakSliceProperties.cpp
        tests/GabberKickTests.cpp
        tests/CaptureProperties.cpp
        tests/VoicebankTests.cpp
        tests/VorbisCommentTests.cpp
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
    carve_link_iconv(carve-tests)

    # The same properties, one layer out: what the sync makes of a song. This
    # one does link the engine -- that is the point of it -- which is why it
    # belongs in the app's own build directory, where tracktion is compiled
    # already. `./run.sh --test` builds the model-layer suite in build-tests at
    # Debug; this one comes with the app:
    #
    #   cmake --build build --target carve-sync-tests --parallel
    #   ./build/carve-sync-tests_artefacts/Release/carve-sync-tests
    juce_add_console_app(carve-sync-tests PRODUCT_NAME "carve-sync-tests")
    target_sources(carve-sync-tests PRIVATE tests/SyncProperties.cpp)
    carve_configure_target(carve-sync-tests)
    # Catch2 without its main(): this suite writes its own, so that the engine
    # and JUCE are opened and closed in a scope rather than at atexit. See the
    # note at the bottom of SyncProperties.cpp.
    target_link_libraries(carve-sync-tests PRIVATE Catch2::Catch2 rapidcheck)
endif()
