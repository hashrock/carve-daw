#pragma once

#include <juce_core/juce_core.h>

namespace carve::app
{

// The content that ships with the app: the factory drum kit today, whatever
// else earns its place later. It is copied in at build time (see the
// POST_BUILD command in CMakeLists.txt) rather than installed, so finding it
// is a matter of looking in the two places the build puts it.
inline juce::File factoryContentFolder()
{
    // The bundle, which is where a released app has it.
    const auto inBundle = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                              .getChildFile ("Contents/Resources/Content");

    if (inBundle.isDirectory())
        return inBundle;

    // Beside the executable, which is where a plain (non-bundled) build has it.
    const auto beside = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                            .getSiblingFile ("Content");

    if (beside.isDirectory())
        return beside;

   #ifdef CARVE_SOURCE_CONTENT_DIR
    // The source tree: for anything linked against these objects that was
    // never through the build's copy step at all.
    if (const juce::File inSource (CARVE_SOURCE_CONTENT_DIR); inSource.isDirectory())
        return inSource;
   #endif

    return {};
}

// The factory drum kit, or nothing if the content did not come along. Callers
// have to cope with the empty case: an app run out of a half-built tree is
// still an app, and one missing folder must not be the end of it.
inline juce::File factoryDrumsFolder()
{
    const auto content = factoryContentFolder();

    if (content == juce::File())
        return {};

    const auto drums = content.getChildFile ("Drums");
    return drums.isDirectory() ? drums : juce::File();
}

} // namespace carve::app
