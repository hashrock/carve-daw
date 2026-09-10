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
// volume fader, pan, mute and solo. A master strip is pinned to the right of
// them, carrying the Edit's master fader, its output meter and its insert
// chain.
//
// Generator edits go through the song model, not straight at the tracktion
// track, so they are undoable and saved in the .carve file like every other
// edit; EditSync pushes them on to the track. Only the meters read the engine
// directly.
//
// The master strip is the exception, and knowingly so: the song model has no
// node for a master fader or a master chain, so those edits go straight at the
// Edit. They are neither undoable nor saved. Giving them a home in the model is
// a change to src/model plus EditSync -- see the strip's own comment.
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

    // A strip's name label was double-clicked: open that generator's window,
    // the same way a double click on the playlist's row label does. The mixer
    // is where you look at a generator's level, so it wants the same way in.
    std::function<void (const juce::String& generatorId)> onOpenGenerator;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    class ChannelStrip;
    class MasterStrip;
    class ReturnStrip;

    // Wider than a bare fader strip needs to be: the insert slots have to
    // show enough of an effect's name to tell two of them apart.
    static constexpr int stripWidth = 116;
    static constexpr int minHeight = 300;

    // The gap that sets the master apart from the generators it sums.
    static constexpr int masterGap = 68;   // wide enough for the add-return button

    // An open editor window, keyed by effect id. The Plugin::Ptr is a strong
    // reference on purpose: removing the effect drops the track's own
    // reference, and an external plugin's editor component would then be
    // holding a freed AudioPluginInstance. Holding on keeps the plugin alive
    // until the window has gone -- which is why `window` is declared last, so
    // it is destroyed first.
    struct OpenEffectWindow
    {
        // Empty for a master insert: it belongs to no generator.
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
    // that track stamped with the effect's id. An empty generatorId means the
    // master list, where the id is the plugin's own EditItemID.
    te::Plugin* findEffectPlugin (const juce::String& generatorId,
                                  const juce::String& effectId) const;

    // The plugin behind a slot, wherever its chain lives. Everything that has
    // to match a slot to a live plugin goes through this rather than picking
    // one of the two lookups below and getting returns wrong.
    te::Plugin* findEffectPluginForOwner (const juce::String& ownerId, const juce::String& effectId);

    void openEffectEditor (const juce::String& generatorId, const juce::String& effectId);
    te::Plugin* findReturnEffectPlugin (const juce::String& returnId, const juce::String& effectId);
    void closeEffectWindow (const juce::String& effectId);

    // Drops any window whose plugin the model or EditSync has replaced or
    // taken away behind our back (an undo, a resync, another view).
    void closeStaleEffectWindows();

    te::Edit& edit;
    juce::UndoManager& undoManager;
    model::Song song { model::Song::create ("Untitled") };

    std::vector<std::unique_ptr<ChannelStrip>> strips;
    std::unique_ptr<MasterStrip> masterStrip;
    std::vector<std::unique_ptr<ReturnStrip>> returnStrips;
    juce::TextButton addReturnButton { "+ Return" };
    std::map<juce::String, OpenEffectWindow> effectWindows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MixerComponent)
};

} // namespace carve::app
