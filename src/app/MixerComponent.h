#pragma once

#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace orionish::app
{

// One channel strip per Generator: post-fader level meter, volume fader, pan,
// mute and solo.
//
// Edits go through the song model, not straight at the tracktion track, so they
// are undoable and saved in the .orion file like every other edit; EditSync
// pushes them on to the track. Only the meters read the engine directly.
//
// Sized to its content — put it in a Viewport.
class MixerComponent : public juce::Component,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater,
                       private juce::Timer
{
public:
    MixerComponent (te::Edit&, juce::UndoManager&);
    ~MixerComponent() override;

    void setSong (model::Song newSong);

    // Fired when adding or removing a generator has changed how wide the
    // strips need to be, so the containing window can re-fit itself.
    std::function<void()> onContentWidthChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ChannelStrip;

    static constexpr int stripWidth = 92;
    static constexpr int minHeight = 300;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override;
    void valueTreeParentChanged (juce::ValueTree&) override  {}

    void handleAsyncUpdate() override  { rebuildStrips(); }
    void timerCallback() override;

    // Rebuilding destroys the strips, so it must not happen while a fader is
    // being dragged: only generator list changes come through here, never
    // mixer property changes.
    void rebuildStrips();
    void generatorListChanged (const juce::ValueTree& parent);

    te::Edit& edit;
    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("Untitled") };

    std::vector<std::unique_ptr<ChannelStrip>> strips;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

} // namespace orionish::app
