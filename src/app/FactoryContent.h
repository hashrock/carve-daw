#pragma once

#include <cstring>

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

// A path into the app's own files as another copy of the app had them, moved
// to where this one has them -- or File() for any other path.
//
// Songs and the browser keep absolute paths, and two kinds of them point
// into the app itself: the factory content inside the bundle, and, on the
// iPad, the app's Documents folder. Both move. iOS gives the app a new bundle
// folder and data folder on every update, so a path stored yesterday names a
// folder that no longer exists; on the Mac the bundle moves when the app does;
// and a song made on one goes to the other with the other's paths in it. The
// part inside the content (or Documents) folder is the same in every copy,
// so that is what is kept.
inline juce::File relocateIntoThisInstall (const juce::File& file)
{
    const auto path = file.getFullPathName();

    // A bundle's content folder, in either system's layout, and only if it
    // is a bundle of this app's.
    for (const auto* marker : { ".app/Contents/Resources/Content/", ".app/Content/" })
        if (const auto at = path.lastIndexOf (marker); at >= 0 && path.substring (0, at).endsWith ("/Carve DAW"))
            if (const auto content = factoryContentFolder(); content != juce::File())
                return content.getChildFile (path.substring (at + (int) std::strlen (marker)));

   #if JUCE_IOS
    // Another install's Documents: .../Data/Application/<id>/Documents/...
    if (const auto at = path.indexOf ("/Data/Application/"); at >= 0)
        if (const auto docs = path.indexOf (at, "/Documents/"); docs >= 0)
            return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                       .getChildFile (path.substring (docs + (int) std::strlen ("/Documents/")));
   #endif

    return {};
}

} // namespace carve::app
