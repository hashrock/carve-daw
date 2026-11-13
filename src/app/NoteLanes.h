#pragma once

#include <cmath>
#include <iterator>
#include <optional>
#include <vector>

#include "model/SongModel.h"

namespace carve::app::lanes
{

// What the lane under the piano roll shows and edits. The first five are a
// value per note, drawn as a bar under each one the way velocity always was;
// the last two are a pattern's controller curves (model::Pattern::Controller),
// drawn as a line across it.
enum class Kind { velocity, tune, retrigger, buzzPitch, slideOut, pitchBend, modWheel };

inline bool isPerNote (Kind kind)  { return kind != Kind::pitchBend && kind != Kind::modWheel; }

inline model::Pattern::Controller controllerOf (Kind kind)
{
    return kind == Kind::pitchBend ? model::Pattern::Controller::pitchBend : model::Pattern::Controller::modWheel;
}

inline const char* nameOf (Kind kind)
{
    switch (kind)
    {
        case Kind::velocity:   return "Velocity";
        case Kind::tune:       return "Tune";
        case Kind::retrigger:  return "Retrigger";
        case Kind::buzzPitch:  return "Buzz Pitch";
        case Kind::slideOut:   return "Slide Out";
        case Kind::pitchBend:  return "Pitch Bend";
        case Kind::modWheel:   return "Mod Wheel";
    }

    return "";
}

// What the lane's gutter says, in the width of the roll's keyboard.
inline const char* shortNameOf (Kind kind)
{
    switch (kind)
    {
        case Kind::velocity:   return "Vel";
        case Kind::tune:       return "Tune";
        case Kind::retrigger:  return "Roll";
        case Kind::buzzPitch:  return "Buzz";
        case Kind::slideOut:   return "Slide";
        case Kind::pitchBend:  return "Bend";
        case Kind::modWheel:   return "Mod";
    }

    return "";
}

// The lanes a generator has: velocity always; what its instrument reads of
// the rest. A lane for a value nothing plays would be a control that does
// nothing.
inline std::vector<Kind> kindsFor (const model::Generator& generator)
{
    std::vector<Kind> kinds { Kind::velocity };

    if (generator.isBreak())
        kinds.insert (kinds.end(), { Kind::tune, Kind::retrigger, Kind::buzzPitch });

    if (generator.isElectricBass())
        kinds.push_back (Kind::slideOut);

    if (generator.readsController (model::Pattern::Controller::pitchBend))
        kinds.push_back (Kind::pitchBend);

    if (generator.readsController (model::Pattern::Controller::modWheel))
        kinds.push_back (Kind::modWheel);

    return kinds;
}

//==============================================================================
// Per-note values, as whole steps from minOf to maxOf: a bar's height in the
// lane is its step. Bipolar ones are drawn up and down from a centre line.

inline int minOf (Kind kind)
{
    switch (kind)
    {
        case Kind::velocity:   return model::Note::quietestVelocity;
        case Kind::tune:       return -model::Note::maxTune;
        case Kind::retrigger:  return 0;
        case Kind::buzzPitch:  return model::Note::lowestBuzzPitch;
        case Kind::slideOut:   return -(int) std::size (model::Note::slideLengths);
        case Kind::pitchBend:
        case Kind::modWheel:   return 0;
    }
}

inline int maxOf (Kind kind)
{
    switch (kind)
    {
        case Kind::velocity:   return model::Note::loudestVelocity;
        case Kind::tune:       return model::Note::maxTune;
        case Kind::retrigger:  return (int) std::size (model::Note::retriggerDivisions);
        case Kind::buzzPitch:  return model::Note::highestBuzzPitch;
        case Kind::slideOut:   return (int) std::size (model::Note::slideLengths);
        case Kind::pitchBend:
        case Kind::modWheel:   return 1;
    }
}

inline bool isBipolar (Kind kind)  { return kind == Kind::tune || kind == Kind::slideOut || kind == Kind::pitchBend; }

inline int valueOf (Kind kind, const model::Note& note)
{
    switch (kind)
    {
        case Kind::velocity:   return note.getVelocity();
        case Kind::tune:       return note.getTune();
        case Kind::buzzPitch:  return note.getBuzzPitch();

        case Kind::retrigger:
        {
            // Off is step 0, then the divisions from slowest to fastest.
            const auto division = note.getRetrigger();

            for (int i = 0; i < (int) std::size (model::Note::retriggerDivisions); ++i)
                if (model::Note::retriggerDivisions[i] == division)
                    return i + 1;

            return 0;
        }

        case Kind::slideOut:
        {
            // None is step 0; then the lengths, signed by the direction.
            const auto beats = note.getSlideOut();

            if (std::abs (beats) < 1.0e-9)
                return 0;

            int nearest = 0;

            for (int i = 1; i < (int) std::size (model::Note::slideLengths); ++i)
                if (std::abs (std::abs (beats) - model::Note::slideLengths[i])
                     < std::abs (std::abs (beats) - model::Note::slideLengths[nearest]))
                    nearest = i;

            return (beats < 0.0 ? -1 : 1) * (nearest + 1);
        }

        case Kind::pitchBend:
        case Kind::modWheel:
            return 0;
    }
}

inline void setValue (Kind kind, model::Note note, int value, juce::UndoManager* um)
{
    value = juce::jlimit (minOf (kind), maxOf (kind), value);

    if (value == valueOf (kind, note))
        return;

    switch (kind)
    {
        case Kind::velocity:   note.setVelocity (value, um); break;
        case Kind::tune:       note.setTune (value, um); break;
        case Kind::buzzPitch:  note.setBuzzPitch (value, um); break;

        case Kind::retrigger:
            note.setRetrigger (value == 0 ? 0 : model::Note::retriggerDivisions[value - 1], um);
            break;

        case Kind::slideOut:
            note.setSlideOut (value == 0 ? 0.0 : (value < 0 ? -1.0 : 1.0) * model::Note::slideLengths[std::abs (value) - 1], um);
            break;

        case Kind::pitchBend:
        case Kind::modWheel:
            break;
    }
}

// A step as the gutter shows it while a bar is dragged.
inline juce::String describe (Kind kind, int value)
{
    switch (kind)
    {
        case Kind::tune:
            return value > 0 ? "+" + juce::String (value) : juce::String (value);

        case Kind::retrigger:
            return value == 0 ? juce::String ("off")
                              : "1/" + juce::String (model::Note::retriggerDivisions[value - 1] * 4);

        case Kind::buzzPitch:
            return juce::MidiMessage::getMidiNoteName (value, true, true, 3);

        case Kind::slideOut:
        {
            if (value == 0)
                return "none";

            const auto beats = model::Note::slideLengths[std::abs (value) - 1];
            return juce::String (value < 0 ? "down " : "up ") + "1/" + juce::String (juce::roundToInt (4.0 / beats));
        }

        case Kind::velocity:
        case Kind::pitchBend:
        case Kind::modWheel:
            return juce::String (value);
    }
}

// A controller value as the gutter shows it.
inline juce::String describe (Kind kind, float value)
{
    if (kind == Kind::pitchBend)
        return (value > 0.0f ? "+" : "") + juce::String (value * 2.0f, 2) + " st";

    return juce::String (juce::roundToInt (value * 127.0f));
}

} // namespace carve::app::lanes
