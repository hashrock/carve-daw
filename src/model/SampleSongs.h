#pragma once

#include "SongModel.h"

namespace carve::model
{

// The songs the app ships with, past the one-screen demo: whole little
// arrangements to open, take apart and play with, each built here rather than
// shipped as a .carve so that none of them can arrive with a missing file or
// a path from somebody else's machine. They use the internal instruments only
// -- the 808 and the synth -- for the same reason.
//
// Names as the menu shows them, in the order it shows them.
juce::StringArray sampleSongNames();

// One of the above, freshly built. An index outside the list gives the demo
// song, so a caller that has been handed a stale menu index still opens
// something rather than nothing.
Song buildSampleSong (int index);

} // namespace carve::model
