#pragma once

#include <juce_core/juce_core.h>

namespace carve::sync
{

// Vorbis comments, read and written on a finished Ogg Vorbis file.
//
// The comments a game loop needs (LOOPSTART / LOOPLENGTH) are not among the
// handful JUCE's Ogg writer knows how to write, and it drops every other
// metadata key. Rather than patch JUCE to add them, they go in afterwards:
// the comment header is the second packet of the stream, so it can be
// replaced without touching the audio. Only the header pages are rebuilt;
// every audio page is copied as it is, renumbered if the header came out a
// page longer or shorter.
//
// Nothing in here needs an Ogg library -- the container is a few fixed
// fields and a checksum.

// Sets these comments on the file, replacing any it already has under the
// same names (names are case-insensitive, as the format says) and leaving
// the rest alone. Returns an error, or empty on success; on an error the
// file is untouched.
juce::String setVorbisComments (const juce::File& oggFile, const juce::StringPairArray& comments);

// Every comment in the file, names upper-cased. Empty if it is not an Ogg
// Vorbis file. Where a name appears more than once, the last one wins.
juce::StringPairArray readVorbisComments (const juce::File& oggFile);

} // namespace carve::sync
