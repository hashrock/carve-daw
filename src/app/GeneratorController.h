#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// The selection authority and model-edit hub the generator panel used to be,
// with the panel's UI gone: the playlist's row labels are the generator list
// now, and the generator window's header is the pattern switcher. What is
// left here is everything those views call back into -- which generator and
// pattern are selected, materialising slots, the add-generator menu, sample
// and drum-pad assignment, and pattern MIDI in/out.
class GeneratorController : private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    GeneratorController (te::Engine& engineToUse, model::Song songModel, juce::UndoManager& um);
    ~GeneratorController() override;

    // (generatorId, patternId) — patternId may be empty if none exists
    std::function<void (const juce::String&, const juce::String&)> onSelectionChanged;
    std::function<void()> onManagePlugins;

    // Fired after every refresh, which any change to the song tree triggers,
    // so the generator window can recolour its slot switcher and pads without
    // listening to the tree itself.
    std::function<void()> onPatternsChanged;

    void setSong (model::Song newSong);
    void selectGenerator (const juce::String& generatorId);

    // Selects one of the current generator's patterns. Call after
    // selectGenerator: selecting a generator picks its first pattern, so the
    // order matters.
    void selectPattern (const juce::String& patternId);

    juce::String getSelectedGeneratorId() const;
    juce::String getSelectedPatternId() const;
    std::optional<model::Generator> getSelectedGenerator() const;

    // A click on the slot switcher: materialises the slot's pattern if it has
    // none yet, selects it, and drops the previous pattern again if it was
    // only browsed past (see discardUntouchedPattern).
    void selectSlot (model::PatternSlot);

    // The slot right-click menu: duplicate, copy to another generator, MIDI in
    // and out. screenArea is where to hang the menu (the switcher cell).
    void showSlotMenu (model::PatternSlot, juce::Rectangle<int> screenArea);

    // The add-generator menu (synth / sampler / drum kit / plugin / audio),
    // hung off screenArea -- the playlist's "+ Generator" button.
    void showAddGeneratorMenu (juce::Rectangle<int> screenArea);

    // The plain sampler's "one sample across the whole keyboard" chooser, for
    // the generator window's Load Sample button. Ignored unless the selected
    // generator is a plain sampler -- a drum kit's sounds are its pads.
    void chooseSampleForSelected();

    // The drum pads in the generator window's Inst tab. A click on an empty
    // pad opens the sample chooser; on a filled one, a replace/clear menu hung
    // off screenArea. Drops fill pads from startPad on.
    void padClicked (int pad, juce::Rectangle<int> screenArea);
    void padFilesDropped (int startPad, const juce::StringArray& files);

    // Whether the engine can read any of these as audio, for the pad grid's
    // drag-over test.
    bool canImportAudioFiles (const juce::StringArray& files) const;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

    void refresh();
    void fireSelectionChanged();
    double newPatternLengthBeats() const;
    model::Generator addGenerator (const juce::String& name, const juce::String& type,
                                   const juce::PluginDescription* description);

    // The sampler side. A sample is picked with a file chooser (or a drop);
    // there is no key-zone editor, so one sample covers the whole keyboard and
    // replaces whatever the generator had.
    void launchSampleChooser (std::function<void (const juce::File&)> onChosen);
    void addSamplerGenerator (const juce::File& sample);
    void assignSample (model::Generator, const juce::File& sample);

    // The drum-kit side. A kit is the same sampler with one sound per pad, so
    // all of this is addSound/removeSound with the key range pinned to the
    // pad's note -- see DrumPadGrid.h for the note layout.
    void assignPadSample (model::Generator, int pad, const juce::File& sample);
    void clearPad (model::Generator, int pad);

    // The model half of the above, without a transaction or a refresh, so a
    // multi-file drop lands as one undoable gesture.
    void setPadSound (model::Generator&, int pad, const juce::File& sample);

    void ensureValidSelection();
    void discardUntouchedPattern (const juce::String& patternId,
                                  const juce::String& generatorId);

    // The menu behind showSlotMenu works on (generatorId, slot) rather than a
    // Pattern, because the menu and the file choosers behind it are
    // asynchronous and the song may be replaced while one is open.
    void duplicatePattern (const juce::String& generatorId, model::PatternSlot,
                           const juce::String& destinationGeneratorId);
    void exportPatternToMidi (const juce::String& generatorId, model::PatternSlot);
    void importMidiIntoSlot (const juce::String& generatorId, model::PatternSlot);

    te::Engine& engine;
    model::Song song;
    juce::UndoManager& undoManager;

    // Have to outlive launchAsync, so they can't be locals. Two of them,
    // because a sample chooser and a MIDI chooser are never open at once but
    // one would otherwise cancel the other's callback on being replaced.
    std::unique_ptr<juce::FileChooser> sampleChooser, midiChooser;

    juce::String selectedGeneratorId;
    juce::String selectedPatternId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratorController)
};

} // namespace carve::app
