#pragma once

#include <juce_core/juce_core.h>

namespace carve::model
{

// Replaces a file's contents, safely where it can and plainly where it must.
//
// The safe way is to write a temporary file beside the target and move it
// over: a failure halfway leaves the original. That needs permission to
// create a second file in the same folder, and inside the App Sandbox a file
// the user picked in a save panel comes with permission for that one path
// and nothing next to it. There the only write available is straight onto
// the file, so that is the fallback.
inline bool replaceFileContents (const juce::File& file, const void* data, size_t size)
{
    if (file.replaceWithData (data, size))
        return true;

    juce::FileOutputStream stream (file);

    if (! stream.openedOk())
        return false;

    stream.setPosition (0);
    stream.truncate();

    return stream.write (data, size) && (stream.flush(), stream.getStatus().wasOk());
}

// The same for text, written exactly as juce::File::replaceWithText writes
// it (UTF-8, no byte-order mark, its default line endings), so which of the
// two paths a save took cannot be told from the file.
inline bool replaceFileText (const juce::File& file, const juce::String& text)
{
    if (file.replaceWithText (text))
        return true;

    juce::FileOutputStream stream (file);

    if (! stream.openedOk())
        return false;

    stream.setPosition (0);
    stream.truncate();

    return stream.writeText (text, false, false, "\r\n") && (stream.flush(), stream.getStatus().wasOk());
}

} // namespace carve::model
