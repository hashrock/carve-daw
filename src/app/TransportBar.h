#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "IconButton.h"
#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

//==============================================================================
// Play/stop, loop toggle, BPM, position readout, document name, undo/redo and
// save/open.
class TransportBar : public juce::Component,
                     private juce::Timer
{
public:
    TransportBar (te::Edit& editToControl, model::Song songModel, juce::UndoManager& um);

    std::function<void()> onSave, onOpen, onOpenMixer, onExport;

    void setSong (model::Song newSong);

    // The window title is the authoritative place for this, but the title bar
    // is easy to lose behind the floating editors, so the transport bar shows
    // it too. Owned by MainComponent, which tracks the document.
    void setDocumentState (const juce::String& documentName, bool hasUnsavedChanges);

    void togglePlay();

    void resized() override;

private:
    void timerCallback() override;
    double getSongLengthBeats() const;

    te::Edit& edit;
    model::Song song;
    juce::UndoManager& undoManager;

    IconButton playButton   { "Play",   Icon::play },
               stopButton   { "Stop",   Icon::stop },
               loopButton   { "Loop",   Icon::loop },     // a lit toggle, not a checkbox
               mixerButton  { "Mixer",  Icon::mixer },
               exportButton { "Export", Icon::exportFile },
               undoButton   { "Undo",   Icon::undo },
               redoButton   { "Redo",   Icon::redo },
               saveButton   { "Save",   Icon::save },
               openButton   { "Open",   Icon::open };
    juce::Label bpmLabel, positionLabel, documentLabel;
};

} // namespace carve::app
