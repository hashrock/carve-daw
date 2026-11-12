#include "MainComponent.h"

#include "sync/EffectChains.h"
#include "sync/EngineIds.h"

#include "EngineSetup.h"
#include "SandboxAccess.h"
#include "AppFiles.h"
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

    // Playback starts out of mastering mode: the chain is for the end, and
    // its lookahead and CPU are in the way of everything before that. Kept on
    // the Edit, so it outlives the songs loaded into it.
    sync::setMasteringActive (*edit, false);

    // The reference player, last on the master, for as long as the Edit lasts.
    referencePlayerPlugin = edit->getPluginCache().createNewPlugin (plugins::ReferencePlayerPlugin::xmlTypeName, {});
    referencePlayer = dynamic_cast<plugins::ReferencePlayerPlugin*> (referencePlayerPlugin.get());

    if (referencePlayerPlugin != nullptr)
        edit->getMasterPluginList().insertPlugin (referencePlayerPlugin, -1, nullptr);

    // And after it, the analyzer: what it measures is what is heard.
    analyzerPlugin = edit->getPluginCache().createNewPlugin (plugins::MasterAnalyzerPlugin::xmlTypeName, {});
    analyzer = dynamic_cast<plugins::MasterAnalyzerPlugin*> (analyzerPlugin.get());

    if (analyzerPlugin != nullptr)
        edit->getMasterPluginList().insertPlugin (analyzerPlugin, -1, nullptr);

    transportBar = std::make_unique<TransportBar> (*edit, song, undoManager);
    transportBar->onNew = [this] { newSong(); };
    transportBar->onSave = [this] { saveSong(); };
    transportBar->onSaveAs = [this] { saveSongAs(); };
    transportBar->onOpen = [this] { openSong(); };
    transportBar->onOpenDemo = [this] { openDemoSong(); };
    transportBar->getRecentSongs = [this] { return recentSongNames(); };
    transportBar->onOpenRecent = [this] (int index) { openRecentSong (index); };
    transportBar->onOpenSampleSong = [this] (int index) { openSampleSong (index); };
    transportBar->onSettings = [this] { openSettings(); };
    transportBar->onAbout = [this] { if (onShowAbout) onShowAbout(); };
    transportBar->onNotices = [this] { if (onShowNotices) onShowNotices(); };

    recentSongs.setMaxNumberOfItems (10);
    recentSongs.restoreFromString (engine.getPropertyStorage().getPropertiesFile()
                                       .getValue (recentSongsKey));

    // Live MIDI in. Built before the controller below so that the selection
    // callbacks, which fire from it, always have somewhere to point.
    midiInput = std::make_unique<MidiInputController> (engine, *edit, song, undoManager);
    midiInput->onRecordingChanged = [this] (bool isRecording)
    {
        transportBar->setRecording (isRecording);
    };
    transportBar->onRecordToggled = [this] (bool shouldRecord)
    {
        midiInput->setRecording (shouldRecord);
    };
    transportBar->setRecordAvailable (midiInput->hasMidiInput());
    transportBar->onCapture = [this] { capturePlayed(); };
    transportBar->onToggleTypingKeyboard = [this] { toggleTypingKeyboard(); };

    generatorController = std::make_unique<GeneratorController> (engine, song, undoManager);
    generatorController->onSelectionChanged = [this] (const juce::String& generatorId,
                                             const juce::String& patternId)
    {
        selectionChanged (generatorId, patternId);
    };
    generatorController->onManagePlugins = [this] { openPluginManager(); };

    // A sampler, SoundFont player or singer made empty: straight to the tab
    // that fills it, rather than a file chooser holding everything up.
    generatorController->onOpenInstrumentForSetup = [this]
    {
        openGeneratorWindow (GeneratorWindow::Tab::instrument);
    };

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
    transportBar->onOpenAnalyzer = [this] { openAnalyzer(); };
    transportBar->onPlayModeChanged = [this] (bool on)
    {
        patternMode = on;
        midiInput->setPatternMode (on);
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
    leaveDragsToContent (playlistViewport);

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

    // Before the sync: a recording still running would otherwise write its
    // last note into a pattern that has just been replaced.
    midiInput->setSong (song);

    editSync = std::make_unique<sync::EditSync> (song, *edit);

    // A generator added or retyped gets its instrument from the sync, which
    // runs after the selection change that opened the window on it; point
    // the window at the instrument once it exists. Cheap when nothing
    // changed: setGenerator returns early on the same kind and plugin.
    editSync->onSynced = [this]
    {
        if (generatorWindow != nullptr)
            retargetGeneratorWindow();

        // A mastering effect added goes on the end of the master list,
        // which is where the reference player has to stay.
        keepReferencePlayerLast();
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
    if (capturingPluginState)
        return;

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
    midiInput->setTarget (generatorId, patternId);

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

    // Every generator's current pattern, so the mode plays the song's parts
    // together the way they are being worked on. An empty list is still
    // Pattern mode -- a song with nothing to audition plays silence, not the
    // playlist.
    sync::Audition audition;
    std::map<juce::String, juce::String> current;

    for (const auto& entry : generatorController->getCurrentPatterns())
    {
        audition.entries.push_back ({ entry.generatorId, entry.patternId });
        current[entry.generatorId] = entry.patternId;
    }

    // The playlist shows what the mode plays: the rack in Pattern mode, the
    // song otherwise -- through an export too, which plays the song but is
    // over in a moment, and the view should not flicker for it.
    playlist.setRack (patternMode, current, sync::auditionLengthBeats (song, audition));

    // An export in progress holds the Edit on the song, and a pattern picked
    // meanwhile must not swap the patterns back in under the render.
    if (! patternMode || exportingSong)
    {
        editSync->setAudition (std::nullopt);
        return;
    }

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
        inst.onLoadSoundFont = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->chooseSoundFontForSelected();
        };
        inst.onSoundFontDropped = [safe = juce::Component::SafePointer (this)] (const juce::File& file)
        {
            if (safe != nullptr)
                safe->generatorController->soundFontDroppedOnSelected (file);
        };
        inst.onSoundFontPresetChosen = [safe = juce::Component::SafePointer (this)] (int bank, int program)
        {
            if (safe != nullptr)
                safe->generatorController->setSoundFontPresetForSelected (bank, program);
        };
        inst.onLoadVoicebank = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->chooseVoicebankForSelected();
        };
        inst.onVoicebankDropped = [safe = juce::Component::SafePointer (this)] (const juce::File& folder)
        {
            if (safe != nullptr)
                safe->generatorController->voicebankDroppedOnSelected (folder);
        };
        inst.onVoiceStyleChanged = [safe = juce::Component::SafePointer (this)] (const juce::Identifier& id, double value)
        {
            if (safe != nullptr)
                safe->generatorController->setVoiceStyleValueForSelected (id, value);
        };
        inst.onLoadBreak = [safe = juce::Component::SafePointer (this)]
        {
            if (safe != nullptr)
                safe->generatorController->chooseBreakForSelected();
        };
        inst.onBreakDropped = [safe = juce::Component::SafePointer (this)] (const juce::File& file)
        {
            if (safe != nullptr)
                safe->generatorController->breakDroppedOnSelected (file);
        };
        inst.onBreakCutsReplaced = [safe = juce::Component::SafePointer (this)] (const std::vector<double>& cuts)
        {
            if (safe != nullptr)
                safe->generatorController->setBreakCutsForSelected (cuts);
        };
        inst.onBreakCutMoved = [safe = juce::Component::SafePointer (this)] (int index, double seconds)
        {
            if (safe != nullptr)
                safe->generatorController->moveBreakCutForSelected (index, seconds);
        };
        inst.onBreakCutAdded = [safe = juce::Component::SafePointer (this)] (double seconds)
        {
            if (safe != nullptr)
                safe->generatorController->addBreakCutForSelected (seconds);
        };
        inst.onBreakCutRemoved = [safe = juce::Component::SafePointer (this)] (int index)
        {
            if (safe != nullptr)
                safe->generatorController->removeBreakCutForSelected (index);
        };
        inst.onBreakBarsChanged = [safe = juce::Component::SafePointer (this)] (int bars)
        {
            if (safe != nullptr)
                safe->generatorController->setBreakBarsForSelected (bars);
        };
        inst.onBreakRepitchChanged = [safe = juce::Component::SafePointer (this)] (bool repitch)
        {
            if (safe != nullptr)
                safe->generatorController->setBreakRepitchForSelected (repitch);
        };
        inst.getBeatsPerBar = [safe = juce::Component::SafePointer (this)]
        {
            return safe != nullptr ? safe->song.getTimeSigAt (0.0).getBeatsPerBar() : 4.0;
        };
        inst.getRecentMedia = [safe = juce::Component::SafePointer (this)] (MediaKind kind)
        {
            return safe != nullptr ? safe->generatorController->getRecentMedia (kind)
                                   : std::vector<RecentMediaList::Item>();
        };
        inst.onRecentMediaChosen = [safe = juce::Component::SafePointer (this)] (MediaKind kind, const juce::File& file)
        {
            if (safe != nullptr)
                safe->generatorController->recentMediaChosen (kind, file);
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
    //
    // Not an edit, though. Copying it in rewrites every instrument's and
    // effect's state node whether or not anything in it differs, and a song
    // that never had them stored (a sample song, a new synth) gains them for
    // the first time -- either way the tree hears a change and the document
    // went unsaved just for opening this window. What it copies is not the
    // user's doing either: turning a plugin's knob changes the plugin's own
    // tree, never the song's, so it never made the document unsaved before
    // the copy and does not after it.
    if (editSync != nullptr)
    {
        const juce::ScopedValueSetter<bool> notAnEdit (capturingPluginState, true);
        editSync->captureLivePluginState();
    }

    exportWindow = std::make_unique<ExportWindow> (*edit, song, std::move (onClose));

    // An export is of the song, whichever mode the transport is in.
    exportWindow->setOnRenderingChanged ([safe = juce::Component::SafePointer (this)] (bool rendering,
                                                                                    bool withMastering)
    {
        if (safe != nullptr)
        {
            safe->exportingSong = rendering;
            safe->updateAudition();

            // ...and through the mastering chain unless asked not to, whether
            // or not it is being listened to: then back to how it was.
            if (safe->editSync != nullptr)
                safe->editSync->setMasteringActive (rendering ? withMastering : safe->masteringMode);
        }
    });
}

void MainComponent::capturePlayed()
{
    if (generatorController->capturePattern (midiInput->getPlayed()))
        midiInput->clearPlayed();
}

void MainComponent::toggleTypingKeyboard()
{
    auto& typing = midiInput->getTypingKeyboard();
    typing.setEnabled (! typing.isEnabled());
    transportBar->setTypingKeyboardOn (typing.isEnabled());
}

void MainComponent::openSettings()
{
    if (settingsWindow != nullptr)
    {
        settingsWindow->toFront (true);
        return;
    }

    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->settingsWindow.reset();
        });
    };

    settingsWindow = std::make_unique<SettingsWindow> (engine, std::move (onClose));
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

    mixerWindow->setMasteringMode (masteringMode);
    mixerWindow->setMasteringModeHandler ([safe = juce::Component::SafePointer (this)] (bool on)
    {
        if (safe != nullptr)
            safe->setMasteringMode (on);
    });

    mixerWindow->setSong (song);
}

//==============================================================================
// Reference A/B

// The master list ends reference player, then analyzer -- whatever the
// sync has added since.
void MainComponent::keepReferencePlayerLast()
{
    auto state = edit->getMasterPluginList().state;
    const int count = state.getNumChildren();

    auto moveTo = [&state] (const te::Plugin::Ptr& plugin, int wanted)
    {
        if (plugin == nullptr)
            return;

        if (const auto index = state.indexOf (plugin->state); index >= 0 && index != wanted)
            state.moveChild (index, wanted, nullptr);
    };

    moveTo (referencePlayerPlugin, count - (analyzerPlugin != nullptr ? 2 : 1));
    moveTo (analyzerPlugin, count - 1);
}

void MainComponent::openAnalyzer()
{
    if (analyzer == nullptr)
        return;

    if (analyzerWindow != nullptr)
    {
        analyzerWindow->toFront (true);
        return;
    }

    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->analyzerWindow.reset();
        });
    };
    auto keyHandler = [safe = juce::Component::SafePointer (this)] (const juce::KeyPress& key)
    {
        return safe != nullptr && safe->handleGlobalKey (key);
    };

    analyzerWindow = std::make_unique<AnalyzerWindow> (engine, *analyzer, referencePlayer, std::move (onClose), std::move (keyHandler));
}

// What the reference player plays follows the song: the soloed reference
// track's clips, at their places (from the first of them in Pattern mode, as
// the pattern loops from zero), its fader as a trim. One reference is heard
// at a time -- a reference soloed unsolos any other, the way radio buttons
// do -- and the meter takes the reference's colour while it is playing.
void MainComponent::updateReferencePlayer()
{
    if (referencePlayer == nullptr)
        return;

    std::vector<model::Generator> soloed;

    for (const auto& generator : song.getGenerators())
        if (generator.isReference() && generator.isSoloed())
            soloed.push_back (generator);

    // The newly soloed one wins; the one that was already playing gives way.
    if (soloed.size() > 1)
    {
        auto keep = std::find_if (soloed.begin(), soloed.end(),
                                  [this] (const model::Generator& g) { return g.getId() != lastSoloedReference; });

        for (auto& g : soloed)
            if (g.getId() != keep->getId())
                g.setSoloed (false, nullptr);

        soloed = { *keep };
    }

    const auto* active = soloed.empty() ? nullptr : &soloed.front();

    if (active != nullptr)
        lastSoloedReference = active->getId();

    // The clips of the soloed reference, or failing that the one last
    // soloed, so the player has it loaded before it is asked for.
    std::optional<model::Generator> source;

    for (const auto& generator : song.getGenerators())
        if (generator.isReference() && (active != nullptr ? generator.getId() == active->getId()
                                                           : generator.getId() == lastSoloedReference))
            source = generator;

    std::vector<plugins::ReferencePlayerPlugin::Clip> clips;
    double firstStart = 0.0;

    if (source)
        for (const auto& placement : song.getPlaylist().getAudioClips())
            if (placement.getGeneratorId() == source->getId() && placement.getFile().existsAsFile())
            {
                plugins::ReferencePlayerPlugin::Clip clip;
                clip.file = placement.getFile();
                clip.start = edit->tempoSequence.toTime (te::BeatPosition::fromBeats (placement.getStart())).inSeconds();
                clip.length = placement.getLengthSeconds();
                clip.fileOffset = placement.getOffsetSeconds();
                firstStart = clips.empty() ? clip.start : juce::jmin (firstStart, clip.start);
                clips.push_back (clip);
            }

    referencePlayer->setClips (std::move (clips),
                               source ? source->getVolumeDb() : 0.0f,
                               ! source || source->isReferenceLevelMatched(),
                               patternMode ? firstStart : 0.0);

    const bool listening = active != nullptr;
    referencePlayer->setListening (listening);
    transportBar->setListeningToReference (listening);
}

// Backslash: the last soloed reference's solo, flipped -- A/B from the keys.
void MainComponent::toggleReferenceSolo()
{
    for (auto generator : song.getGenerators())
        if (generator.isReference() && (lastSoloedReference.isEmpty() || generator.getId() == lastSoloedReference))
        {
            lastSoloedReference = generator.getId();
            generator.setSoloed (! generator.isSoloed(), nullptr);
            updateReferencePlayer();
            return;
        }
}

void MainComponent::setMasteringMode (bool on)
{
    masteringMode = on;

    // An export holds the chain the way the export asked for until it is
    // done, and puts this back afterwards (see openExport).
    if (editSync != nullptr && ! exportingSong)
        editSync->setMasteringActive (on);

    if (mixerWindow != nullptr)
        mixerWindow->setMasteringMode (on);
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
    // Every way in is compiled out of a build that hosts no plugins; this is
    // for the one nobody thought of.
   #if ! CARVE_PLUGIN_HOSTING
    return;
   #else
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
   #endif
}

//==============================================================================
// Where the last song was opened from or saved to. Without one of its own the
// chooser opens on whatever the app browsed last, which is the sample library:
// picking samples happens many times per session and opening a song once, so
// the songs kept losing the race. Sits in the same properties file the sample
// directory uses (see GeneratorController) because tracktion's PropertyStorage
// only takes keys from its own fixed SettingID enum.
static constexpr const char* lastSongDirectoryKey = "lastSongDirectory";

// The iPad's Save As: a name, and the song goes into the app's Documents
// folder under it (see AppFiles.h) -- a save panel there could only offer
// places the song could not then be saved in.
void MainComponent::saveSongIntoDocuments (std::function<void (bool)> onDone)
{
    const auto suggested = currentFile != juce::File() ? currentFile.getFileNameWithoutExtension()
                         : song.getName().isNotEmpty() ? song.getName()
                                                       : juce::String ("Untitled");

    // Owned by the callback, so it goes when the box is answered.
    auto prompt = std::make_shared<juce::AlertWindow> ("Save song", "In On My iPad > Carve DAW",
                                                       juce::MessageBoxIconType::NoIcon, this);
    prompt->addTextEditor ("name", suggested);
    prompt->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    prompt->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    // Above the floating windows, as the look and feel puts its own alerts:
    // under the mixer, nothing could reach its buttons.
    prompt->setAlwaysOnTop (true);

    prompt->enterModalState (true, juce::ModalCallbackFunction::create (
        [safe = juce::Component::SafePointer (this), prompt, onDone = std::move (onDone)] (int result)
    {
        if (safe == nullptr)
            return;

        const auto name = juce::File::createLegalFileName (prompt->getTextEditorContents ("name").trim());

        if (result != 1 || name.isEmpty())
        {
            if (onDone)
                onDone (false);

            return;
        }

        // The song's own file again is a save; any other song of that name
        // is kept, and this one numbered beside it.
        auto file = appfiles::documents().getChildFile (name + ".carve");

        if (file.exists() && file != safe->currentFile)
            file = appfiles::freshChild (appfiles::documents(), name, ".carve");

        const bool saved = safe->writeSongTo (file);

        if (onDone)
            onDone (saved);
    }), false);

    if (auto* editor = prompt->getTextEditor ("name"))
        editor->grabKeyboardFocus();
}

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
    // On the iPad a song opened from elsewhere in Files can be read but not
    // saved over (see AppFiles.h), so saving it means naming a copy.
    const bool canSaveInPlace = ! appfiles::writesIntoDocuments || appfiles::isInDocuments (currentFile);

    if (currentFile != juce::File() && canSaveInPlace)
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
    if (appfiles::writesIntoDocuments)
    {
        saveSongIntoDocuments (std::move (onDone));
        return;
    }

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
                                                     "Could not write " + appfiles::describe (file));
        return false;
    }

    currentFile = file;
    sandbox::rememberAccess (file);   // so the next launch can save over it, and reopen it
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
            const auto file = sandbox::claimChosen (chooser.getURLResult());

            if (safe != nullptr && file != juce::File())
                safe->openSongFile (file);
        });
    });
}

void MainComponent::openSongFile (const juce::File& file)
{
    if (auto loaded = model::Song::loadFromFile (file))
    {
        sandbox::rememberAccess (file);   // so Open Recent can open it next time
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
        case Action::settings:          openSettings(); break;
    }
}

bool MainComponent::handleGlobalKey (const juce::KeyPress& key)
{
    // Ahead of the rest: with nothing focused a typed note arrives here
    // rather than at the component the typing keyboard is listening on.
    if (midiInput->getTypingKeyboard().keyPressed (key, this))
        return true;

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
    if (key == juce::KeyPress ('r', juce::ModifierKeys::commandModifier, 0))
    {
        transportBar->toggleRecord();
        return true;
    }
    if (key == juce::KeyPress ('r', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        capturePlayed();
        return true;
    }
    if (key == juce::KeyPress ('k', juce::ModifierKeys::commandModifier, 0))
    {
        toggleTypingKeyboard();
        return true;
    }
    if (key == juce::KeyPress ('a', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        openAnalyzer();
        return true;
    }
    if (key == juce::KeyPress ('\\', juce::ModifierKeys::noModifiers, 0))
    {
        toggleReferenceSolo();
        return true;
    }
    if (key == juce::KeyPress ('m', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        setMasteringMode (! masteringMode);
        return true;
    }
    if (key == juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0))
    {
        openSettings();
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
    updateReferencePlayer();

    // The analyzer reads before the master fader, which comes after it.
    if (analyzer != nullptr)
        analyzer->setMonitorGainDb (song.getMasterBus().getVolumeDb());

    // A keyboard plugged in while the app is running: the device list is
    // watched, but the button that greys itself out on it is here.
    transportBar->setRecordAvailable (midiInput->hasMidiInput());
    transportBar->setCaptureAvailable (midiInput->canCapture());

    if (generatorWindow != nullptr)
        generatorWindow->setPlayheadPatternBeat (patternBeatOfPlayhead (songBeat));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar->setBounds (area.removeFromTop (transportBar->getHeightForWidth (area.getWidth())));
    helpBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));
    clipProperties.setBounds (area.removeFromRight (210));

    if (browser.isVisible())
        browser.setBounds (area.removeFromLeft (SampleBrowser::preferredWidth));

    playlistViewport.setBounds (area);
    playlist.hostViewportResized();   // its minimum width is the viewport's
}

} // namespace carve::app
