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

juce_add_console_app(orionish-te-render
    PRODUCT_NAME "orionish-te-render")
target_sources(orionish-te-render PRIVATE src/RenderMain.cpp src/DemoEdit.h)
target_include_directories(orionish-te-render PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(orionish-te-render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:orionish-te-render,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:orionish-te-render,JUCE_VERSION>")
target_link_libraries(orionish-te-render PRIVATE
    tracktion::tracktion_engine
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
