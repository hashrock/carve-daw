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
    transportBar->onNew = [this] { newSong(); };
    transportBar->onSave = [this] { saveSong(); };
    transportBar->onSaveAs = [this] { saveSongAs(); };
    transportBar->onOpen = [this] { openSong(); };
    transportBar->onOpenDemo = [this] { openDemoSong(); };

    generatorController = std::make_unique<GeneratorController> (engine, song, undoManager);
    generatorController->onSelectionChanged = [this] (const juce::String& generatorId,
                                             const juce::String& patternId)
    {
        selectionChanged (generatorId, patternId);
    };
    generatorController->onManagePlugins = [this] { openPluginManager(); };

    // The pattern list changes without the selection moving (a rename, a
    // pattern made on another generator); the controller's refresh already
    // runs then, so it restocks the window's picker and pads too.
    generatorController->onPatternsChanged = [this]
    {
        if (generatorWindow != nullptr)
            generatorWindow->updatePatterns (song.findGenerator (selectedGeneratorId),
                                             selectedPatternId);
    };

    transportBar->onOpenMixer = [this] { openMixer(); };
    transportBar->onPlayModeChanged = [this] (bool on)
    {
        patternMode = on;
        updateAudition();
    };
    transportBar->getAuditionLengthBeats = [this] { return selectedPatternLengthBeats(); };
    transportBar->onExport = [this] { openExport(); };
    transportBar->onToggleBrowser = [this] { toggleBrowser(); };

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

    playlist.onGeneratorMenu = [this] (const juce::String& generatorId, juce::Rectangle<int> screenArea)
    {
        generatorController->showGeneratorMenu (generatorId, screenArea);
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

        playlistHelpEntries = entries;
        helpBar.setEntries (std::move (entries));
    };

    // The browser takes the bar over while the pointer is on it and hands it
    // back (an empty list) on leaving: the playlist only re-sends its entries
    // when they change, so the bar has to remember them for it.
    browser.onShortcutHelpChanged = [this] (std::vector<ShortcutHelpBar::Entry> entries)
    {
        helpBar.setEntries (entries.empty() ? playlistHelpEntries : std::move (entries));
    };

    // Double-clicking a file in the browser loads it: into the selected
    // sampler, onto the selected kit's next empty pad, or as a new sampler.
    browser.onFileActivated = [this] (const juce::File& file)
    {
        generatorController->loadSampleIntoSelected (file);
    };

    addAndMakeVisible (playlistViewport);
    addAndMakeVisible (clipProperties);
    addChildComponent (browser);
    addAndMakeVisible (helpBar);

    helpBar.setEntries (globalShortcutHelp());
    playlistHelpEntries = globalShortcutHelp();
    setBrowserShown (browser.wasShownLastSession());

    // An empty song, not the demo: the app opens on the user's next piece,
    // and the demo is a menu item away for anyone who wants to see one.
    loadSong (model::Song::create ("Untitled"));

    setWantsKeyboardFocus (true);
    startTimerHz (30);
    setSize (1320, 640);   // room for the browser beside the playlist
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

    // A generator added or retyped gets its instrument from the sync, which
    // runs after the selection change that opened the window on it; point
    // the window at the instrument once it exists. Cheap when nothing
    // changed: setGenerator returns early on the same kind and plugin.
    editSync->onSynced = [this]
    {
        if (generatorWindow != nullptr)
            retargetGeneratorWindow();
    };

    updateAudition();   // the mode outlives the song

    // The playback context is allocated up front rather than lazily on the
    // first note preview, so the first preview doesn't pay for the allocation
    // cascade (the live MIDI node's listener registration schedules a second
    // graph rebuild) -- and so the mixer's master meter has a context to read
    // while stopped.
    edit->getTransport().ensureContextAllocated();

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

    // In pattern mode the selection is what plays.
    if (patternMode)
        updateAudition();
}

void MainComponent::updateAudition()
{
    if (editSync == nullptr)
        return;

    if (patternMode && selectedGeneratorId.isNotEmpty() && selectedPatternId.isNotEmpty())
        editSync->setAudition (sync::Audition { selectedGeneratorId, selectedPatternId });
    else
        editSync->setAudition (std::nullopt);
}

double MainComponent::selectedPatternLengthBeats() const
{
    if (auto generator = song.findGenerator (selectedGeneratorId))
        if (auto pattern = generator->findPattern (selectedPatternId))
            return pattern->getLengthBeats();

    return 0.0;
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
        // onPatternsChanged to restock the picker and recolour the pads.
        auto& patterns = generatorWindow->getPatternPicker();
        patterns.onPatternPicked = [safe = juce::Component::SafePointer (this)] (const juce::String& patternId)
        {
            if (safe != nullptr)
                safe->generatorController->selectPattern (patternId);
        };
        patterns.onNewPattern = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->createPattern();
        };
        patterns.onClonePattern = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->clonePattern();
        };
        patterns.onPatternMenu = [safe = juce::Component::SafePointer (this)]
                                     (juce::Rectangle<int> screenArea)
        {
            if (safe != nullptr)
                safe->generatorController->showPatternMenu (screenArea);
        };

        auto& inst = generatorWindow->getInstrumentView();
        inst.onPadClicked = [safe = juce::Component::SafePointer (this)] (int pad,
                                                                          juce::Rectangle<int> screenArea)
        {
            if (safe != nullptr)
                safe->generatorController->padClicked (pad, screenArea);
        };
        inst.onPadTriggered = [safe = juce::Component::SafePointer (this)] (int pad)
        {
            // The same guide-note path the piano roll previews through, on the
            // note the pad is mapped to.
            if (safe != nullptr)
                safe->previewNote (drumkit::getNoteForPad (pad), drumkit::previewVelocity);
        };
        inst.onPadFilesDropped = [safe = juce::Component::SafePointer (this)] (int startPad,
                                                                               const juce::StringArray& files)
        {
            if (safe != nullptr)
                safe->generatorController->padFilesDropped (startPad, files);
        };
        inst.isInterestedInAudioFiles = [safe = juce::Component::SafePointer (this)] (const juce::StringArray& files)
        {
            return safe != nullptr && safe->generatorController->canImportAudioFiles (files);
        };
        inst.onSampleFilesDropped = [safe = juce::Component::SafePointer (this)] (const juce::StringArray& files)
        {
            if (safe != nullptr)
                safe->generatorController->sampleFilesDropped (files);
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

void MainComponent::toggleBrowser()
{
    setBrowserShown (! browser.isVisible());
}

void MainComponent::setBrowserShown (bool shown)
{
    browser.setVisible (shown);   // the browser remembers this itself
    transportBar->setBrowserShown (shown);
    resized();
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

void MainComponent::saveSong (std::function<void (bool)> onDone)
{
    if (currentFile != juce::File())
    {
        const bool saved = writeSongTo (currentFile);

        if (onDone)
            onDone (saved);

        return;
    }

    saveSongAs (std::move (onDone));
}

void MainComponent::saveSongAs (std::function<void (bool)> onDone)
{
    fileChooser = std::make_shared<juce::FileChooser> ("Save song", currentFile, "*.carve");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, onDone = std::move (onDone)] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();

        // Backing out of the chooser is not an error, but it is not a save
        // either -- and whoever asked for one needs to know the difference.
        if (file == juce::File())
        {
            if (onDone)
                onDone (false);

            return;
        }

        const bool saved = writeSongTo (file.withFileExtension ("carve"));

        if (onDone)
            onDone (saved);
    });
}

bool MainComponent::writeSongTo (const juce::File& file)
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
        return false;
    }

    currentFile = file;
    hasUnsavedChanges = false;   // captureLivePluginState above will have set it
    updateDocumentDisplay();
    return true;
}

void MainComponent::confirmDiscardChanges (std::function<void (bool)> onResolved)
{
    if (! hasUnsavedChanges)
    {
        onResolved (true);
        return;
    }

    const auto documentName = currentFile != juce::File() ? currentFile.getFileName()
                                                          : juce::String ("Untitled");

    // showYesNoCancelBox rather than a MessageBoxOptions alert: its three
    // buttons come back as documented values (1 / 2 / 0) whether the look and
    // feel draws the box itself or hands it to the OS, which the generic
    // showAsync does not promise.
    juce::AlertWindow::showYesNoCancelBox (
        juce::MessageBoxIconType::WarningIcon,
        "Unsaved changes",
        documentName + " has changes that have not been saved.",
        "Save", "Don't Save", "Cancel",
        this,
        juce::ModalCallbackFunction::create (
            [safe = juce::Component::SafePointer (this),
             onResolved = std::move (onResolved)] (int result)
    {
        if (safe == nullptr)
            return;

        if (result == 2)            // Don't Save
        {
            onResolved (true);
            return;
        }

        if (result != 1)            // Cancel, or the box dismissed some other way
        {
            onResolved (false);
            return;
        }

        // Save, and go ahead only if the song actually reached disk: a
        // cancelled chooser or a failed write must not take it with them.
        safe->saveSong (onResolved);
    }));
}

void MainComponent::newSong()
{
    confirmDiscardChanges ([safe = juce::Component::SafePointer (this)] (bool goAhead)
    {
        if (safe != nullptr && goAhead)
            safe->loadSong (model::Song::create ("Untitled"));
    });
}

void MainComponent::openDemoSong()
{
    // The demo has no file behind it: it opens as an unsaved "Untitled", so
    // that playing with it and then saving asks where, rather than
    // overwriting whatever was open before.
    confirmDiscardChanges ([safe = juce::Component::SafePointer (this)] (bool goAhead)
    {
        if (safe != nullptr && goAhead)
            safe->loadSong (model::buildDemoSong());
    });
}

void MainComponent::openSong()
{
    confirmDiscardChanges ([safe = juce::Component::SafePointer (this)] (bool goAhead)
    {
        if (safe == nullptr || ! goAhead)
            return;

        safe->fileChooser = std::make_shared<juce::FileChooser> ("Open song", juce::File(), "*.carve;*.orion");
        safe->fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                            | juce::FileBrowserComponent::canSelectFiles,
                                        [safe] (const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();

            if (safe != nullptr && file != juce::File())
                safe->openSongFile (file);
        });
    });
}

void MainComponent::openSongFile (const juce::File& file)
{
    if (auto loaded = model::Song::loadFromFile (file))
    {
        loadSong (*loaded, file);
        return;
    }

    juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                 "Open failed",
                                                 "Could not read " + file.getFullPathName());
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
    if (key == juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0))
    {
        toggleBrowser();
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

    // The Clone button without the trip to the header. Cloning is the one
    // pattern gesture that happens mid-take, which is exactly when reaching
    // for a button is what breaks it.
    if (key == juce::KeyPress ('d', juce::ModifierKeys::commandModifier, 0))
    {
        generatorController->clonePattern();
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
    // In pattern mode the pattern starts at beat zero, by construction.
    if (patternMode)
    {
        seekToSongBeat (patternBeat);
        return;
    }

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

    if (patternMode)
        return std::fmod (std::max (0.0, songBeat), patternLength);

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
    transportBar->setBounds (area.removeFromTop (TransportBar::preferredHeight));
    helpBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));
    clipProperties.setBounds (area.removeFromRight (210));

    if (browser.isVisible())
        browser.setBounds (area.removeFromLeft (SampleBrowser::preferredWidth));

    playlistViewport.setBounds (area);
    playlist.hostViewportResized();   // its minimum width is the viewport's
}

} // namespace carve::app
