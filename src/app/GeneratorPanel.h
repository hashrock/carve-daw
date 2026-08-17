#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace orionish::app
{

// Left-hand panel: the list of Generators and, for the selected one, its
// Patterns. This is the entry point of the Orion workflow: pick a Generator,
// pick/create one of its Patterns, then edit it in the piano roll.
class GeneratorPanel : public juce::Component,
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

    void setSong (model::Song newSong);
    void selectGenerator (const juce::String& generatorId);

    juce::String getSelectedGeneratorId() const;
    juce::String getSelectedPatternId() const;

    void resized() override;

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int) override;

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

    void refresh();
    void rebuildPatternBox();
    void fireSelectionChanged();
    void showAddGeneratorMenu();
    void addGenerator (const juce::String& name, const juce::String& type,
                       const juce::PluginDescription* description);

    te::Engine& engine;
    model::Song song;
    juce::UndoManager& undoManager;

    juce::ListBox generatorList;
    juce::TextButton addGeneratorButton { "+ Generator" };
    juce::TextButton instrumentButton { "Instrument UI" };
    juce::Label patternHeader;
    juce::ComboBox patternBox;
    juce::TextButton addPatternButton { "+ Pattern" };

    juce::String selectedPatternId;
    bool isRefreshing = false;
};

} // namespace orionish::app
