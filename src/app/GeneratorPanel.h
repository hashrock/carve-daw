#pragma once

#include <array>

#include <tracktion_engine/tracktion_engine.h>

#include "DrumPadGrid.h"
#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// Left-hand panel: the list of Generators and, for the selected one, its
// Patterns. This is the entry point of the Orion workflow: pick a Generator,
// pick/create one of its Patterns, then edit it in the piano roll.
//
// The A1..D9 slot switcher itself lives in the generator window's header
// (GeneratorWindow.h); the panel stays the authority over what is selected,
// which is why selectSlot and showSlotMenu are public API here.
class GeneratorPanel : public juce::Component,
                       public juce::FileDragAndDropTarget,
                       private juce::ListBoxModel,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater
{
public:
    GeneratorPanel (te::Engine& engineToUse, model::Song songModel, juce::UndoManager& um);
    ~GeneratorPanel() override;

    // (generatorId, patternId) — patternId may be empty if none exists
    std::function<void (const juce::String&, const juce::String&)> onSelectionChanged;
    std::function<void (const juce::String&)> onOpenPluginEditor;   // generatorId
    std::function<void()> onManagePlugins;
    std::function<void()> onOpenPatternEditor;

    // Fired after every refresh, which any change to the song tree triggers,
    // so the generator window's slot switcher can recolour without listening
    // to the tree itself.
    std::function<void()> onPatternsChanged;

    void setSong (model::Song newSong);
    void selectGenerator (const juce::String& generatorId);

    // Selects one of the current generator's patterns. Call after
    // selectGenerator: selecting a generator picks its first pattern, so the
    // order matters.
    void selectPattern (const juce::String& patternId);

    juce::String getSelectedGeneratorId() const;
    juce::String getSelectedPatternId() const;

    // A click on the slot switcher: materialises the slot's pattern if it has
    // none yet, selects it, and drops the previous pattern again if it was
    // only browsed past (see discardUntouchedPattern).
    void selectSlot (model::PatternSlot);

    // The slot right-click menu: duplicate, copy to another generator, MIDI in
    // and out. screenArea is where to hang the menu (the switcher cell).
    void showSlotMenu (model::PatternSlot, juce::Rectangle<int> screenArea);

    void resized() override;

    // Dropping an audio file makes a sampler generator out of it, or swaps the
    // sample of the sampler it lands on. Anything the engine can't read is
    // left alone, so a drag meant for someone else passes through.
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

    void refresh();
    void rebuildPatternBox();
    void rebuildPatternHeader();
    void rebuildPadGrid();
    void fireSelectionChanged();
    double newPatternLengthBeats() const;
    void showAddGeneratorMenu();
    model::Generator addGenerator (const juce::String& name, const juce::String& type,
                                   const juce::PluginDescription* description);

    // The sampler side of the panel. A sample is picked with a file chooser
    // (or a drop); there is no key-zone editor, so one sample covers the whole
    // keyboard and replaces whatever the generator had.
    void launchSampleChooser (std::function<void (const juce::File&)> onChosen);
    void addSamplerGenerator (const juce::File& sample);
    void assignSample (model::Generator, const juce::File& sample);
    std::optional<model::Generator> getGeneratorAt (juce::Point<int>) const;

    // The drum-kit side of the panel. A kit is the same sampler with one sound
    // per pad, so all of this is addSound/removeSound with the key range
    // pinned to the pad's note -- see DrumPadGrid.h for the note layout.
    void padClicked (int pad);
    void padFilesDropped (int startPad, const juce::StringArray& files);
    void assignPadSample (model::Generator, int pad, const juce::File& sample);
    void clearPad (model::Generator, int pad);
    void assignToFirstFreePad (model::Generator, const juce::File& sample);

    // The model half of the above, without a transaction or a refresh, so a
    // multi-file drop lands as one undoable gesture.
    void setPadSound (model::Generator&, int pad, const juce::File& sample);

    std::optional<model::Generator> getSelectedGenerator() const;
    void ensureValidPatternSelection();
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

    juce::ListBox generatorList;
    juce::TextButton addGeneratorButton { "+ Generator" };
    juce::TextButton instrumentButton { "Instrument UI" };
    juce::TextButton editPatternButton { "Edit Pattern" };
    juce::Label patternHeader;
    DrumPadGrid padGrid;         // visible only while a drum kit is selected
    juce::ComboBox patternBox;   // only the patterns outside the slot switcher

    // Have to outlive launchAsync, so they can't be locals. Two of them,
    // because a sample chooser and a MIDI chooser are never open at once but
    // one would otherwise cancel the other's callback on being replaced.
    std::unique_ptr<juce::FileChooser> sampleChooser, midiChooser;

    juce::String selectedPatternId;
    bool isRefreshing = false;
};

} // namespace carve::app
