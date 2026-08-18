#pragma once

#include <tracktion_engine/tracktion_engine.h>

#include "GeneratorPanel.h"
#include "MixerWindow.h"
#include "PianoRollWindow.h"
#include "ClipPropertiesPanel.h"
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
    void openPluginEditor (const juce::String& generatorId);
    void openPluginManager();
    void openPatternEditor();
    void openMixer();
    void previewNote (int pitch, int velocity);

    // Shortcuts that work from anywhere, appended after whatever the focused
    // view contributes.
    static std::vector<ShortcutHelpBar::Entry> globalShortcutHelp()
    {
        return { { "Space", "play/stop" },
                 { "Cmd+Z", "undo" },
                 { "Cmd+S", "save" } };
    }
    bool handleGlobalKey (const juce::KeyPress&);

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
    std::unique_ptr<GeneratorPanel> generatorPanel;
    PlaylistComponent playlist { undoManager };
    juce::Viewport playlistViewport;
    ClipPropertiesPanel clipProperties { undoManager };
    ShortcutHelpBar helpBar;

    juce::String selectedGeneratorId, selectedPatternId;

    std::shared_ptr<juce::FileChooser> fileChooser;
    std::map<juce::String, std::unique_ptr<PluginEditorWindow>> pluginEditorWindows;
    std::unique_ptr<PluginScanWindow> pluginScanWindow;
    std::unique_ptr<PianoRollWindow> pianoRollWindow;
    std::unique_ptr<MixerWindow> mixerWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace carve::app
