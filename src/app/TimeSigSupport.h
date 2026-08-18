#pragma once

#include <functional>

#include <juce_data_structures/juce_data_structures.h>

#include "model/SongModel.h"

namespace carve::app
{

// Shared by the views that have to say what a bar is.
//
// Bars are how patterns and clips are thought about, but the model stores
// beats: the song's signature map is what turns one into the other, and it can
// change part way through. Everything here exists so that no view has to
// assume four beats to a bar.

// "4/4", for a label that has to say which signature its bars are counted in.
inline juce::String timeSigText (model::TimeSignature sig)
{
    return juce::String (sig.numerator) + "/" + juce::String (sig.denominator);
}

// A number with its trailing zeros gone, for the readouts that spell out a
// length the spinners round. Zeros are only trimmed past the decimal point:
// trimming "0" off "16.00" without checking for one would leave "1".
inline juce::String trimmedNumber (double value)
{
    auto text = juce::String (value, 3);

    if (text.containsChar ('.'))
        text = text.trimCharactersAtEnd ("0").trimCharactersAtEnd (".");

    return text;
}

// Whether the song is in more than one time signature, which is what decides
// whether a bar count is a property of the song or only of one point in it.
// A change sitting at beat 0 is not a change -- it only says what the song is
// in -- and neither is a later one restating the signature already in force,
// which is what dropping one onto a bar that was already in it leaves behind.
inline bool songSignatureVaries (const model::Song& song)
{
    model::TimeSignature sig;   // 4/4 until the first change

    for (const auto& change : song.getTimeSigChanges())
    {
        if (change.getStartBeat() > 0.0 && change.getSignature() != sig)
            return true;

        sig = change.getSignature();
    }

    return false;
}

// Watches a song for time signature edits and nothing else.
//
// The pattern editor counts bars out of the song's signature map, but what it
// is handed is a Pattern -- so without this it would never hear a signature
// being added, moved or retyped. Listening to the whole song tree instead
// would fire on every note the user draws.
class TimeSigWatcher : private juce::ValueTree::Listener
{
public:
    ~TimeSigWatcher() override  { detach(); }

    // Fired after any edit to the song's time signature map.
    std::function<void()> onChanged;

    void setSong (juce::ValueTree newSong)
    {
        detach();
        songState = std::move (newSong);

        if (songState.isValid())
            songState.addListener (this);
    }

private:
    void detach()
    {
        if (songState.isValid())
            songState.removeListener (this);
    }

    // The container counts too: undoing the first signature change takes the
    // whole TIMESIGS branch away in one go, and only its own type is left.
    static bool isTimeSigTree (const juce::ValueTree& v)
    {
        return v.hasType (model::ids::TIMESIG) || v.hasType (model::ids::TIMESIGS);
    }

    void notifyChanged() const
    {
        if (onChanged)
            onChanged();
    }

    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier&) override
    {
        if (isTimeSigTree (tree))
            notifyChanged();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child) override
    {
        if (isTimeSigTree (child))
            notifyChanged();
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int) override
    {
        if (isTimeSigTree (child))
            notifyChanged();
    }

    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override  {}
    void valueTreeParentChanged (juce::ValueTree&) override                {}

    juce::ValueTree songState;
};

} // namespace carve::app
