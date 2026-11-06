#pragma once

#include <functional>

#include <tracktion_engine/tracktion_engine.h>

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
// the early part is copied into the destination. The broken first block is in
// the part that is thrown away; what is kept is exactly the range asked for,
// sample for sample what it would have been had the renderer been right.
//
// The early part plays whatever the song has there -- nothing before the
// start of the song, or what leads into a loop range. tracktion already
// pre-rolls a little before every render to settle the graph, with the
// transport playing, so a range never started from cold anyway.
struct PrerollRender
{
    juce::File destination;   // where the trimmed file ends up
    juce::File renderFile;    // what the renderer actually writes
    juce::int64 prerollSamples = 0;
};

// How many samples early a render starts: whole blocks, at least half a
// second and at least two blocks, so a graph latency of up to about half a
// second -- far more than a lookahead limiter, as much as a linear-phase EQ --
// still leaves the broken block inside what is cut.
juce::int64 getPrerollSamples (int blockSize, double sampleRate);

// Moves params' start back by the preroll and points its destFile at a
// temporary file. Call before creating the render task.
PrerollRender applyPreroll (te::Renderer::Parameters&);

// Once the render has finished: writes everything after the preroll from the
// render file into the destination, in the same format, sample rate and bit
// depth, then deletes the render file. Returns an error, or empty on success.
// Safe on any thread. shouldStop, if given, is asked between chunks; a stop
// leaves neither file and returns an error.
juce::String finishPreroll (const PrerollRender&, te::Engine&, int bitDepth,
                            std::function<bool()> shouldStop = {});

// Throws a render away -- cancelled or failed -- leaving neither file.
void discardPreroll (const PrerollRender&);

} // namespace carve::sync
