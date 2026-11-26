cmake_minimum_required(VERSION 3.24)

project(CarveDAW VERSION 0.23.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The iPad build. scripts/appstore-ios.sh makes the store's .ipa; for the
# simulator, configure it with the Xcode generator:
#
#   cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
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

# ccache, when it is installed: a rebuild after a branch switch, a fresh
# build directory (build-asan) or a fresh worktree then gets back from the
# cache what any checkout has compiled before. Before any target is made,
# since the launcher is read when a target is created. The Makefile and Ninja
# generators honour it; Xcode does not, and is left alone. A launcher given
# on the command line (the CI workflow's) is left as it is.
#
# base_dir is what makes the cache shared between checkouts: paths inside
# this checkout are hashed relative to the build directory, so the same file
# in two worktrees is the same compile. That holds while build directories
# sit inside the checkout (build/, build-asan/, ...) and nothing outside it
# differs between checkouts -- which is why tracktion lives in the shared
# directory below, at one absolute path for all of them, and why no compile
# line carries this checkout's absolute path in a -D (see the CARVE_SOURCE_
# definitions on Main.cpp).
# -DCARVE_USE_CCACHE=OFF to do without.
option(CARVE_USE_CCACHE "Compile through ccache when it is installed" ON)
if(CARVE_USE_CCACHE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
    find_program(CARVE_CCACHE_PROGRAM ccache)
    if(CARVE_CCACHE_PROGRAM)
        file(REAL_PATH "${CMAKE_SOURCE_DIR}" _carve_base_dir)
        foreach(_carve_lang C CXX OBJC OBJCXX)
            if(NOT CMAKE_${_carve_lang}_COMPILER_LAUNCHER)
                set(CMAKE_${_carve_lang}_COMPILER_LAUNCHER
                    "${CARVE_CCACHE_PROGRAM}" "base_dir=${_carve_base_dir}")
            endif()
        endforeach()
        unset(_carve_lang)
        unset(_carve_base_dir)
    endif()
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

# tracktion_engine, with this tree's fixes to it. Each patch is explained in
# its own header:
#   * fixed-seed oscillator phase, so two renders of a song are identical
#   * pending live MIDI (note previews) survives the graph rebuild that
#     every MIDI note edit triggers
#   * SoundTouch (LGPL) is compiled only when its stretcher is asked
#     for, which here it never is
#   * an audio clip already at the output rate skips the sinc resampler,
#     so it plays bit-for-bit rather than band-limited
#   * a constant-power pan law whose centre is unity
# JUCE itself is not patched.
set(CARVE_TRACKTION_TAG v3.2.0)
set(CARVE_TRACKTION_PATCHES
    tracktion-deterministic-osc-phase.patch
    tracktion-live-midi-survives-note-edit-rebuild.patch
    tracktion-soundtouch-optional.patch
    tracktion-wave-clip-sinc-only-when-resampling.patch
    tracktion-pan-law-3db-unity-centre.patch)

# The checkout (~500MB with JUCE) is kept outside any one build directory, in
# a directory every checkout of this repository shares: a new worktree or
# build directory finds it there instead of cloning its own, and -- because it
# is at the same absolute path for all of them -- ccache finds the engine
# compiled by any of them. Its name carries the tag and a hash of the
# patches, so a branch that changes a patch gets a checkout of its own and
# never edits the one the others are using. Old ones are safe to delete.
#
# -DFETCHCONTENT_SOURCE_DIR_TRACKTION=<dir> still overrides all of this, and
# -DCARVE_DEPS_DIR=<dir> moves the shared directory.
if(APPLE)
    set(_carve_deps_default "$ENV{HOME}/Library/Caches/carve-daw/deps")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_carve_deps_default "$ENV{XDG_CACHE_HOME}/carve-daw/deps")
else()
    set(_carve_deps_default "$ENV{HOME}/.cache/carve-daw/deps")
endif()
set(CARVE_DEPS_DIR "${_carve_deps_default}" CACHE PATH
    "Where the tracktion_engine checkout shared by every checkout of this repository is kept")
unset(_carve_deps_default)

if(NOT FETCHCONTENT_SOURCE_DIR_TRACKTION)
    set(_carve_key "${CARVE_TRACKTION_TAG}")
    foreach(_carve_patch IN LISTS CARVE_TRACKTION_PATCHES)
        file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_carve_patch}" _carve_hash)
        string(APPEND _carve_key " ${_carve_patch}=${_carve_hash}")
    endforeach()
    string(SHA256 _carve_key "${_carve_key}")
    string(SUBSTRING "${_carve_key}" 0 12 _carve_key)
    set(_carve_tracktion_dir "${CARVE_DEPS_DIR}/tracktion-${CARVE_TRACKTION_TAG}-${_carve_key}")

    # Cloned and patched beside its final name and renamed into place only
    # when complete, so a checkout never sees a half-made one, and two
    # configures racing to make it cannot step on each other: the second
    # rename fails, and that configure uses the first one's.
    if(NOT EXISTS "${_carve_tracktion_dir}/.carve-complete")
        string(RANDOM LENGTH 8 _carve_suffix)
        set(_carve_partial "${_carve_tracktion_dir}.partial-${_carve_suffix}")
        message(STATUS "Fetching tracktion_engine ${CARVE_TRACKTION_TAG} into ${_carve_tracktion_dir}")
        file(MAKE_DIRECTORY "${CARVE_DEPS_DIR}")
        execute_process(
            COMMAND git clone --quiet --depth 1 --branch ${CARVE_TRACKTION_TAG}
                    --recurse-submodules --shallow-submodules
                    https://github.com/Tracktion/tracktion_engine.git "${_carve_partial}"
            RESULT_VARIABLE _carve_result)
        if(NOT _carve_result EQUAL 0)
            file(REMOVE_RECURSE "${_carve_partial}")
            message(FATAL_ERROR "Cloning tracktion_engine ${CARVE_TRACKTION_TAG} failed")
        endif()
        foreach(_carve_patch IN LISTS CARVE_TRACKTION_PATCHES)
            execute_process(
                COMMAND git apply "${CMAKE_CURRENT_SOURCE_DIR}/patches/${_carve_patch}"
                WORKING_DIRECTORY "${_carve_partial}"
                RESULT_VARIABLE _carve_result)
            if(NOT _carve_result EQUAL 0)
                file(REMOVE_RECURSE "${_carve_partial}")
                message(FATAL_ERROR "patches/${_carve_patch} does not apply to tracktion_engine ${CARVE_TRACKTION_TAG}")
            endif()
        endforeach()
        file(TOUCH "${_carve_partial}/.carve-complete")
        file(RENAME "${_carve_partial}" "${_carve_tracktion_dir}" RESULT _carve_result)
        if(NOT _carve_result EQUAL 0)
            file(REMOVE_RECURSE "${_carve_partial}")
            if(NOT EXISTS "${_carve_tracktion_dir}/.carve-complete")
                message(FATAL_ERROR "Could not put tracktion_engine at ${_carve_tracktion_dir}: ${_carve_result}")
            endif()
        endif()
    endif()

    # A plain variable, not the cache entry: it follows the patches, so a
    # reconfigure after one changes picks the new checkout.
    set(FETCHCONTENT_SOURCE_DIR_TRACKTION "${_carve_tracktion_dir}")
    unset(_carve_key)
    unset(_carve_hash)
    unset(_carve_patch)
    unset(_carve_suffix)
    unset(_carve_partial)
    unset(_carve_result)
    unset(_carve_tracktion_dir)
endif()

# JUCE configures juceaide in a CMake build of its own under tracktion's build
# directory, and that build refuses to run against a source directory other
# than the one it was made from. So when the tracktion checkout this build
# directory uses moves -- a patch changed, or a build directory made before
# the shared one existed -- tracktion's build directory goes, and is made
# again from the new checkout.
file(REAL_PATH "${FETCHCONTENT_SOURCE_DIR_TRACKTION}" _carve_tracktion_now)
if(NOT CARVE_TRACKTION_SOURCE_IN_USE STREQUAL _carve_tracktion_now)
    file(REMOVE_RECURSE "${FETCHCONTENT_BASE_DIR}/tracktion-build")
    set(CARVE_TRACKTION_SOURCE_IN_USE "${_carve_tracktion_now}" CACHE INTERNAL
        "The tracktion checkout this build directory's tracktion-build was made from")
endif()
unset(_carve_tracktion_now)

FetchContent_Declare(tracktion
    GIT_REPOSITORY https://github.com/Tracktion/tracktion_engine.git
    GIT_TAG        ${CARVE_TRACKTION_TAG}
    # Keep tracktion's bundled examples (TestRunner etc.) out of the default
    # build: they compile with -Werror and fail on newer clang.
    EXCLUDE_FROM_ALL)

# For iOS, JUCE builds juceaide (a host tool, for macOS) while configuring,
# and hands it CMAKE_OSX_DEPLOYMENT_TARGET. From the second configure on,
# that is the 10.13 tracktion's examples leave in the cache, and Xcode 27
# builds nothing for macOS below 12.0. A macOS value while tracktion and
# JUCE are added, then the cache's again: what the app is built for is left
# as it was.
if(CARVE_IOS)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

FetchContent_MakeAvailable(tracktion)

if(CARVE_IOS)
    unset(CMAKE_OSX_DEPLOYMENT_TARGET)
endif()

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
    src/plugins/PianoModel.h
    src/plugins/PianoModel.cpp
    src/plugins/PianoPlugin.h
    src/plugins/PianoPlugin.cpp
    src/plugins/BassModel.h
    src/plugins/BassModel.cpp
    src/plugins/BassModelPlugin.h
    src/plugins/BassModelPlugin.cpp
    src/plugins/FactoryPresets.h
    src/plugins/FactoryPresets.cpp
    src/plugins/DrumModel.h
    src/plugins/DrumModelBessel.h
    src/plugins/DrumModelSimd.h
    src/plugins/DrumModelModal.cpp
    src/plugins/DrumModelSnare.cpp
    src/plugins/DrumModelKit.h
    src/plugins/DrumModelKit.cpp
    src/plugins/DrumModelPlugin.h
    src/plugins/DrumModelPlugin.cpp
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
    src/voice/LyricText.h
    src/voice/LyricText.cpp
    src/voice/VoiceSynth.h
    src/voice/VoiceSynth.cpp
    src/voice/VocalModel.h
    src/voice/VocalModel.cpp
    src/voice/VocalScore.h
    src/voice/VocalScore.cpp
    src/plugins/VocalModelPlugin.h
    src/plugins/VocalModelPlugin.cpp
    src/plugins/NoteMonitorPlugin.h
    src/plugins/NoteMonitorPlugin.cpp)

# The modelled singer follows the web-voice prototype's JavaScript to rounding
# (see src/voice/VocalModel.cpp), which needs each multiply and add rounded on
# its own: no fused multiply-adds, which clang emits by default on arm64.
set_source_files_properties(src/voice/VocalModel.cpp src/voice/VocalScore.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")

# Modelled Drums' engines run a few thousand modes and a grid a sample at a
# time; the PhysKit engines they port were built with fast maths too. Not
# finite-only: they test for NaN-free contact gaps with !(e > 0).
set_source_files_properties(src/plugins/DrumModelModal.cpp src/plugins/DrumModelSnare.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3;-ffast-math;-fno-finite-math-only>")

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
        assets/instruments/electric-bass.bas
        # Modelled Drums: the PhysKit project's real-time models (gzipped
        # PKD2 / PKS1, see src/plugins/DrumModel.h) and its dry kit's cymbals
        # as Ogg Vorbis, made by scripts/build-modelled-drums.py.
        assets/instruments/modelled-drums/tom_hi.bin.gz
        assets/instruments/modelled-drums/tom_mid.bin.gz
        assets/instruments/modelled-drums/tom_floor.bin.gz
        assets/instruments/modelled-drums/snare.bin.gz
        assets/instruments/modelled-drums/kick.bin.gz
        assets/instruments/modelled-drums/cymbals.txt
        assets/instruments/modelled-drums/crash_choke_l1_rr1.ogg
        assets/instruments/modelled-drums/crash_choke_l1_rr2.ogg
        assets/instruments/modelled-drums/crash_choke_l2_rr1.ogg
        assets/instruments/modelled-drums/crash_choke_l2_rr2.ogg
        assets/instruments/modelled-drums/crash_l1_rr1.ogg
        assets/instruments/modelled-drums/crash_l1_rr2.ogg
        assets/instruments/modelled-drums/crash_l2_rr1.ogg
        assets/instruments/modelled-drums/crash_l2_rr2.ogg
        assets/instruments/modelled-drums/crash_l3_rr1.ogg
        assets/instruments/modelled-drums/crash_l3_rr2.ogg
        assets/instruments/modelled-drums/crash_l4_rr1.ogg
        assets/instruments/modelled-drums/crash_l4_rr2.ogg
        assets/instruments/modelled-drums/hh_closed_l1_rr1.ogg
        assets/instruments/modelled-drums/hh_closed_l1_rr2.ogg
        assets/instruments/modelled-drums/hh_closed_l2_rr1.ogg
        assets/instruments/modelled-drums/hh_closed_l2_rr2.ogg
        assets/instruments/modelled-drums/hh_closed_l3_rr1.ogg
        assets/instruments/modelled-drums/hh_closed_l3_rr2.ogg
        assets/instruments/modelled-drums/hh_closed_l4_rr1.ogg
        assets/instruments/modelled-drums/hh_closed_l4_rr2.ogg
        assets/instruments/modelled-drums/hh_open_l1_rr1.ogg
        assets/instruments/modelled-drums/hh_open_l1_rr2.ogg
        assets/instruments/modelled-drums/hh_open_l2_rr1.ogg
        assets/instruments/modelled-drums/hh_open_l2_rr2.ogg
        assets/instruments/modelled-drums/hh_open_l3_rr1.ogg
        assets/instruments/modelled-drums/hh_open_l3_rr2.ogg
        assets/instruments/modelled-drums/hh_pedal_l1_rr1.ogg
        assets/instruments/modelled-drums/hh_pedal_l1_rr2.ogg
        assets/instruments/modelled-drums/hh_pedal_l2_rr1.ogg
        assets/instruments/modelled-drums/hh_pedal_l2_rr2.ogg
        assets/instruments/modelled-drums/ride_bell_l1_rr1.ogg
        assets/instruments/modelled-drums/ride_bell_l1_rr2.ogg
        assets/instruments/modelled-drums/ride_bell_l2_rr1.ogg
        assets/instruments/modelled-drums/ride_bell_l2_rr2.ogg
        assets/instruments/modelled-drums/ride_bell_l3_rr1.ogg
        assets/instruments/modelled-drums/ride_bell_l3_rr2.ogg
        assets/instruments/modelled-drums/ride_l1_rr1.ogg
        assets/instruments/modelled-drums/ride_l1_rr2.ogg
        assets/instruments/modelled-drums/ride_l2_rr1.ogg
        assets/instruments/modelled-drums/ride_l2_rr2.ogg
        assets/instruments/modelled-drums/ride_l3_rr1.ogg
        assets/instruments/modelled-drums/ride_l3_rr2.ogg
        # Modelled Voice (EN)'s pronouncing dictionary: the CMU Pronouncing
        # Dictionary, as the web-voice prototype's tools/build-en-dict.mjs
        # writes it (see THIRD_PARTY_NOTICES.txt).
        assets/voice/en-dict.txt)

# tracktion_engine and JUCE, compiled once. JUCE modules (tracktion_engine is
# one) hand their .cpp files to whatever links them, so the app, carve-render
# and carve-sync-tests each used to compile the whole engine over again: three
# times the work for identical objects. This is how JUCE's own plugin targets
# share their code (_SharedCode): the modules are linked PRIVATE, so their
# sources stay in this library, and the include paths and definitions they
# need are handed on by name.
#
# Nothing of this tree's own is on these compile lines -- no src/, no build
# directory paths -- so every checkout, sharing the one tracktion directory
# (CARVE_DEPS_DIR), compiles the engine with the same command, and ccache
# serves the second from the first.
add_library(carve-engine STATIC)
target_compile_definitions(carve-engine PUBLIC
    # What juce_add_gui_app / juce_add_console_app define for an app's code.
    JUCE_STANDALONE_APPLICATION=1
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
    JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(carve-engine PRIVATE
    tracktion::tracktion_engine
    # SHA-256, to check the sound library's downloads (src/app/SoundLibrary).
    juce::juce_cryptography
    # Not JUCE's warning flags: this is tracktion's and JUCE's code, not
    # ours, and under them it says a couple of hundred things nobody here
    # is going to fix.
    juce::juce_recommended_config_flags)
# Nor clang's own defaults, for the same reason: with them gone, a warning
# in the build is always one of ours.
target_compile_options(carve-engine PRIVATE -w)
# JUCE's LV2 and ARA wrappers compile to nothing with those formats off, and
# Apple's ar and ranlib say so for each archive; there is nothing to do about
# it. ar is told to leave the symbol table (S) to ranlib, which can be told to
# keep quiet.
if(APPLE AND NOT CMAKE_GENERATOR STREQUAL "Xcode")
    foreach(_carve_lang C CXX OBJC OBJCXX)
        set(CMAKE_${_carve_lang}_ARCHIVE_CREATE "<CMAKE_AR> qcS <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_${_carve_lang}_ARCHIVE_APPEND "<CMAKE_AR> qS <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_${_carve_lang}_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols <TARGET>")
    endforeach()
    unset(_carve_lang)
endif()
# The modules' include paths and definitions, for whatever links this -- the
# same re-export JUCE's plugin wrappers do (see _juce_link_plugin_wrapper).
# SYSTEM, so their headers stay quiet in our sources too, which do build
# with the warning flags.
target_include_directories(carve-engine SYSTEM INTERFACE
    $<TARGET_PROPERTY:carve-engine,INCLUDE_DIRECTORIES>)
target_compile_definitions(carve-engine INTERFACE
    $<TARGET_PROPERTY:carve-engine,COMPILE_DEFINITIONS>)

# The model, sync and plugin sources: everything the three engine targets
# share above the engine, also compiled once.
add_library(carve-core STATIC ${CARVE_CORE_SOURCES})
target_include_directories(carve-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# SYSTEM, and by path rather than by linking the interface targets, so
# the headers' own warnings stay out of a build with JUCE's flags on.
target_include_directories(carve-core SYSTEM PUBLIC
    ${tinysoundfont_SOURCE_DIR}
    ${signalsmith-stretch_SOURCE_DIR}/include
    ${signalsmith-linear_SOURCE_DIR}/include)
target_link_libraries(carve-core
    PUBLIC
        carve-engine
        world
        carve-instrument-data
    PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
carve_link_iconv(carve-core)

function(carve_configure_target target)
    target_compile_definitions(${target} PRIVATE
        # Per target, which is why only Main.cpp may read them: the libraries
        # above are shared, and have neither.
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${target},JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${target},JUCE_VERSION>")
    target_link_libraries(${target} PRIVATE
        carve-core
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
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
# (its only network use is HTTPS downloads of the sound library through the
# system's own URL loading, which is exempt), and the one
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
    src/app/VocalModelEditor.h
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
    src/app/SoundLibrary.h
    src/app/SoundLibrary.cpp
    src/app/SoundLibraryWindow.h
    src/app/SoundLibraryWindow.cpp
    src/app/AppFiles.h)
# Security-scoped bookmarks are Foundation, so the real thing is Objective-C++.
# The Mac's App Sandbox and the iPad, which keeps a file picked in Files
# readable the same way (see claimChosen).
if(APPLE)
    target_sources(carve PRIVATE src/app/SandboxAccess.mm src/app/SoundLibraryApple.mm)
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
#
# On the two files that read them and no others: an absolute path of this
# checkout on a compile line makes that compile this checkout's alone, so
# ccache could not share it with another worktree (see CARVE_USE_CCACHE).
set_property(SOURCE src/app/Main.cpp src/app/SampleBrowser.cpp
    TARGET_DIRECTORY carve
    APPEND PROPERTY COMPILE_DEFINITIONS
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
# engine, and carve-render is the console target that already links one.
target_sources(carve-render PRIVATE src/RenderMain.cpp tests/NoteOffPlayback.cpp tests/SignalPathChecks.cpp
    tests/PresetAudition.cpp)
target_include_directories(carve-render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
carve_configure_target(carve-render)
endif()


# --- Tests ---
#
# On by default. It was off to begin with, on the grounds that a plain build of
# the app has no use for Catch2 or RapidCheck -- but a test suite nobody runs
# without knowing to ask for it is a test suite nobody runs, and the two are a
# rounding error next to the ~500MB tracktion checkout the build needs
# anyway. Turn it off with -DCARVE_BUILD_TESTS=OFF.
#
#   ./run.sh --test                   builds and runs them
#
# ...or by hand:
#
#   cmake -B build-tests -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-tests --target carve-tests --parallel
#   ./build-tests/carve-tests_artefacts/Debug/carve-tests
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
        src/voice/LyricText.cpp
        src/plugins/BreakAnalysis.cpp
        src/plugins/GabberKickDsp.cpp
        src/plugins/PianoModel.cpp
        src/plugins/BassModel.cpp
        src/plugins/DrumModelModal.cpp
        src/plugins/DrumModelSnare.cpp
        src/plugins/DrumModelKit.cpp
        src/voice/VocalModel.cpp
        src/voice/VocalScore.cpp
        src/sync/VorbisComments.cpp
        tests/BreakSliceProperties.cpp
        tests/GabberKickTests.cpp
        tests/GeneratorEditTests.cpp
        tests/PianoTests.cpp
        tests/BassModelTests.cpp
        tests/DrumModelTests.cpp
        tests/CaptureProperties.cpp
        tests/ControllerCurveTests.cpp
        tests/VoicebankTests.cpp
        tests/LyricTextTests.cpp
        tests/VocalModelTests.cpp
        tests/VorbisCommentTests.cpp
        tests/NoteGestureProperties.cpp
        tests/SongStateProperties.cpp
        tests/SongTimeProperties.cpp)
    target_include_directories(carve-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    # The JUCE modules it needs, compiled in a library of their own for the
    # same reason as carve-engine: so their warnings can be switched off
    # without switching off ours.
    add_library(carve-tests-juce STATIC)
    target_compile_definitions(carve-tests-juce PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(carve-tests-juce PRIVATE
        juce::juce_dsp
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_data_structures
        juce::juce_recommended_config_flags)
    target_compile_options(carve-tests-juce PRIVATE -w)
    target_include_directories(carve-tests-juce SYSTEM INTERFACE
        $<TARGET_PROPERTY:carve-tests-juce,INCLUDE_DIRECTORIES>)
    target_compile_definitions(carve-tests-juce INTERFACE
        $<TARGET_PROPERTY:carve-tests-juce,COMPILE_DEFINITIONS>)
    target_link_libraries(carve-tests PRIVATE
        carve-instrument-data
        carve-tests-juce
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
