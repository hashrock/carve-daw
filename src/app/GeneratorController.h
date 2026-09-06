#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// The selection authority and model-edit hub the generator panel used to be,
// with the panel's UI gone: the playlist's row labels are the generator list
// now, and the generator window's header is the pattern picker. What is left
// here is everything those views call back into -- which generator and
// pattern are selected, making and cloning and deleting patterns, the
// add-generator menu, sample and drum-pad assignment, and pattern MIDI in/out.
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
    // so the generator window can restock its pattern picker and recolour its
    // pads without listening to the tree itself.
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

    // The picker's New button: a fresh, empty, automatically named pattern on
    // the selected generator, selected on the spot. Nothing to type, so a new
    // pattern costs one click.
    void createPattern();

    // The picker's Clone button (and cmd-D): a copy of the selected pattern,
    // named after it, selected on the spot. The same one click -- cloning is
    // the middle of "make one, clone it, vary it", and a dialog there is what
    // breaks the take.
    void clonePattern();

    // The picker's menu button: rename, delete, copy to another generator,
    // MIDI in and out. screenArea is where to hang the menu.
    void showPatternMenu (juce::Rectangle<int> screenArea);

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

    // The chooser reopens where the last sample was picked from, so filling a
    // kit pad by pad does not walk back down to the sample library each time.
    juce::File getSampleBrowseDirectory() const;
    void rememberSampleDirectory (const juce::File& sample);
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

    // Everything behind showPatternMenu works on (generatorId, patternId)
    // rather than on a Pattern, because the menu and the file choosers behind
    // it are asynchronous and the song may be replaced while one is open.
    void duplicatePattern (const juce::String& generatorId, const juce::String& patternId,
                           const juce::String& destinationGeneratorId);
    void renamePattern (const juce::String& generatorId, const juce::String& patternId);
    void deletePattern (const juce::String& generatorId, const juce::String& patternId);
    void exportPatternToMidi (const juce::String& generatorId, const juce::String& patternId);
    void importMidiIntoPattern (const juce::String& generatorId, const juce::String& patternId);

    te::Engine& engine;
    model::Song song;
    juce::UndoManager& undoManager;

    // Have to outlive launchAsync, so they can't be locals. Two of them,
    // because a sample chooser and a MIDI chooser are never open at once but
    // one would otherwise cancel the other's callback on being replaced.
    std::unique_ptr<juce::FileChooser> sampleChooser, midiChooser;

    // Same reason: the rename box is asynchronous too.
    std::unique_ptr<juce::AlertWindow> renameWindow;

    juce::String selectedGeneratorId;
    juce::String selectedPatternId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratorController)
};

} // namespace carve::app
