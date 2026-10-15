#include "Capture.h"

#include <algorithm>
#include <cmath>

#include "SongModel.h"

namespace carve::model::capture
{

std::vector<PlayedNote> lastPhrase (const std::vector<PlayedNote>& played, double gapBeats)
{
    if (played.empty())
        return {};

    // Walk forward keeping the latest end so far: a gap opens wherever a note
    // starts that far past everything before it.
    size_t phraseStart = 0;
    double soundingUntil = played.front().endBeat;

    for (size_t i = 1; i < played.size(); ++i)
    {
        if (played[i].startBeat - soundingUntil >= gapBeats)
            phraseStart = i;

        soundingUntil = std::max (soundingUntil, played[i].endBeat);
    }

    return { played.begin() + (std::ptrdiff_t) phraseStart, played.end() };
}

CapturedPattern toPattern (const std::vector<PlayedNote>& phrase, double beatsPerBar)
{
    beatsPerBar = std::max (Pattern::minLengthBeats, beatsPerBar);

    CapturedPattern result;
    result.lengthBeats = beatsPerBar;

    if (phrase.empty())
        return result;

    const auto& first = phrase.front();
    const auto origin = first.startBeat - std::fmod (std::max (0.0, first.barPhase.value_or (0.0)), beatsPerBar);

    const auto lastStart = phrase.back().startBeat - origin;
    result.lengthBeats = (std::floor (lastStart / beatsPerBar) + 1.0) * beatsPerBar;

    for (const auto& note : phrase)
    {
        const auto start = note.startBeat - origin;
        const auto length = std::min (note.endBeat - note.startBeat, result.lengthBeats - start);

        result.notes.push_back ({ start, Note::clampLength (length), note.pitch, note.velocity });
    }

    return result;
}

} // namespace carve::model::capture
