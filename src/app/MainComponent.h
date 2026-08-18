#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GeneratorController.h"
#include "GeneratorWindow.h"
#include "MixerWindow.h"
#include "ClipPropertiesPanel.h"
#include "ExportWindow.h"
#include "PlaylistComponent.h"
#include "ShortcutHelpBar.h"
#include "PluginWindows.h"
#include "TransportBar.h"
#include "model/SongModel.h"
#include "sync/EditSync.h"

namespace carve::app
{

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ValueTree::Listener
{
public:
    explicit MainComponent (te::Engine& engineToUse);
    ~MainComponent() override;

    void resized() override;
    void parentHierarchyChanged() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void timerCallback() override;

    // The one place a new song arrives, whether from disk or the demo builder.
    void loadSong (model::Song newSong, juce::File sourceFile = {});
    void selectionChanged (const juce::String& generatorId, const juce::String& patternId);

    // Any edit at all reaches the song tree, so that is what "dirty" watches.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { markDirty(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { markDirty(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { markDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { markDirty(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void markDirty();
    void updateDocumentDisplay();

    void saveSong();
    void saveSongAs();
    void writeSongTo (const juce::File& file);
    void openSong();
    void openPluginManager();
    void openPatternEditor();

    // One window per selection, the tab picking which half is in front. The
    // pattern editor above and the playlist's label double-click land here.
    void openGeneratorWindow (GeneratorWindow::Tab);
    void retargetGeneratorWindow();
    void openMixer();
    void openExport();
    void previewNote (int pitch, int velocity);

    // Shortcuts that work from anywhere, appended after whatever the focused
    // view contributes.
    static std::vector<ShortcutHelpBar::Entry> globalShortcutHelp()
    {
        return { { "Space", "play/stop" },
                 { "Cmd+Z", "undo" },
                 { "Cmd+S", "save" },
                 { "Cmd+E", "export" } };
    }
    bool handleGlobalKey (const juce::KeyPress&);

    // Ruler seeks. The playlist's ruler names a song beat outright; the piano
    // roll's names a beat inside the edited pattern, which lands on that beat
    // of the pattern's earliest placement -- a pattern that is not placed has
    // no position in the song, so that seek goes nowhere.
    void seekToSongBeat (double beat);
    void seekToPatternBeat (double patternBeat);

    // Where in the selected pattern the transport is, or nothing while it is
    // outside every placement of it. Drives the roll's playhead.
    std::optional<double> patternBeatOfPlayhead (double songBeat) const;

    te::Engine& engine;
    std::unique_ptr<te::Edit> edit;
    juce::UndoManager undoManager;
    model::Song song { model::Song::create ("Untitled") };
    std::unique_ptr<sync::EditSync> editSync;

    // The file the song came from / goes back to on Cmd-S; empty until it has
    // been saved once.
    juce::File currentFile;
    bool hasUnsavedChanges = false;

    std::unique_ptr<TransportBar> transportBar;
    std::unique_ptr<GeneratorController> generatorController;
    PlaylistComponent playlist { undoManager };
    juce::Viewport playlistViewport;
    ClipPropertiesPanel clipProperties { undoManager };
    ShortcutHelpBar helpBar;

    juce::String selectedGeneratorId, selectedPatternId;

    std::shared_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<PluginScanWindow> pluginScanWindow;
    std::unique_ptr<GeneratorWindow> generatorWindow;
    std::unique_ptr<MixerWindow> mixerWindow;
    std::unique_ptr<ExportWindow> exportWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace carve::app
