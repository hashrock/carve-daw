#pragma once

#include <map>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "EffectParameterWindow.h"
#include "EffectSlotList.h"
#include "PluginWindows.h"
#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// One channel strip per Generator: insert effect slots, post-fader level meter,
// volume fader, pan, mute and solo.
//
// Edits go through the song model, not straight at the tracktion track, so they
// are undoable and saved in the .carve file like every other edit; EditSync
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

    // Wider than a bare fader strip needs to be: the insert slots have to
    // show enough of an effect's name to tell two of them apart.
    static constexpr int stripWidth = 116;
    static constexpr int minHeight = 300;

    // An open editor window, keyed by effect id. The Plugin::Ptr is a strong
    // reference on purpose: removing the effect drops the track's own
    // reference, and an external plugin's editor component would then be
    // holding a freed AudioPluginInstance. Holding on keeps the plugin alive
    // until the window has gone -- which is why `window` is declared last, so
    // it is destroyed first.
    struct OpenEffectWindow
    {
        juce::String generatorId;
        te::Plugin::Ptr plugin;
        std::unique_ptr<juce::DocumentWindow> window;
    };

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
    void effectListChanged (const juce::ValueTree& parent);

    // generator order == track order (EditSync invariant), then the plugin on
    // that track stamped with the effect's id.
    te::Plugin* findEffectPlugin (const juce::String& generatorId,
                                  const juce::String& effectId) const;

    void openEffectEditor (const juce::String& generatorId, const model::Effect&);
    void closeEffectWindow (const juce::String& effectId);

    // Drops any window whose plugin the model or EditSync has replaced or
    // taken away behind our back (an undo, a resync, another view).
    void closeStaleEffectWindows();

    te::Edit& edit;
    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("Untitled") };

    std::vector<std::unique_ptr<ChannelStrip>> strips;
    std::map<juce::String, OpenEffectWindow> effectWindows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

} // namespace carve::app
