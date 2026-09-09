#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// Watches the MIDI on its way into an instrument and keeps, per note, whether
// the note is held and how many times it has been struck. Nothing else: the
// audio goes through untouched and no state is saved.
//
// It exists so a drum kit's pads can light while they sound. The engine
// offers no other view of that: SamplerPlugin keeps its voices private, and
// the MIDI a track plays is assembled on the audio thread from clips and
// guide notes, so the only place both kinds of hit can be seen is in the
// plugin chain, just before the sampler. EditSync puts one of these there on
// every drum kit's track (see ensureNoteMonitor) and leaves it alone across
// resyncs, the way it leaves the aux sends -- it carries no effect id and is
// not an instrument type, so the chain sync passes it by.
//
// Read from the message thread by polling. A hit count rather than a level:
// a drum hit is over between two timer ticks, and a counter that moved says
// there was one where a flag would already have cleared.
class NoteMonitorPlugin : public te::Plugin
{
public:
    explicit NoteMonitorPlugin (te::PluginCreationInfo);
    ~NoteMonitorPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("Note Monitor"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "Note Monitor"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "Mon"; }
    juce::String getSelectableDescription() override  { return TRANS ("Note Monitor"); }

    // Whatever comes in goes out: the sampler after it clears the buffer
    // itself, and anything before it is passed on as found.
    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return numInputChannels; }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return true; }
    bool producesAudioWhenNoAudioInput() override     { return false; }

    // Never something to put in a clip or a rack: it is EditSync's, and
    // means nothing anywhere but in front of a kit.
    bool canBeAddedToClip() override                  { return false; }
    bool canBeAddedToRack() override                  { return false; }
    bool canBeAddedToMaster() override                { return false; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    struct NoteActivity
    {
        uint32_t hits = 0;    // note-ons seen so far; wraps, compare for change only
        bool held = false;    // a note-on with no note-off yet
    };

    // Safe from any thread; nothing is locked.
    NoteActivity getActivity (int midiNote) const;

private:
    void releaseAll();

    std::array<std::atomic<uint32_t>, 128> hitCounts;
    std::array<std::atomic<bool>, 128> heldNotes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteMonitorPlugin)
};

} // namespace carve::plugins
