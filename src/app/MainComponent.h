#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GeneratorController.h"
#include "GeneratorWindow.h"
#include "MixerWindow.h"
#include "ClipPropertiesPanel.h"
#include "MissingMediaWindow.h"
#include "ExportWindow.h"
#include "PlaylistComponent.h"
#include "ShortcutHelpBar.h"
#include "PluginWindows.h"
#include "SampleBrowser.h"
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

    // Asks about unsaved work before something is about to throw it away:
    // quitting, or replacing the song with a new, opened or demo one. Calls onResolved with true to go ahead and false to stay
    // put, and always asynchronously in spirit: the alert is, and so is the
    // save behind its first button.
    void confirmDiscardChanges (std::function<void (bool goAhead)> onResolved);

    // What the menu bar can ask for. The same handlers the logo menu, the
    // transport buttons and the shortcuts call, reached by name so the
    // application's MenuBarModel (Main.cpp) needs nothing else from here.
    enum class Action
    {
        newSong, openSong, openDemoSong, save, saveAs, exportWav,
        undo, redo,
        toggleBrowser, openMixer, openPatternEditor, pluginManager
    };

    void perform (Action);
    bool isBrowserShown() const  { return browser.isVisible(); }

private:
    void timerCallback() override;

    // The one place a new song arrives, whether from disk or the demo builder.
    void loadSong (model::Song newSong, juce::File sourceFile = {});

    // After a load: tries the song's own folder for any media it could not
    // find, and lists whatever is still missing for the user to relink.
    void checkForMissingMedia();
    void selectionChanged (const juce::String& generatorId, const juce::String& patternId);

    // Any edit at all reaches the song tree, so that is what "dirty" watches.
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { markDirty(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { markDirty(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { markDirty(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { markDirty(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}

    void markDirty();
    void updateDocumentDisplay();

    // onDone is told whether the song actually reached disk: a cancelled file
    // chooser and a failed write both answer false, so a caller that was going
    // to throw the document away afterwards can stop instead.
    void saveSong (std::function<void (bool saved)> onDone = {});
    void saveSongAs (std::function<void (bool saved)> onDone = {});
    bool writeSongTo (const juce::File& file);
    void newSong();
    void openSong();
    void openSongFile (const juce::File&);

    // Where the song choosers open, kept apart from the folder the sample
    // choosers remember so that browsing samples does not drag the song's
    // Open / Save along with it.
    juce::File songBrowseDirectory() const;
    void rememberSongDirectory (const juce::File& songFile);
    void openDemoSong();
    void openPluginManager();
    void openPatternEditor();

    // One window per selection, the tab picking which half is in front. The
    // pattern editor above and the playlist's label double-click land here.
    void openGeneratorWindow (GeneratorWindow::Tab);
    void retargetGeneratorWindow();
    void openMixer();
    void openExport();
    void previewNote (int pitch, int velocity);

    // The sample browser down the left of the playlist, shown or not; the
    // choice is remembered between sessions by the browser itself.
    void toggleBrowser();
    void setBrowserShown (bool shown);

    // Shortcuts that work from anywhere, appended after whatever the focused
    // view contributes.
    static std::vector<ShortcutHelpBar::Entry> globalShortcutHelp()
    {
        return { { "Space", "play/stop" },
                 { "Cmd+Z", "undo" },
                 { "Cmd+S", "save" },
                 { "Cmd+E", "export" },
                 { "Cmd+B", "browser" } };
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

    // Pattern mode (the transport bar's Pattern / Song switch): the Edit is
    // built around every generator's current pattern rather than the
    // playlist, and follows the pattern pickers while the mode is on.
    bool patternMode = false;
    void updateAudition();
    double auditionLengthBeats() const;

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
    SampleBrowser browser { engine };
    ShortcutHelpBar helpBar;

    // What the playlist last asked the help bar to show, so the bar can go
    // back to it when the pointer leaves the browser.
    std::vector<ShortcutHelpBar::Entry> playlistHelpEntries;

    juce::String selectedGeneratorId, selectedPatternId;

    std::shared_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<PluginScanWindow> pluginScanWindow;
    std::unique_ptr<GeneratorWindow> generatorWindow;
    std::unique_ptr<MixerWindow> mixerWindow;
    std::unique_ptr<ExportWindow> exportWindow;
    std::unique_ptr<MissingMediaWindow> missingMediaWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace carve::app
