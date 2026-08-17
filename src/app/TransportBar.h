#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace orionish::app
{

// Play/stop, loop toggle, BPM, position readout, undo/redo and save/open.
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um);

    std::function<void()> onSave, onOpen, onOpenMixer;

    void setSong (model::Song newSong);
    void togglePlay();

    void resized() override;

private:
    void timerCallback() override;
    double getSongLengthBeats() const;

    te::Edit& edit;
    model::Song song;
    juce::UndoManager& undoManager;

    juce::TextButton playButton { "Play" }, stopButton { "Stop" },
                     mixerButton { "Mixer" },
                     undoButton { "Undo" }, redoButton { "Redo" },
                     saveButton { "Save" }, openButton { "Open" };
    juce::ToggleButton loopButton { "Loop" };
    juce::Label bpmLabel, positionLabel;
};

} // namespace orionish::app
