#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GeneratorPanel.h"
#include "MixerWindow.h"
#include "PianoRollWindow.h"
#include "PlaylistComponent.h"
#include "PluginWindows.h"
#include "TransportBar.h"
#include "model/SongModel.h"
#include "sync/EditSync.h"

namespace orionish::app
{

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    explicit MainComponent (te::Engine& engineToUse);
    ~MainComponent() override;

    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;
    void loadSong (model::Song newSong);
    void selectionChanged (const juce::String& generatorId, const juce::String& patternId);
    void saveSong();
    void openSong();
    void openPluginEditor (const juce::String& generatorId);
    void openPluginManager();
    void openPatternEditor();
    void openMixer();
    bool handleGlobalKey (const juce::KeyPress&);

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    juce::UndoManager undoManager;
    model::Song song { model::Song::create ("Untitled") };
    std::unique_ptr<sync::EditSync> editSync;

    std::unique_ptr<TransportBar> transportBar;
    std::unique_ptr<GeneratorPanel> generatorPanel;
    PlaylistComponent playlist { undoManager };
    juce::Viewport playlistViewport;

    juce::String selectedGeneratorId, selectedPatternId;

    std::shared_ptr<juce::FileChooser> fileChooser;
    std::map<juce::String, std::unique_ptr<PluginEditorWindow>> pluginEditorWindows;
    std::unique_ptr<PluginScanWindow> pluginScanWindow;
    std::unique_ptr<PianoRollWindow> pianoRollWindow;
    std::unique_ptr<MixerWindow> mixerWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace orionish::app
