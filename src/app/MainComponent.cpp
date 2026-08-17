#include "MainComponent.h"

#include "EngineSetup.h"
#include "model/DemoSong.h"

namespace orionish::app
{

MainComponent::MainComponent (te::Engine& engineToUse)
    : engine (engineToUse)
{
    edit = te::Edit::createSingleTrackEdit (engine);

    transportBar = std::make_unique<TransportBar> (*edit, song, undoManager);
    transportBar->onSave = [this] { saveSong(); };
    transportBar->onOpen = [this] { openSong(); };

    generatorPanel = std::make_unique<GeneratorPanel> (engine, song, undoManager);
    generatorPanel->onSelectionChanged = [this] (const juce::String& generatorId,
                                                 const juce::String& patternId)
    {
        selectionChanged (generatorId, patternId);
    };
    generatorPanel->onOpenPluginEditor = [this] (const juce::String& generatorId)
    {
        openPluginEditor (generatorId);
    };
    generatorPanel->onManagePlugins = [this] { openPluginManager(); };
    generatorPanel->onOpenPatternEditor = [this] { openPatternEditor(); };

    transportBar->onOpenMixer = [this] { openMixer(); };

    playlist.onSelectGenerator = [this] (const juce::String& generatorId)
    {
        generatorPanel->selectGenerator (generatorId);
    };

    playlistViewport.setViewedComponent (&playlist, false);

    addAndMakeVisible (*transportBar);
    addAndMakeVisible (*generatorPanel);
    addAndMakeVisible (playlistViewport);

    loadSong (model::buildDemoSong());

    setWantsKeyboardFocus (true);
    startTimerHz (30);
    setSize (1100, 620);
}

MainComponent::~MainComponent()
{
    song.state.removeListener (this);
    edit->getTransport().stop (false, false);
}

void MainComponent::loadSong (model::Song newSong, juce::File sourceFile)
{
    edit->getTransport().stop (false, false);
    pluginEditorWindows.clear();
    pianoRollWindow.reset();
    mixerWindow.reset();
    undoManager.clearUndoHistory();

    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);

    editSync = std::make_unique<sync::EditSync> (song, *edit);

    transportBar->setSong (song);
    playlist.setSong (song);
    generatorPanel->setSong (song);   // fires onSelectionChanged -> updates piano roll

    // Last, because handing the song to the views can write to the tree and a
    // song that has just been loaded has nothing unsaved by definition.
    currentFile = std::move (sourceFile);
    hasUnsavedChanges = false;
    updateDocumentDisplay();
}

void MainComponent::markDirty()
{
    if (hasUnsavedChanges)
        return;   // a fader drag arrives here on every mouse move

    hasUnsavedChanges = true;
    updateDocumentDisplay();
}

void MainComponent::updateDocumentDisplay()
{
    const auto documentName = currentFile != juce::File() ? currentFile.getFileName()
                                                          : juce::String ("Untitled");

    // The window does not exist yet while the constructor loads the demo song;
    // parentHierarchyChanged() comes back here once it does.
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName (documentName + (hasUnsavedChanges ? " *" : "")
                             + " - " + orionish::applicationName);

    if (transportBar != nullptr)
        transportBar->setDocumentState (documentName, hasUnsavedChanges);
}

void MainComponent::parentHierarchyChanged()
{
    updateDocumentDisplay();
}

void MainComponent::selectionChanged (const juce::String& generatorId, const juce::String& patternId)
{
    selectedGeneratorId = generatorId;
    selectedPatternId = patternId;
    playlist.setSelection (generatorId, patternId);

    if (pianoRollWindow != nullptr)
        openPatternEditor();   // retarget the open editor to the new selection
}

void MainComponent::openPatternEditor()
{
    std::optional<model::Pattern> pattern;
    juce::String title;

    if (auto generator = song.findGenerator (selectedGeneratorId))
        if ((pattern = generator->findPattern (selectedPatternId)))
            title = generator->getName() + " / " + pattern->getName();

    if (pianoRollWindow == nullptr)
    {
        auto onClose = [safe = juce::Component::SafePointer (this)]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->pianoRollWindow.reset();
            });
        };
        auto keyHandler = [safe = juce::Component::SafePointer (this)] (const juce::KeyPress& key)
        {
            return safe != nullptr && safe->handleGlobalKey (key);
        };
        pianoRollWindow = std::make_unique<PianoRollWindow> (undoManager, std::move (onClose),
                                                             std::move (keyHandler));
    }

    pianoRollWindow->setPattern (std::move (pattern), title);
    pianoRollWindow->toFront (true);
}

void MainComponent::openMixer()
{
    if (mixerWindow != nullptr)
    {
        mixerWindow->toFront (true);
        return;
    }

    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->mixerWindow.reset();
        });
    };
    auto keyHandler = [safe = juce::Component::SafePointer (this)] (const juce::KeyPress& key)
    {
        return safe != nullptr && safe->handleGlobalKey (key);
    };
    mixerWindow = std::make_unique<MixerWindow> (*edit, undoManager,
                                                 std::move (onClose), std::move (keyHandler));
    mixerWindow->setSong (song);
}

void MainComponent::openPluginEditor (const juce::String& generatorId)
{
    if (auto existing = pluginEditorWindows.find (generatorId);
        existing != pluginEditorWindows.end())
    {
        existing->second->toFront (true);
        return;
    }

    // generator order == track order (EditSync invariant)
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        if (generators[(size_t) i].getId() != generatorId)
            continue;

        if (auto external = tracks[i]->pluginList.findFirstPluginOfType<te::ExternalPlugin>())
        {
            if (auto instance = external->getAudioPluginInstance())
            {
                // destruction is deferred: the close callback runs inside the
                // window's own member function
                auto onClose = [safe = juce::Component::SafePointer (this), generatorId]
                {
                    juce::MessageManager::callAsync ([safe, generatorId]
                    {
                        if (safe != nullptr)
                            safe->pluginEditorWindows.erase (generatorId);
                    });
                };
                pluginEditorWindows[generatorId] =
                    std::make_unique<PluginEditorWindow> (*instance, std::move (onClose));
            }
        }
        return;   // internal 4OSC has no editor window yet
    }
}

void MainComponent::openPluginManager()
{
    if (pluginScanWindow != nullptr)
    {
        pluginScanWindow->toFront (true);
        return;
    }
    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->pluginScanWindow.reset();
        });
    };
    pluginScanWindow = std::make_unique<PluginScanWindow> (engine, std::move (onClose));
}

void MainComponent::saveSong()
{
    if (currentFile != juce::File())
    {
        writeSongTo (currentFile);
        return;
    }

    saveSongAs();
}

void MainComponent::saveSongAs()
{
    fileChooser = std::make_shared<juce::FileChooser> ("Save song", currentFile, "*.orion");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        writeSongTo (file.withFileExtension ("orion"));
    });
}

void MainComponent::writeSongTo (const juce::File& file)
{
    // An external plugin's state only exists inside the live instance until
    // this copies it into the model, so every save has to come through here.
    if (editSync != nullptr)
        editSync->captureLivePluginState();

    if (! song.saveToFile (file))
    {
        juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                     "Save failed",
                                                     "Could not write " + file.getFullPathName());
        return;
    }

    currentFile = file;
    hasUnsavedChanges = false;   // captureLivePluginState above will have set it
    updateDocumentDisplay();
}

void MainComponent::openSong()
{
    fileChooser = std::make_shared<juce::FileChooser> ("Open song", juce::File(), "*.orion");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        if (auto loaded = model::Song::loadFromFile (file))
            loadSong (*loaded, file);
    });
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    return handleGlobalKey (key);
}

bool MainComponent::handleGlobalKey (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        transportBar->togglePlay();
        return true;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        undoManager.undo();
        return true;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        undoManager.redo();
        return true;
    }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0))
    {
        saveSong();
        return true;
    }
    if (key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        saveSongAs();
        return true;
    }
    return false;
}

void MainComponent::timerCallback()
{
    const auto position = edit->getTransport().getPosition();
    playlist.setPlayheadBeats (edit->tempoSequence.toBeats (position).inBeats());
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar->setBounds (area.removeFromTop (44));
    generatorPanel->setBounds (area.removeFromLeft (220));
    playlistViewport.setBounds (area);
}

} // namespace orionish::app
