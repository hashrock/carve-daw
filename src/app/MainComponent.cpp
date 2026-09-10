#include "MainComponent.h"

#include "sync/EngineIds.h"

#include "EngineSetup.h"
#include "model/DemoSong.h"
#include "model/SampleSongs.h"

namespace carve::app
{

//==============================================================================
// The recent songs, in the same properties file. juce::RecentlyOpenedFilesList
// does the ordering and the de-duplicating; all that is kept is its string.
static constexpr const char* recentSongsKey = "recentSongs";

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
    transportBar->getRecentSongs = [this] { return recentSongNames(); };
    transportBar->onOpenRecent = [this] (int index) { openRecentSong (index); };
    transportBar->onOpenSampleSong = [this] (int index) { openSampleSong (index); };

    recentSongs.setMaxNumberOfItems (10);
    recentSongs.restoreFromString (engine.getPropertyStorage().getPropertiesFile()
                                       .getValue (recentSongsKey));

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

        // Pattern mode plays a pattern of every generator, not only the
        // selected one, so a pattern deleted or restored on another generator
        // changes what it should play without moving the selection. Cheap
        // when nothing did: setAudition compares by value.
        if (patternMode)
            updateAudition();
    };

    transportBar->onOpenMixer = [this] { openMixer(); };
    transportBar->onPlayModeChanged = [this] (bool on)
    {
        patternMode = on;
        updateAudition();
    };
    transportBar->getAuditionLengthBeats = [this] { return auditionLengthBeats(); };
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
    playlist.onSelectPattern = [this] (const juce::String& generatorId,
                                       const juce::String& patternId)
    {
        generatorController->selectGenerator (generatorId);
        generatorController->selectPattern (patternId);
    };

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

    playlist.onClipSelectionChanged = [this] (const std::vector<model::PlaylistClip>& clips,
                                              const std::vector<model::AudioClip>& audioClips)
    {
        clipProperties.setSelection (clips, audioClips);
    };

    clipProperties.onReloadAudioClip = [this] (const juce::String& placementId)
    {
        if (editSync != nullptr)
            editSync->reloadAudioClip (placementId);
        playlist.invalidateWaveforms();
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
    missingMediaWindow.reset();   // it was about the song that just went away
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

    // After the flags above on purpose: a relink is a change to the document
    // the file on disk does not have, so it should count as unsaved.
    checkForMissingMedia();
}

void MainComponent::checkForMissingMedia()
{
    // The song's own folder first, without asking: a project moved with its
    // media whose relative paths no longer line up is the common case, and
    // the one the user cannot be expected to help with.
    if (currentFile != juce::File())
        song.relinkMissingMedia (currentFile.getParentDirectory(), nullptr);

    if (song.findMissingMedia().empty())
        return;

    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->missingMediaWindow.reset();
        });
    };

    // Deferred so the main window is on screen first: the constructor loads
    // the demo song through here before there is one.
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer (this),
                                      songHandle = song, onClose = std::move (onClose)]
    {
        // Only for the song that was loaded; a load that came in between
        // has had its own check.
        if (safe == nullptr || safe->song.state != songHandle.state)
            return;

        safe->missingMediaWindow = std::make_unique<MissingMediaWindow> (songHandle, onClose);
    });
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

    if (! patternMode)
    {
        editSync->setAudition (std::nullopt);
        return;
    }

    // Every generator's current pattern, so the mode plays the song's parts
    // together the way they are being worked on. An empty list is still
    // Pattern mode -- a song with nothing to audition plays silence, not the
    // playlist.
    sync::Audition audition;

    for (const auto& current : generatorController->getCurrentPatterns())
        audition.entries.push_back ({ current.generatorId, current.patternId });

    editSync->setAudition (std::move (audition));

    // Picking a longer pattern on any generator lengthens the loop; a running
    // transport keeps its own copy of the range, so it is told.
    if (transportBar != nullptr)
        transportBar->refreshPatternLoop();
}

double MainComponent::auditionLengthBeats() const
{
    if (editSync != nullptr)
        if (const auto& audition = editSync->getAudition())
            return sync::auditionLengthBeats (song, *audition);

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
        generatorWindow = std::make_unique<GeneratorWindow> (engine, undoManager, std::move (onClose),
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
        inst.onPadParameterChanged = [this] (int pad, int parameter, double value)
        {
            if (auto generator = song.findGenerator (selectedGeneratorId))
                if (auto sound = drumkit::findSoundForPad (*generator, pad))
                {
                    undoManager.beginNewTransaction();
                    if (parameter == 0) sound->setGainDb ((float) value, &undoManager);
                    if (parameter == 1) sound->setRootNote (drumkit::getNoteForPad (pad) - (int) value, &undoManager);
                    if (parameter == 2) sound->setLengthSeconds (value, &undoManager);
                }
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

    // The same door the playlist's row label is, so the mixer needs no way of
    // its own to get at a generator's editor.
    mixerWindow->setOpenGeneratorHandler ([safe = juce::Component::SafePointer (this)]
                                          (const juce::String& generatorId)
    {
        if (safe == nullptr)
            return;

        safe->generatorController->selectGenerator (generatorId);
        safe->openGeneratorWindow (GeneratorWindow::Tab::instrument);
    });

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

//==============================================================================
// Where the last song was opened from or saved to. Without one of its own the
// chooser opens on whatever the app browsed last, which is the sample library:
// picking samples happens many times per session and opening a song once, so
// the songs kept losing the race. Sits in the same properties file the sample
// directory uses (see GeneratorController) because tracktion's PropertyStorage
// only takes keys from its own fixed SettingID enum.
static constexpr const char* lastSongDirectoryKey = "lastSongDirectory";

juce::File MainComponent::songBrowseDirectory() const
{
    const juce::File remembered (engine.getPropertyStorage().getPropertiesFile()
                                     .getValue (lastSongDirectoryKey));

    // A folder that has since been deleted or unmounted would open the chooser
    // on nothing at all, so fall back rather than trust it.
    if (remembered.isDirectory())
        return remembered;

    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

void MainComponent::rememberSongDirectory (const juce::File& songFile)
{
    engine.getPropertyStorage().getPropertiesFile()
        .setValue (lastSongDirectoryKey, songFile.getParentDirectory().getFullPathName());
}

//==============================================================================
void MainComponent::rememberRecentSong (const juce::File& songFile)
{
    recentSongs.addFile (songFile);
    engine.getPropertyStorage().getPropertiesFile().setValue (recentSongsKey, recentSongs.toString());
}

juce::StringArray MainComponent::recentSongNames()
{
    // A song that has been moved or deleted since would open onto an error
    // box, so it leaves the list here rather than sitting in the menu.
    recentSongs.removeNonExistentFiles();

    juce::StringArray names;

    for (int i = 0; i < recentSongs.getNumFiles(); ++i)
        names.add (recentSongs.getFile (i).getFileName());

    return names;
}

void MainComponent::openRecentSong (int index)
{
    // Between the menu being built and an item being picked the file could
    // have gone away, and removeNonExistentFiles above may have shifted the
    // list; both come out as an index with no file behind it.
    const auto file = recentSongs.getFile (index);

    if (! file.existsAsFile())
        return;

    confirmDiscardChanges ([safe = juce::Component::SafePointer (this), file] (bool goAhead)
    {
        if (safe != nullptr && goAhead)
            safe->openSongFile (file);
    });
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
    const auto startAt = currentFile != juce::File() ? currentFile : songBrowseDirectory();

    fileChooser = std::make_shared<juce::FileChooser> ("Save song", startAt, "*.carve");
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
    rememberSongDirectory (file);
    rememberRecentSong (file);
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

void MainComponent::openSampleSong (int index)
{
    // Like the demo: built rather than loaded, so it opens as an unsaved
    // "Untitled" and saving it asks where rather than writing over anything
    // -- least of all over the copy the app ships with.
    confirmDiscardChanges ([safe = juce::Component::SafePointer (this), index] (bool goAhead)
    {
        if (safe != nullptr && goAhead)
            safe->loadSong (model::buildSampleSong (index));
    });
}

void MainComponent::openSong()
{
    confirmDiscardChanges ([safe = juce::Component::SafePointer (this)] (bool goAhead)
    {
        if (safe == nullptr || ! goAhead)
            return;

        safe->fileChooser = std::make_shared<juce::FileChooser> ("Open song", safe->songBrowseDirectory(),
                                                                 "*.carve;*.orion");
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
        rememberSongDirectory (file);
        rememberRecentSong (file);
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

void MainComponent::perform (Action action)
{
    switch (action)
    {
        case Action::newSong:           newSong(); break;
        case Action::openSong:          openSong(); break;
        case Action::openDemoSong:      openDemoSong(); break;
        case Action::save:              saveSong(); break;
        case Action::saveAs:            saveSongAs(); break;
        case Action::exportWav:         openExport(); break;
        case Action::undo:              undoManager.undo(); break;
        case Action::redo:              undoManager.redo(); break;
        case Action::toggleBrowser:     toggleBrowser(); break;
        case Action::openMixer:         openMixer(); break;
        case Action::openPatternEditor: openPatternEditor(); break;
        case Action::pluginManager:     openPluginManager(); break;
    }
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
