#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins::presets
{

// The sounds the built-in instruments ship with, and the one way of putting a
// set of settings onto an internal plugin that both they and the user's own
// presets (app/PresetManager) go through.
//
// A factory preset is the same thing a saved .carvepreset is: the properties
// of the plugin's tree. Loading one replaces the plugin's settings with the
// preset's, and what the preset does not mention goes back to its default --
// so a factory preset sounds the same whichever sound was loaded before it,
// and a song saved after loading one stores nothing new.
//
// The Kick Designer keeps its sounds in a table of its own (GabberKickDsp),
// because its patch is more than a list of numbers -- its drive stages are a
// JSON property -- and the DSP's tests render it without the engine. It is
// listed here all the same, so the preset bar asks one place for every
// instrument. The Modelled Bass's table (BassModel.h) is likewise the model's
// own, but it is plain numbers, so it goes through the same code as the rest.

// The preset names for this plugin, in menu order; empty for a plugin that
// ships none (and for every external plugin, whose programs are its own).
juce::StringArray getFactoryPresetNames (te::Plugin&);

bool hasFactoryPresets (te::Plugin&);

// Applies one; false for an index out of range or a plugin with none.
// Not undoable, like every other parameter move in the app.
bool applyFactoryPreset (te::Plugin&, int index);

// The factory preset the plugin's settings are exactly, or -1.
int findMatchingFactoryPreset (te::Plugin&);

//==============================================================================
// The shared mechanics.

// What in a plugin's tree says where it sits in the Edit rather than how it
// sounds -- its id, its window, the bypass switch, the effect id EditSync
// reconciles by. None of it travels in a preset.
bool isStructuralProperty (const juce::Identifier&);

// Automation and modulation belong to the song, not to the sound.
bool isStructuralChild (const juce::Identifier& type);

void stripStructuralParts (juce::ValueTree& state);

// Makes the plugin's settings the stored tree's: the properties it carries
// are written, the non-structural ones it does not carry are removed (which
// reads as their default), and its non-structural children replace the
// plugin's. Properties named in `keep` are left exactly as they are -- how a
// part is played (legato handling, the sustain pedal, a singer's language)
// rather than how the instrument sounds.
void replaceSettings (te::Plugin&, const juce::ValueTree& stored,
                      const juce::Array<juce::Identifier>& keep = {});

} // namespace carve::plugins::presets
