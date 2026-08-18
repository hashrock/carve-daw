#include "MainComponent.h"

#include "sync/EngineIds.h"

#include "EngineSetup.h"
#include "model/DemoSong.h"

namespace carve::app
{

MainComponent::MainComponent (te::Engine& engineToUse)
    : engine (engineToUse)
{
    edit = te::Edit::createSingleTrackEdit (engine);

    transportBar = std::make_unique<TransportBar> (*edit, song, undoManager);
    transportBar->onSave = [this] { saveSong(); };
    transportBar->onOpen = [this] { openSong(); };

    generatorController = std::make_unique<GeneratorController> (engine, song, undoManager);
    generatorController->onSelectionChanged = [this] (const juce::String& generatorId,
                                             const juce::String& patternId)
    {
        selectionChanged (generatorId, patternId);
    };
    generatorController->onManagePlugins = [this] { openPluginManager(); };

    // Slot states change without the selection moving (a note lands in a
    // pattern); the controller's refresh already runs then, so it recolours
    // the window's switcher and pads too.
    generatorController->onPatternsChanged = [this]
    {
        if (generatorWindow != nullptr)
            generatorWindow->updateSlots (song.findGenerator (selectedGeneratorId),
                                          selectedPatternId);
    };

    transportBar->onOpenMixer = [this] { openMixer(); };
    transportBar->onExport = [this] { openExport(); };

    playlist.onSelectGenerator = [this] (const juce::String& generatorId)
    {
        generatorController->selectGenerator (generatorId);
    };

    // Double-clicking a row label opens the generator itself, so the Inst tab;
    // a clip's double click below opens the pattern it plays instead.
    playlist.onOpenGenerator = [this] (const juce::String& generatorId)
    {
        generatorController->selectGenerator (generatorId);
        openGeneratorWindow (GeneratorWindow::Tab::instrument);
    };

    playlist.onAddGenerator = [this] (juce::Rectangle<int> screenArea)
    {
        generatorController->showAddGeneratorMenu (screenArea);
    };

    playlist.onSeek = [this] (double beat) { seekToSongBeat (beat); };

    // Double-clicking a clip edits the pattern it plays: make that pattern the
    // selection, then open (or retarget) the pattern editor on it.
    playlist.onEditPattern = [this] (const juce::String& generatorId,
                                     const juce::String& patternId)
    {
        generatorController->selectGenerator (generatorId);
        generatorController->selectPattern (patternId);
        openPatternEditor();
    };

    playlistViewport.setViewedComponent (&playlist, false);

    addAndMakeVisible (*transportBar);
    // The automation lane's parameter menu: names and ranges come from the
    // live plugins, which the playlist deliberately cannot reach.
    playlist.getAutomatableParams = [this] (const juce::String& generatorId)
    {
        std::vector<PlaylistComponent::AutomatableParamInfo> result;
        result.push_back ({ model::AutomationLane::volumeTarget, {}, "Volume", 0.0f, 1.0f, 0.65f });
        result.push_back ({ model::AutomationLane::panTarget, {}, "Pan", -1.0f, 1.0f, 0.0f });

        const auto generators = song.getGenerators();
        const auto tracks = te::getAudioTracks (*edit);

        for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        {
            const auto& generator = generators[(size_t) i];

            if (generator.getId() != generatorId)
                continue;

            auto addParams = [&result] (te::Plugin* plugin, const juce::String& target,
                                        const juce::String& suffix)
            {
                if (plugin == nullptr)
                    return;

                for (auto param : plugin->getAutomatableParameters())
                {
                    const auto range = param->getValueRange();
                    result.push_back ({ target, param->paramID,
                                        param->getParameterName() + suffix,
                                        range.getStart(), range.getEnd(),
                                        param->getCurrentValue() });
                }
            };

            addParams (sync::findInstrumentPlugin (*tracks[i]),
                       model::AutomationLane::instrumentTarget, " (inst)");

            for (const auto& effect : generator.getEffects())
                for (auto plugin : tracks[i]->pluginList.getPlugins())
                    if (plugin->state.getProperty (sync::effectIdProperty).toString() == effect.getId())
                        addParams (plugin, effect.getId(),
                                   " (" + plugin->getShortName (6) + ")");

            break;
        }

        return result;
    };

    playlist.onClipSelectionChanged = [this] (const std::vector<model::PlaylistClip>& clips)
    {
        clipProperties.setSelection (clips);
    };

    // The playlist knows the current tool; the global shortcuts are ours, and
    // go last because the bar drops whatever doesn't fit.
    playlist.onShortcutHelpChanged = [this] (std::vector<ShortcutHelpBar::Entry> entries)
    {
        for (auto& global : globalShortcutHelp())
            entries.push_back (global);

        helpBar.setEntries (std::move (entries));
    };

    addAndMakeVisible (playlistViewport);
    addAndMakeVisible (clipProperties);
    addAndMakeVisible (helpBar);

    helpBar.setEntries (globalShortcutHelp());

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
    generatorWindow.reset();
    mixerWindow.reset();
    exportWindow.reset();
    undoManager.clearUndoHistory();

    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);

    editSync = std::make_unique<sync::EditSync> (song, *edit);

    transportBar->setSong (song);
    playlist.setSong (song);
    clipProperties.setSong (song);
    generatorController->setSong (song);   // fires onSelectionChanged -> updates piano roll

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
                             + " - " + carve::applicationName);

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

    if (generatorWindow != nullptr)
        retargetGeneratorWindow();   // retarget the open editor to the new selection
}

void MainComponent::openPatternEditor()
{
    openGeneratorWindow (GeneratorWindow::Tab::pianoRoll);
}

void MainComponent::openGeneratorWindow (GeneratorWindow::Tab tab)
{
    if (generatorWindow == nullptr)
    {
        auto onClose = [safe = juce::Component::SafePointer (this)]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->generatorWindow.reset();
            });
        };
        auto keyHandler = [safe = juce::Component::SafePointer (this)] (const juce::KeyPress& key)
        {
            return safe != nullptr && safe->handleGlobalKey (key);
        };
        generatorWindow = std::make_unique<GeneratorWindow> (undoManager, std::move (onClose),
                                                             std::move (keyHandler));

        generatorWindow->setPreviewNoteCallback (
            [safe = juce::Component::SafePointer (this)] (int pitch, int velocity)
            {
                if (safe != nullptr)
                    safe->previewNote (pitch, velocity);
            });

        generatorWindow->onSeekPatternBeat =
            [safe = juce::Component::SafePointer (this)] (double patternBeat)
        {
            if (safe != nullptr)
                safe->seekToPatternBeat (patternBeat);
        };

        // The window only reports gestures; the controller stays the selection
        // and model-edit authority, and its refresh comes back around through
        // onPatternsChanged to recolour the switcher and pads.
        auto& slots = generatorWindow->getSlotSwitcher();
        slots.onSlotClicked = [safe = juce::Component::SafePointer (this)] (model::PatternSlot slot)
        {
            if (safe != nullptr)
                safe->generatorController->selectSlot (slot);
        };
        slots.onSlotMenu = [safe = juce::Component::SafePointer (this)] (model::PatternSlot slot,
                                                                         juce::Rectangle<int> screenArea)
        {
            if (safe != nullptr)
                safe->generatorController->showSlotMenu (slot, screenArea);
        };

        generatorWindow->onUnslottedPatternPicked =
            [safe = juce::Component::SafePointer (this)] (const juce::String& patternId)
        {
            if (safe != nullptr)
                safe->generatorController->selectPattern (patternId);
        };

        auto& inst = generatorWindow->getInstrumentView();
        inst.onPadClicked = [safe = juce::Component::SafePointer (this)] (int pad,
                                                                          juce::Rectangle<int> screenArea)
        {
            if (safe != nullptr)
                safe->generatorController->padClicked (pad, screenArea);
        };
        inst.onPadFilesDropped = [safe = juce::Component::SafePointer (this)] (int startPad,
                                                                               const juce::StringArray& files)
        {
            if (safe != nullptr)
                safe->generatorController->padFilesDropped (startPad, files);
        };
        inst.isInterestedInPadFiles = [safe = juce::Component::SafePointer (this)] (const juce::StringArray& files)
        {
            return safe != nullptr && safe->generatorController->canImportAudioFiles (files);
        };
        inst.onLoadSample = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->chooseSampleForSelected();
        };
    }

    retargetGeneratorWindow();
    generatorWindow->showTab (tab);
    generatorWindow->toFront (true);
}

void MainComponent::retargetGeneratorWindow()
{
    auto generator = song.findGenerator (selectedGeneratorId);

    std::optional<model::Pattern> pattern;
    juce::String title;

    if (generator)
        if ((pattern = generator->findPattern (selectedPatternId)))
            title = generator->getName() + " / " + pattern->getName();

    // generator order == track order (EditSync invariant)
    te::Plugin* instrument = nullptr;
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
        if (generators[(size_t) i].getId() == selectedGeneratorId)
        {
            // Not findFirstPluginOfType<ExternalPlugin>: an insert effect can
            // be an external plugin too, and this must find the instrument.
            instrument = sync::findInstrumentPlugin (*tracks[i]);
            break;
        }

    // Song before pattern: the roll counts its bars in the song's signature,
    // and this path is reached on load too.
    generatorWindow->setSong (song);
    generatorWindow->setGenerator (generator, instrument, std::move (pattern), title);
}

void MainComponent::previewNote (int pitch, int velocity)
{
    // generator order == track order (EditSync invariant)
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        if (generators[(size_t) i].getId() != selectedGeneratorId)
            continue;

        // Guide notes go through the live MIDI path, which needs a playback
        // context — there isn't one until the transport has been started once.
        edit->getTransport().ensureContextAllocated();

        // autorelease: the roll only tells us a note was touched, never that it
        // was let go, so the engine has to end it for us.
        tracks[i]->playGuideNote (pitch, te::MidiChannel (1), velocity, true, false, true);
        return;
    }
}

void MainComponent::openExport()
{
    if (exportWindow != nullptr)
    {
        exportWindow->toFront (true);
        return;
    }

    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->exportWindow.reset();
        });
    };

    // The live plugin state belongs in the model before anything renders from
    // it, exactly as it does before a save.
    if (editSync != nullptr)
        editSync->captureLivePluginState();

    exportWindow = std::make_unique<ExportWindow> (*edit, song, std::move (onClose));
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
    fileChooser = std::make_shared<juce::FileChooser> ("Save song", currentFile, "*.carve");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        writeSongTo (file.withFileExtension ("carve"));
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
    fileChooser = std::make_shared<juce::FileChooser> ("Open song", juce::File(), "*.carve;*.orion");
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
    if (key == juce::KeyPress ('e', juce::ModifierKeys::commandModifier, 0))
    {
        openExport();
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

void MainComponent::seekToSongBeat (double beat)
{
    edit->getTransport().setPosition (
        edit->tempoSequence.toTime (te::BeatPosition::fromBeats (std::max (0.0, beat))));
}

void MainComponent::seekToPatternBeat (double patternBeat)
{
    // The earliest placement of the pattern: seeking "into the pattern" has to
    // pick one of its placements, and the first is the one the user can
    // predict without looking at the playlist.
    std::optional<double> earliest;

    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getPatternId() == selectedPatternId)
            if (! earliest || clip.getStart() < *earliest)
                earliest = clip.getStart();

    if (earliest)
        seekToSongBeat (*earliest + patternBeat);
}

std::optional<double> MainComponent::patternBeatOfPlayhead (double songBeat) const
{
    auto generator = song.findGenerator (selectedGeneratorId);
    if (! generator)
        return std::nullopt;

    auto pattern = generator->findPattern (selectedPatternId);
    if (! pattern || pattern->getLengthBeats() <= 0.0)
        return std::nullopt;

    const auto patternLength = pattern->getLengthBeats();

    // Inside any placement counts; a clip longer than the pattern loops it, so
    // the position folds back into pattern beats the same way playback does.
    for (const auto& clip : song.getPlaylist().getClips())
    {
        if (clip.getPatternId() != selectedPatternId)
            continue;

        const auto start = clip.getStart();
        const auto length = clip.getLength (patternLength);

        if (songBeat >= start && songBeat < start + length)
            return std::fmod (songBeat - start, patternLength);
    }

    return std::nullopt;
}

void MainComponent::timerCallback()
{
    const auto position = edit->getTransport().getPosition();
    const auto songBeat = edit->tempoSequence.toBeats (position).inBeats();

    playlist.setPlayheadBeats (songBeat);

    if (generatorWindow != nullptr)
        generatorWindow->setPlayheadPatternBeat (patternBeatOfPlayhead (songBeat));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar->setBounds (area.removeFromTop (44));
    helpBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));
    clipProperties.setBounds (area.removeFromRight (210));
    playlistViewport.setBounds (area);
    playlist.hostViewportResized();   // its minimum width is the viewport's
}

} // namespace carve::app
