#pragma once

#include <array>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// The A1..D9 pattern slot grid, drawn as one click-per-slot pad grid the way
// Orion and FL show their pattern selectors. It holds no model state of its
// own: GeneratorPanel pushes a state per slot on every refresh, so a slot that
// is not in the tree yet is simply drawn as "unused" and can still be clicked.
class PatternSlotGrid final : public juce::Component
{
public:
    enum class SlotState
    {
        unused,     // no pattern in the tree for this slot
        empty,      // pattern exists but has no notes
        hasNotes
    };

    PatternSlotGrid();

    std::function<void (model::PatternSlot)> onSlotClicked;

    void setSlotState (const model::PatternSlot&, SlotState);
    void setSelectedSlot (std::optional<model::PatternSlot>);
    void clearSlots();

    // Height needed to show all four banks; the pads take whatever width there is.
    static int getPreferredHeight();

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::Rectangle<int> getSlotBounds (const model::PatternSlot&) const;
    std::optional<model::PatternSlot> getSlotAt (juce::Point<int>) const;

    std::array<SlotState, (size_t) model::PatternSlot::numSlots> slotStates;
    std::optional<model::PatternSlot> selectedSlot;
};

// Left-hand panel: the list of Generators and, for the selected one, its
// Patterns. This is the entry point of the Orion workflow: pick a Generator,
// pick/create one of its Patterns, then edit it in the piano roll.
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

    void setSong (model::Song newSong);
    void selectGenerator (const juce::String& generatorId);

    // Selects one of the current generator's patterns. Call after
    // selectGenerator: selecting a generator picks its first pattern, so the
    // order matters.
    void selectPattern (const juce::String& patternId);

    juce::String getSelectedGeneratorId() const;
    juce::String getSelectedPatternId() const;

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
    void rebuildSlotGrid();
    void fireSelectionChanged();
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

    std::optional<model::Generator> getSelectedGenerator() const;
    void ensureValidPatternSelection();
    void slotClicked (model::PatternSlot);
    void discardUntouchedPattern (const juce::String& patternId,
                                  const juce::String& generatorId);

    te::Engine& engine;
    model::Song song;
    juce::UndoManager& undoManager;

    juce::ListBox generatorList;
    juce::TextButton addGeneratorButton { "+ Generator" };
    juce::TextButton instrumentButton { "Instrument UI" };
    juce::TextButton editPatternButton { "Edit Pattern" };
    juce::Label patternHeader;
    PatternSlotGrid slotGrid;
    juce::ComboBox patternBox;   // only the patterns outside the slot grid

    // Has to outlive launchAsync, so it can't be a local.
    std::unique_ptr<juce::FileChooser> sampleChooser;

    juce::String selectedPatternId;
    bool isRefreshing = false;
};

} // namespace carve::app
