#include "GeneratorController.h"

#include "DrumPadGrid.h"
#include "model/MidiPatternIO.h"

namespace carve::app
{

GeneratorController::GeneratorController (te::Engine& engineToUse, model::Song songModel,
                                          juce::UndoManager& um)
    : engine (engineToUse), song (std::move (songModel)), undoManager (um)
{
    song.state.addListener (this);
    refresh();   // picks the first generator, if the song opens with any
}

GeneratorController::~GeneratorController()
{
    song.state.removeListener (this);
}

// One bar of whatever the song opens in, so a 6/8 song doesn't hand every new
// pattern a bar and a third.
double GeneratorController::newPatternLengthBeats() const
{
    return song.getTimeSigAt (0.0).getBeatsPerBar();
}

void GeneratorController::showAddGeneratorMenu (juce::Rectangle<int> screenArea)
{
    juce::PopupMenu menu;
    menu.addItem (1, "4OSC (internal synth)");
    menu.addItem (3, "Sampler (choose a sample)...");
    menu.addItem (4, "Drum Kit (16 pads)");
    menu.addItem (5, "Audio track");

    const auto types = engine.getPluginManager().knownPluginList.getTypes();
    juce::Array<juce::PluginDescription> instruments;
    for (const auto& type : types)
        if (type.isInstrument)
            instruments.add (type);

    if (! instruments.isEmpty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("Plugins");
        for (int i = 0; i < instruments.size(); ++i)
            menu.addItem (100 + i, instruments[i].name + "  (" + instruments[i].pluginFormatName + ")");
    }

    menu.addSeparator();
    menu.addItem (2, "Scan / manage plugins...");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, instruments] (int result)
    {
        if (result == 1)
            addGenerator ("Synth " + juce::String (song.getNumGenerators() + 1),
                          "internal-synth", nullptr);
        else if (result == 2)
        {
            if (onManagePlugins)
                onManagePlugins();
        }
        else if (result == 3)
        {
            // The sample comes first so the generator can be named after it;
            // a cancelled chooser leaves no empty sampler behind.
            launchSampleChooser ([this] (const juce::File& sample) { addSamplerGenerator (sample); });
        }
        else if (result == 4)
        {
            // The other way round from a sampler: the kit is created empty and
            // filled a pad at a time, so there is nothing to name it after.
            addGenerator ("Drum Kit " + juce::String (song.getNumGenerators() + 1),
                          model::Generator::drumKitType, nullptr);
        }
        else if (result == 5)
        {
            // No instrument at all: its clips are the audio files placed on it.
            addGenerator ("Audio " + juce::String (song.getNumGenerators() + 1),
                          model::Generator::audioType, nullptr);
        }
        else if (result >= 100 && result < 100 + instruments.size())
        {
            const auto& description = instruments.getReference (result - 100);
            addGenerator (description.name, "plugin", &description);
        }
    });
}

model::Generator GeneratorController::addGenerator (const juce::String& name, const juce::String& type,
                                                    const juce::PluginDescription* description)
{
    undoManager.beginNewTransaction();
    auto generator = song.addGenerator (name, type, &undoManager);
    if (description != nullptr)
        generator.setPlugin (*description, &undoManager);

    // One pattern to start with, so the generator opens with something to draw
    // into rather than making the user ask for one first.
    auto pattern = generator.createPattern (&undoManager, newPatternLengthBeats());
    selectedGeneratorId = generator.getId();
    selectedPatternId = pattern.getId();
    refresh();
    fireSelectionChanged();
    return generator;
}

//==============================================================================
// Samples

// Where the last sample came from. Samples live in whatever library the user
// keeps them in, and reopening at ~/Music every time means walking back down
// to it on every pad of a kit. tracktion's PropertyStorage only takes keys
// from its own fixed SettingID enum, so this goes in the properties file the
// engine keeps beside the rest of the app's settings (see EngineSetup.h).
static constexpr const char* lastSampleDirectoryKey = "lastSampleDirectory";

juce::File GeneratorController::getSampleBrowseDirectory() const
{
    const juce::File remembered (engine.getPropertyStorage().getPropertiesFile()
                                     .getValue (lastSampleDirectoryKey));

    // A remembered folder that has since been deleted or unmounted would open
    // the chooser on nothing at all, so fall back rather than trust it.
    if (remembered.isDirectory())
        return remembered;

    return juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

void GeneratorController::rememberSampleDirectory (const juce::File& sample)
{
    engine.getPropertyStorage().getPropertiesFile()
        .setValue (lastSampleDirectoryKey, sample.getParentDirectory().getFullPathName());
}

void GeneratorController::launchSampleChooser (std::function<void (const juce::File&)> onChosen)
{
    sampleChooser = std::make_unique<juce::FileChooser> (
        "Choose a sample",
        getSampleBrowseDirectory(),
        engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());

    sampleChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                [this, onChosen = std::move (onChosen)] (const juce::FileChooser& chooser)
    {
        if (const auto sample = chooser.getResult(); sample.existsAsFile())
        {
            rememberSampleDirectory (sample);
            onChosen (sample);
        }
    });
}

void GeneratorController::addSamplerGenerator (const juce::File& sample)
{
    auto generator = addGenerator (sample.getFileNameWithoutExtension(),
                                   model::Generator::samplerType, nullptr);
    generator.setSingleSound (sample, &undoManager);
    refresh();
}

void GeneratorController::assignSample (model::Generator generator, const juce::File& sample)
{
    // A drum kit is a sampler too, but its sounds are its pads: one sample
    // stretched over the keyboard would silently delete the whole kit.
    if (! generator.isSampler() || generator.isDrumKit())
        return;

    undoManager.beginNewTransaction();

    // One sample over the whole keyboard, replacing whatever was there. The
    // generator's own name is left alone: it may well be the user's.
    generator.setSingleSound (sample, &undoManager);
    refresh();
}

void GeneratorController::chooseSampleForSelected()
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isSampler() || generator->isDrumKit())
        return;

    const auto generatorId = generator->getId();

    // The generator is looked up again inside the callback: the chooser is
    // asynchronous, and the song may have been replaced by then.
    launchSampleChooser ([this, generatorId] (const juce::File& sample)
    {
        if (auto target = song.findGenerator (generatorId))
            assignSample (*target, sample);
    });
}

bool GeneratorController::canImportAudioFiles (const juce::StringArray& files) const
{
    for (const auto& path : files)
        if (engine.getAudioFileFormatManager().canOpen (juce::File (path)))
            return true;

    return false;
}

//==============================================================================
// Drum pads

void GeneratorController::padClicked (int pad, juce::Rectangle<int> screenArea)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isDrumKit())
        return;

    const auto generatorId = generator->getId();

    // The generator is looked up again inside every callback: the chooser and
    // the menu are asynchronous, and the song may have been replaced by then.
    auto chooseSample = [this, generatorId, pad]
    {
        launchSampleChooser ([this, generatorId, pad] (const juce::File& sample)
        {
            if (auto target = song.findGenerator (generatorId))
                assignPadSample (*target, pad, sample);
        });
    };

    if (! drumkit::findSoundForPad (*generator, pad))
    {
        chooseSample();
        return;
    }

    juce::PopupMenu menu;
    menu.addItem (1, "Replace sample...");
    menu.addItem (2, "Clear pad");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, generatorId, pad, chooseSample] (int result)
    {
        if (result == 1)
            chooseSample();
        else if (result == 2)
            if (auto target = song.findGenerator (generatorId))
                clearPad (*target, pad);
    });
}

void GeneratorController::setPadSound (model::Generator& generator, int pad, const juce::File& sample)
{
    if (pad < 0 || pad >= drumkit::numPads)
        return;

    // Whatever the pad had goes: two sounds on one note would both fire, which
    // is never what assigning a sample to a pad means.
    if (auto existing = drumkit::findSoundForPad (generator, pad))
        generator.removeSound (*existing, &undoManager);

    const int note = drumkit::getNoteForPad (pad);
    auto sound = generator.addSound (sample, &undoManager);

    // Root note == the pad's note, so the sample plays back untransposed, and
    // a one-note key range, so it answers to this pad and nothing else.
    sound.setRootNote (note, &undoManager);
    sound.setKeyRange (note, note, &undoManager);
}

void GeneratorController::assignPadSample (model::Generator generator, int pad, const juce::File& sample)
{
    if (! generator.isDrumKit())
        return;

    undoManager.beginNewTransaction();
    setPadSound (generator, pad, sample);
    refresh();
}

void GeneratorController::clearPad (model::Generator generator, int pad)
{
    auto sound = drumkit::findSoundForPad (generator, pad);

    if (! sound)
        return;

    undoManager.beginNewTransaction();
    generator.removeSound (*sound, &undoManager);
    refresh();
}

void GeneratorController::padFilesDropped (int startPad, const juce::StringArray& files)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isDrumKit())
        return;

    // One transaction for the whole drop: dropping a handful of one-shots at
    // once is a single gesture, and undo should take the whole kit back.
    undoManager.beginNewTransaction();

    int pad = startPad;

    for (const auto& path : files)
    {
        if (pad >= drumkit::numPads)
            break;

        const juce::File sample (path);

        if (engine.getAudioFileFormatManager().canOpen (sample))
            setPadSound (*generator, pad++, sample);
    }

    refresh();
}

//==============================================================================
// Selection

void GeneratorController::setSong (model::Song newSong)
{
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);

    selectedGeneratorId = song.getNumGenerators() > 0 ? song.getGenerator (0).getId()
                                                      : juce::String();
    selectedPatternId.clear();
    refresh();
    fireSelectionChanged();
}

void GeneratorController::selectGenerator (const juce::String& generatorId)
{
    if (generatorId == selectedGeneratorId || ! song.findGenerator (generatorId))
        return;

    // Selecting a generator selects its first pattern unless the current
    // pattern already belongs to it; refresh does that.
    selectedGeneratorId = generatorId;
    refresh();
    fireSelectionChanged();
}

void GeneratorController::selectPattern (const juce::String& patternId)
{
    if (patternId.isEmpty())
        return;

    // refresh falls back to the first pattern if this id isn't one of the
    // selected generator's, so a stale id can't leave a bad selection.
    selectedPatternId = patternId;
    refresh();
    fireSelectionChanged();
}

std::optional<model::Generator> GeneratorController::getSelectedGenerator() const
{
    return song.findGenerator (selectedGeneratorId);
}

juce::String GeneratorController::getSelectedGeneratorId() const
{
    return selectedGeneratorId;
}

juce::String GeneratorController::getSelectedPatternId() const
{
    return selectedPatternId;
}

void GeneratorController::createPattern()
{
    auto generator = getSelectedGenerator();

    if (! generator || generator->isAudio())
        return;

    undoManager.beginNewTransaction();
    auto pattern = generator->createPattern (&undoManager, newPatternLengthBeats());
    selectedPatternId = pattern.getId();
    refresh();
    fireSelectionChanged();
}

void GeneratorController::clonePattern()
{
    auto generator = getSelectedGenerator();

    if (! generator)
        return;

    if (auto pattern = generator->findPattern (selectedPatternId))
        duplicatePattern (generator->getId(), pattern->getId(), generator->getId());
}

//==============================================================================
// The pattern menu: rename, delete, copy to another generator, MIDI in and out

void GeneratorController::showPatternMenu (juce::Rectangle<int> screenArea)
{
    auto generator = getSelectedGenerator();

    // An audio generator has no patterns at all, and the picker is disabled
    // for one -- but a stale click could still arrive while it is being
    // swapped.
    if (! generator || generator->isAudio())
        return;

    const auto generatorId = generator->getId();
    const auto pattern = generator->findPattern (selectedPatternId);

    if (! pattern)
        return;

    const auto patternId = pattern->getId();

    // Which generators the pattern could be copied to, in list order, so the
    // menu ids below index straight into this.
    juce::StringArray otherGeneratorIds;
    juce::PopupMenu copyToMenu;

    for (const auto& other : song.getGenerators())
    {
        if (other.getId() == generatorId || other.isAudio())
            continue;

        otherGeneratorIds.add (other.getId());
        copyToMenu.addItem (100 + otherGeneratorIds.size() - 1, other.getName());
    }

    juce::PopupMenu menu;
    menu.addSectionHeader (pattern->getName());
    menu.addItem (4, "Rename...");

    if (copyToMenu.getNumItems() > 0)
        menu.addSubMenu ("Copy to generator", copyToMenu);

    menu.addSeparator();
    menu.addItem (2, "Export as MIDI...");
    menu.addItem (3, pattern->isEmpty() ? juce::String ("Import MIDI...")
                                        : "Import MIDI (replaces " + pattern->getName() + ")...");

    // The last pattern stays: the piano roll and the playlist both address one
    // by id, and a generator with none to select would leave both pointing at
    // nothing.
    menu.addSeparator();
    menu.addItem (5, "Delete", generator->getNumPatterns() > 1);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (screenArea),
                        [this, generatorId, patternId, otherGeneratorIds] (int result)
    {
        if (result == 2)
            exportPatternToMidi (generatorId, patternId);
        else if (result == 3)
            importMidiIntoPattern (generatorId, patternId);
        else if (result == 4)
            renamePattern (generatorId, patternId);
        else if (result == 5)
            deletePattern (generatorId, patternId);
        else if (result >= 100 && result < 100 + otherGeneratorIds.size())
            duplicatePattern (generatorId, patternId, otherGeneratorIds[result - 100]);
    });
}

void GeneratorController::duplicatePattern (const juce::String& generatorId,
                                            const juce::String& patternId,
                                            const juce::String& destinationGeneratorId)
{
    auto source = song.findGenerator (generatorId);
    auto destination = song.findGenerator (destinationGeneratorId);

    if (! source || ! destination)
        return;

    auto pattern = source->findPattern (patternId);

    if (! pattern)
        return;

    undoManager.beginNewTransaction();
    auto copy = destination->duplicatePattern (*pattern, &undoManager);

    // Land on the copy: cloning is the middle of "make one, clone it, vary
    // it", so the next edit belongs to the new one.
    selectedGeneratorId = destination->getId();
    selectedPatternId = copy.getId();
    refresh();
    fireSelectionChanged();
}

void GeneratorController::renamePattern (const juce::String& generatorId,
                                         const juce::String& patternId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    auto pattern = generator->findPattern (patternId);

    if (! pattern)
        return;

    renameWindow = std::make_unique<juce::AlertWindow> ("Rename pattern", juce::String(),
                                                        juce::MessageBoxIconType::NoIcon);
    renameWindow->addTextEditor ("name", pattern->getName(), "Name");
    renameWindow->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    renameWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    renameWindow->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, generatorId, patternId] (int result)
    {
        // Read before the window goes, and the song looked up again: the box
        // was modal, but a reload could still have replaced the song under it.
        const auto name = result == 1 ? renameWindow->getTextEditorContents ("name").trim()
                                      : juce::String();

        // Deferred: the window is what is running this callback, and it is
        // still on screen until the message loop comes back round.
        juce::MessageManager::callAsync ([this] { renameWindow.reset(); });

        if (name.isEmpty())
            return;

        auto generator = song.findGenerator (generatorId);

        if (! generator)
            return;

        if (auto pattern = generator->findPattern (patternId))
        {
            undoManager.beginNewTransaction();
            pattern->setName (name, &undoManager);
            refresh();
            fireSelectionChanged();
        }
    }));
}

void GeneratorController::deletePattern (const juce::String& generatorId,
                                         const juce::String& patternId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    auto pattern = generator->findPattern (patternId);

    // The last one stays, for the reason the menu item is greyed out for.
    if (! pattern || generator->getNumPatterns() <= 1)
        return;

    // Deleting a pattern the playlist plays would leave clips pointing at
    // nothing, so those go with it. Undo takes the whole thing back.
    undoManager.beginNewTransaction();

    auto playlist = song.getPlaylist();

    for (const auto& clip : playlist.getClips())
        if (clip.getPatternId() == patternId)
            playlist.removeClip (clip, &undoManager);

    generator->removePattern (*pattern, &undoManager);

    // refresh's fallback picks the generator's first pattern, which is what a
    // deletion should land on -- nothing here has to name the next selection.
    selectedPatternId.clear();
    refresh();
    fireSelectionChanged();
}

void GeneratorController::exportPatternToMidi (const juce::String& generatorId,
                                               const juce::String& patternId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    auto pattern = generator->findPattern (patternId);

    if (! pattern)
        return;

    // Named after both, because a folder of files called "Ba1.mid" says
    // nothing a week later about which instrument each one was for.
    const auto suggested = juce::File::createLegalFileName (
        generator->getName() + " - " + pattern->getName() + ".mid");

    midiChooser = std::make_unique<juce::FileChooser> (
        "Export pattern as MIDI",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (suggested),
        model::midiio::fileWildcard);

    midiChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, generatorId, patternId] (const juce::FileChooser& chooser)
    {
        auto destination = chooser.getResult();

        if (destination == juce::File())
            return;

        if (destination.getFileExtension().isEmpty())
            destination = destination.withFileExtension ("mid");

        // Looked up again rather than captured: the chooser is asynchronous,
        // and the song may have been replaced while it stood open.
        auto generator = song.findGenerator (generatorId);

        if (! generator)
            return;

        auto pattern = generator->findPattern (patternId);

        if (! pattern)
            return;

        // The song's tempo and meter, not the pattern's: a pattern has neither,
        // and these are what it is being played at here.
        if (! model::midiio::writePattern (*pattern, destination, song.getTempo(),
                                           song.getTimeSigAt (0.0)))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "Export failed",
                                                         "Could not write " + destination.getFullPathName());
    });
}

void GeneratorController::importMidiIntoPattern (const juce::String& generatorId,
                                                 const juce::String& patternId)
{
    midiChooser = std::make_unique<juce::FileChooser> (
        "Import MIDI into pattern",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        model::midiio::fileWildcard);

    midiChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this, generatorId, patternId] (const juce::FileChooser& chooser)
    {
        const auto source = chooser.getResult();

        if (! source.existsAsFile())
            return;

        const auto imported = model::midiio::readFile (source);

        if (! imported)
        {
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "Import failed",
                                                         "Could not read " + source.getFileName()
                                                             + " as a MIDI file.");
            return;
        }

        auto generator = song.findGenerator (generatorId);

        if (! generator)
            return;

        auto pattern = generator->findPattern (patternId);

        if (! pattern)
            return;

        undoManager.beginNewTransaction();

        // An empty pattern holds nothing of the user's, so it takes the file's
        // name and the picker says what was imported. One with notes in it is
        // being replaced, but its name was the user's choice and stays.
        if (imported->name.isNotEmpty() && pattern->isEmpty())
            pattern->setName (imported->name, &undoManager);

        model::midiio::applyToPattern (*imported, *pattern, newPatternLengthBeats(), &undoManager);

        selectedPatternId = pattern->getId();
        refresh();
        fireSelectionChanged();
    });
}

//==============================================================================

void GeneratorController::ensureValidSelection()
{
    auto generator = song.findGenerator (selectedGeneratorId);

    // An undo or a reload can have taken the generator out from under us:
    // fall back to the first one, the way the old list view clamped its row.
    if (! generator)
    {
        generator = song.getNumGenerators() > 0 ? std::optional<model::Generator> (song.getGenerator (0))
                                                : std::nullopt;
        selectedGeneratorId = generator ? generator->getId() : juce::String();
        selectedPatternId.clear();
    }

    // The piano roll follows this id, so it must never dangle: fall back to the
    // generator's first pattern.
    if (generator && ! generator->findPattern (selectedPatternId))
        selectedPatternId = generator->getNumPatterns() > 0 ? generator->getPattern (0).getId()
                                                            : juce::String();
}

void GeneratorController::refresh()
{
    const auto generatorBefore = selectedGeneratorId;
    const auto patternBefore = selectedPatternId;

    ensureValidSelection();

    if (onPatternsChanged)
        onPatternsChanged();

    // The fallback above is a real selection change (a deleted generator, an
    // undone pattern), and the host must hear about those the same way it
    // hears about deliberate ones.
    if (generatorBefore != selectedGeneratorId || patternBefore != selectedPatternId)
        fireSelectionChanged();
}

void GeneratorController::fireSelectionChanged()
{
    if (onSelectionChanged)
        onSelectionChanged (selectedGeneratorId, selectedPatternId);
}

} // namespace carve::app
