#pragma once

#include <optional>

#include <juce_audio_basics/juce_audio_basics.h>

#include "Voicebank.h"

namespace carve::voice
{

// One sung note, the way UTAU's resampler makes it: a stretch of a recording
// (an oto.ini entry) re-pitched and re-timed with the WORLD vocoder, which
// moves the pitch and length without moving the formants -- the voice keeps
// sounding like the singer an octave away from where it was recorded.
//
// The output starts where the note's sound starts: preutterMs before the note
// itself, the consonant sounding ahead of the beat as a singer's does.
struct NoteRender
{
    OtoEntry entry;

    // Possibly shortened from the entry's own (see UtauPlugin's scaling for a
    // note that follows a short one); the recording is entered later by the
    // difference, so the note still begins at the same point of it.
    double preutterMs = 0.0;

    double lengthMs = 0.0;      // the whole output, preutterance included
    double targetHz = 440.0;
    std::optional<double> fromHz;   // the note before's pitch, when it leads straight in

    float gain = 1.0f;
    double fadeInMs = 0.0;      // over the previous note's tail: this one's overlap
    double fadeOutMs = 0.0;     // under the next note's head: its overlap

    // The voice's colouring (model::Generator::VoiceStyle, same ranges). The
    // defaults change nothing.
    double gender = 0.0, breath = 50.0, brightness = 0.0, modulation = 0.0;
};

struct RenderedNote
{
    juce::AudioBuffer<float> audio;   // mono
    double sampleRate = 44100.0;
};

// Synthesises a note. Reads and analyses the recording on the calling thread
// -- the analysis of a recent stretch is kept, so the same syllable sung
// again only costs the synthesis. Nothing, with the reason in `error`, when
// the recording cannot be read.
std::optional<RenderedNote> renderNote (const NoteRender&, juce::String& error);

// The recording an entry names, as it was recorded: from offset to cutoff,
// mono, untouched -- for playing notes live, where WORLD is too slow to wait
// for. Also its pitch, for the live notes to be transposed from.
struct RawSample
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    double hz = 0.0;                 // its median voiced pitch; 0 if none was found
    double startMs = 0.0;            // where the vowel can be held from (after the consonant)
};

std::optional<RawSample> readRawSample (const OtoEntry&, juce::String& error);

inline double hzForMidiNote (double note)  { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

} // namespace carve::voice
