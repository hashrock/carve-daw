#pragma once

#include <juce_core/juce_core.h>

namespace carve::app::appfiles
{

// Where the iPad build writes what it makes.
//
// A save panel on the iPad is the document picker, and a file it names
// outside the app is one the app may only touch in place: no temporary file
// beside it to rename over it, no new files in its folder -- which is how
// songs, renders and stems are all written. So on the iPad the app writes
// into its own Documents folder, which Files shows as On My iPad > Carve DAW,
// and the user carries files on from there. On the desktop nothing here is
// used; the save panels go wherever the user likes.
#if JUCE_IOS
inline constexpr bool writesIntoDocuments = true;
#else
inline constexpr bool writesIntoDocuments = false;
#endif

inline juce::File documents()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
}

// Renders, stems, MIDI and the rest: kept apart from the songs.
inline juce::File exports()
{
    auto folder = documents().getChildFile ("Exports");
    folder.createDirectory();
    return folder;
}

// `name` (with `extension`, dot and all, or none for a folder) in `folder`,
// numbered rather than writing over something already there.
inline juce::File freshChild (const juce::File& folder, const juce::String& name, const juce::String& extension)
{
    return folder.getNonexistentChildFile (juce::File::createLegalFileName (name), extension, true);
}

inline bool isInDocuments (const juce::File& file)
{
    return file.isAChildOf (documents());
}

// A place as the user would find it: through Files on the iPad -- the real
// path there is a container's, a long string of IDs -- and as a path
// everywhere else.
inline juce::String describe (const juce::File& file)
{
    if (writesIntoDocuments && isInDocuments (file))
        return "On My iPad > Carve DAW > " + file.getRelativePathFrom (documents()).replace ("/", " > ");

    return file.getFullPathName();
}

} // namespace carve::app::appfiles
