#pragma once

#include <functional>
#include <optional>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::sync
{

// Renders that start early and throw the start away.
//
// tracktion's renderer gets the first block of a render wrong whenever the
// graph has latency -- the master limiter's lookahead is enough. It drops the
// latency's worth of samples off the front of the output, as it should, but
// the first block's audio arrives a further latency late, and the second
// block is on time again. A sound at the very start of a render therefore
// comes out as a stretch of silence, then that sound late, then a jump back
// into place: a click about 10ms in for a 512-sample block. Nothing in
// Renderer::Parameters reaches this, and the code is the same upstream.
//
// So the render starts some blocks before the range that was asked for, into
// a temporary file beside the destination, and once it is done everything but
// the early part is written into the destination. The broken first block is
// in the part that is thrown away; what is kept is exactly the range asked
// for, sample for sample what it would have been had the renderer been right.
//
// The early part plays whatever the song has there -- nothing before the
// start of the song, or what leads into a loop range. tracktion already
// pre-rolls a little before every render to settle the graph, with the
// transport playing, so a range never started from cold anyway.
//
// The temporary file is always 32-bit float WAV, whatever the destination
// is to be. Writing the destination is then the one and only conversion: to
// the bit depth asked for (exactly what the renderer would have written,
// since it converts from the same floats with the same writer), or to Ogg
// Vorbis without first being rounded to integers.
struct PrerollRender
{
    juce::File destination;   // where the finished file ends up
    juce::File renderFile;    // what the renderer actually writes
    juce::int64 prerollSamples = 0;
};

// The Ogg Vorbis quality a render is written at unless asked otherwise: an
// index into OggVorbisAudioFormat::getQualityOptions(), 192 kbps.
inline constexpr int defaultOggQualityIndex = 6;

// What the destination is to be.
struct RenderOutput
{
    juce::AudioFormat* format = nullptr;   // nullptr: WAV
    int bitDepth = 24;                     // ignored by formats with one depth (Ogg)
    int qualityIndex = 0;                  // into format->getQualityOptions(), where it has any
    // e.g. gameLoopMetadata(). For Ogg these are written as Vorbis comments
    // under their own names, once the file is finished.
    juce::StringPairArray metadata;

    // The exact number of samples to write, or negative for "everything the
    // render produced". A game loop needs this: its loop points are sample
    // positions, and the loop ends where the file does, so the file cannot
    // be a rounding error longer or shorter than the song says.
    juce::int64 exactLength = -1;
};

// How many samples early a render starts: whole blocks, at least half a
// second and at least two blocks, so a graph latency of up to about half a
// second -- far more than a lookahead limiter, as much as a linear-phase EQ --
// still leaves the broken block inside what is cut.
juce::int64 getPrerollSamples (int blockSize, double sampleRate);

// Moves params' start back by the preroll and points it at a temporary float
// WAV file. Call before creating the render task.
PrerollRender applyPreroll (te::Renderer::Parameters&, te::Engine&);

// Once the render has finished: writes everything after the preroll from the
// render file into the destination, as `output` says, then deletes the
// render file. Returns an error, or empty on success. Safe on any thread.
// shouldStop, if given, is asked between chunks; a stop leaves neither file
// and returns an error.
juce::String finishPreroll (const PrerollRender&, te::Engine&, const RenderOutput&,
                            std::function<bool()> shouldStop = {});

// Throws a render away -- cancelled or failed -- leaving neither file.
void discardPreroll (const PrerollRender&);

//==============================================================================
// Game loops: a file that plays the song from its start to the end of its
// loop range, and says where the loop begins, so a game can play the intro
// once and repeat the rest. What goes past the end of the loop is cut, not
// folded back in.
//
// The loop is the song's LOOP range, the same one the transport plays, and
// the points are written as the two Vorbis comments games read: LOOPSTART,
// the first sample of the loop, and LOOPLENGTH, how many samples it runs.
// (Only Ogg Vorbis carries them. They are added to the finished file; see
// VorbisComments.h.)
struct GameLoop
{
    te::TimeRange range;            // what to render: song start to loop end
    juce::int64 loopStart = 0;      // samples from the start of the file
    juce::int64 loopLength = 0;     // samples; the loop ends where the file does

    juce::int64 getFileLength() const  { return loopStart + loopLength; }
};

// The song's game loop at this sample rate, or nothing when it has no loop
// range. The edit has to be synced to the song: beats become seconds by its
// tempo track.
std::optional<GameLoop> getGameLoop (const model::Song&, const te::Edit&, double sampleRate);

juce::StringPairArray gameLoopMetadata (const GameLoop&);

} // namespace carve::sync
