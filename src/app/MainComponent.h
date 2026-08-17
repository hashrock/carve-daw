#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GeneratorPanel.h"
#include "PianoRollComponent.h"
#include "PlaylistComponent.h"
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

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    juce::UndoManager undoManager;
    model::Song song { model::Song::create ("Untitled") };
    std::unique_ptr<sync::EditSync> editSync;

    std::unique_ptr<TransportBar> transportBar;
    std::unique_ptr<GeneratorPanel> generatorPanel;
    PianoRollComponent pianoRoll { undoManager };
    PlaylistComponent playlist { undoManager };
    juce::Viewport pianoRollViewport, playlistViewport;

    std::shared_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace orionish::app
