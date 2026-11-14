#include "GeneratorController.h"

#include <algorithm>
#include <map>

#include "DrumPadGrid.h"
#include "model/MidiPatternIO.h"
#include "voice/Voicebank.h"
#include "SandboxAccess.h"
#include "AppFiles.h"
#include "TouchUi.h"

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

namespace
{
// The kinds of generator the menus offer, by submenu and in menu order:
// what each is called (and so what a new one is named: "Drum Synth 3"), a
// word on what it is, shown beside the name, its type, the name it had
// before the instruments were renamed (an old song's "Slate 3" is still a
// name the app gave it), and whether it is made empty and so opens on its
// Inst tab to be filled. The add menu offers all of them; Replace
// Instrument the ones that play patterns. The rows without a group (audio,
// reference) sit on the menu itself, under the groups.
struct GeneratorChoice
{
    int menuId;
    const char* group;
    const char* name;
    const char* hint;
    const char* type;
    const char* oldStem;
    bool opensSetup;
};

constexpr GeneratorChoice generatorChoices[]
{
    { 1,  "Synth & Keys", "Slate Synth",    "subtractive synth",     model::Generator::synthType,         "Slate",       false },
    { 14, "Synth & Keys", "Modelled Piano", "acoustic piano",        model::Generator::pianoType,         "Piano",       false },
    { 12, "Synth & Keys", "Electric Bass",  "resynthesized Jazz Bass", model::Generator::electricBassType,  "Bass",        false },
    { 17, "Synth & Keys", "Modelled Bass",  "electric bass, strings computed", model::Generator::bassModelType, "Bass Model",  false },
    { 6,  "Drums",        "Drum Synth",     "analog-style kit",      model::Generator::drumSynthType,     "Drums",       false },
    { 11, "Drums",        "Acoustic Drums", "resynthesized kit",     model::Generator::acousticDrumsType, "Drums",       false },
    { 4,  "Drums",        "Drum Sampler",   "16 pads",               model::Generator::drumKitType,       "Drum Kit",    false },
    { 10, "Drums",        "Kick Designer",  "gabber / hard kicks",   model::Generator::gabberKickType,    "Gabber Kick", true },
    { 15, "Voice",        "Modelled Voice (JP)", "Japanese singer",  model::Generator::vocalModelType,    "Vocal",       false },
    { 16, "Voice",        "Modelled Voice (EN)", "English singer",   model::Generator::vocalModelEnType,  "Vocal EN",    false },
    { 8,  "Voice",        "UTAU Singer",    "voicebank",             model::Generator::utauType,          "Singer",      true },
    { 3,  "Sampler",      "Sampler",        "one sample",            model::Generator::samplerType,       "Sampler",     true },
    { 7,  "Sampler",      "SF2 Player",     ".sf2",                  model::Generator::soundFontType,     "SF2",         true },
    { 9,  "Sampler",      "Break Slicer",   "chops a drum loop",     model::Generator::breakType,         "Break",       true },
    { 5,  nullptr,        "Audio track",    "",                      model::Generator::audioType,         "Audio",       false },
    { 13, nullptr,        "Reference track", "",                     model::Generator::referenceType,     "Reference",   false },
};

const GeneratorChoice* findChoiceForType (const juce::String& type)
{
    for (const auto& choice : generatorChoices)
        if (type == choice.type)
            return &choice;

    return nullptr;
}

// What a new generator of a choice is named: "Drum Synth 3" (an audio row
// "Audio 3").
juce::String newNameStem (const GeneratorChoice& choice)
{
    return choice.group != nullptr ? juce::String (choice.name) : juce::String (choice.oldStem);
}

// The stem of a name the app gave a generator of this type, "Drum Synth" for
// "Drum Synth 3" (or, from an older song, "Drums" for "Drums 3"); empty for
// a name the user gave it.
juce::String givenNameStem (const juce::String& name, const juce::String& type)
{
    const auto* choice = findChoiceForType (type);

    if (choice == nullptr)
        return {};

    for (const auto& stem : { newNameStem (*choice), juce::String (choice->oldStem) })
    {
        const auto rest = name.substring (stem.length());

        if (name.startsWith (stem + " ") && rest.length() > 1 && rest.substring (1).containsOnly ("0123456789"))
            return stem;
    }

    return {};
}

const GeneratorChoice* findChoice (int menuId)
{
    for (const auto& choice : generatorChoices)
        if (choice.menuId == menuId)
            return &choice;

    return nullptr;
}

bool playsPatterns (const GeneratorChoice& choice)
{
    return juce::String (choice.type) != model::Generator::audioType
            && juce::String (choice.type) != model::Generator::referenceType;
}

// The instrument plugins the menus offer: none in a build that hosts none
// (the App Store one), even if a settings file from another build lists some.
juce::Array<juce::PluginDescription> instrumentPlugins (te::Engine& engine)
{
    juce::Array<juce::PluginDescription> instruments;

   #if CARVE_PLUGIN_HOSTING
    for (const auto& type : engine.getPluginManager().knownPluginList.getTypes())
        if (type.isInstrument)
            instruments.add (type);
   #else
    juce::ignoreUnused (engine);
   #endif

    return instruments;
}

constexpr int firstPluginItem = 100;

// The instruments as submenus of `menu`: a submenu a group of the built-in
// ones, then the plugins, by maker when there are several makers. The
// current instrument (a built-in type, or a plugin) is ticked, and so is the
// submenu it is in. Plugin items are firstPluginItem + their index in
// `instruments`. The ungrouped rows are left to the caller.
void addInstrumentMenus (juce::PopupMenu& menu, bool playingOnly, const juce::Array<juce::PluginDescription>& instruments,
                         const juce::String& currentType = {}, const juce::PluginDescription* currentPlugin = nullptr)
{
    juce::StringArray groups;

    for (const auto& choice : generatorChoices)
        if (choice.group != nullptr)
            groups.addIfNotAlreadyThere (choice.group);

    for (const auto& group : groups)
    {
        juce::PopupMenu sub;
        bool holdsCurrent = false;

        for (const auto& choice : generatorChoices)
        {
            if (choice.group == nullptr || group != choice.group || (playingOnly && ! playsPatterns (choice)))
                continue;

            const bool current = currentType == choice.type;
            holdsCurrent = holdsCurrent || current;
            juce::PopupMenu::Item item (choice.name);
            item.setID (choice.menuId).setTicked (current);
            item.shortcutKeyDescription = choice.hint;   // drawn dimmed on the right
            sub.addItem (std::move (item));
        }

        menu.addSubMenu (group, sub, true, nullptr, holdsCurrent);
    }

    if (instruments.isEmpty())
        return;

    auto isCurrent = [&] (const juce::PluginDescription& d)
    {
        return currentType == "plugin" && currentPlugin != nullptr
                && currentPlugin->createIdentifierString() == d.createIdentifierString();
    };

    // By maker, each sorted by name; one maker's plugins go straight in.
    struct Caseless
    {
        bool operator() (const juce::String& a, const juce::String& b) const  { return a.compareIgnoreCase (b) < 0; }
    };

    std::map<juce::String, std::vector<int>, Caseless> byMaker;

    for (int i = 0; i < instruments.size(); ++i)
    {
        const auto maker = instruments[i].manufacturerName.trim();
        byMaker[maker.isEmpty() ? juce::String ("Other") : maker].push_back (i);
    }

    auto addPlugins = [&] (juce::PopupMenu& to, std::vector<int> indices)
    {
        std::sort (indices.begin(), indices.end(), [&] (int a, int b)
                   { return instruments[a].name.compareNatural (instruments[b].name) < 0; });

        bool any = false;

        for (auto i : indices)
        {
            to.addItem (firstPluginItem + i, instruments[i].name + "  (" + instruments[i].pluginFormatName + ")",
                        true, isCurrent (instruments[i]));
            any = any || isCurrent (instruments[i]);
        }

        return any;
    };

    juce::PopupMenu plugins;
    bool holdsCurrent = false;

    if (byMaker.size() == 1)
    {
        holdsCurrent = addPlugins (plugins, byMaker.begin()->second);
    }
    else
    {
        for (auto& [maker, indices] : byMaker)
        {
            juce::PopupMenu sub;
            const bool here = addPlugins (sub, indices);
            plugins.addSubMenu (maker, sub, true, nullptr, here);
            holdsCurrent = holdsCurrent || here;
        }
    }

    menu.addSubMenu ("Plugins", plugins, true, nullptr, holdsCurrent);
}
} // namespace

void GeneratorController::showAddGeneratorMenu (juce::Rectangle<int> screenArea)
{
    juce::PopupMenu menu;
    const auto instruments = instrumentPlugins (engine);
    addInstrumentMenus (menu, false, instruments);
    menu.addSeparator();

    for (const auto& choice : generatorChoices)
        if (choice.group == nullptr)
            menu.addItem (choice.menuId, choice.name);

   #if CARVE_PLUGIN_HOSTING
    menu.addSeparator();
    menu.addItem (2, "Scan / manage plugins...");
   #endif

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, instruments] (int result)
    {
        if (result == 2)
        {
            if (onManagePlugins)
                onManagePlugins();
        }
        else if (const auto* choice = findChoice (result))
        {
            // The ones made empty (a sampler, a font player, a singer, a
            // slicer, the kick) open on their Inst tab to be filled, from a
            // chooser or the history, without anything waiting on it. The
            // rest play as they are made; their editors are there to shape
            // them, and a singer's piano roll to give it words. An audio or
            // reference row has no instrument at all: its clips are the
            // files dropped on it.
            addGenerator (newNameStem (*choice) + " " + juce::String (song.getNumGenerators() + 1),
                          choice->type, nullptr);

            if (choice->opensSetup && onOpenInstrumentForSetup)
                onOpenInstrumentForSetup();
        }
        else if (result >= firstPluginItem && result < firstPluginItem + instruments.size())
        {
            const auto& description = instruments.getReference (result - firstPluginItem);
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
        if (const auto sample = sandbox::claimChosen (chooser.getURLResult()); sample.existsAsFile())
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
    rememberMedia (MediaKind::sample, sample);
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
    // generator's own name is left alone -- it may well be the user's --
    // unless it is still the one it was made with.
    const bool wasEmpty = generator.getSounds().empty();
    generator.setSingleSound (sample, &undoManager);
    nameAfterFirstMedia (generator, wasEmpty, sample.getFileNameWithoutExtension());
    rememberMedia (MediaKind::sample, sample);
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

//==============================================================================
// SoundFonts

static constexpr const char* lastSoundFontDirectoryKey = "lastSoundFontDirectory";

void GeneratorController::launchSoundFontChooser (std::function<void (const juce::File&)> onChosen)
{
    const juce::File remembered (engine.getPropertyStorage().getPropertiesFile()
                                     .getValue (lastSoundFontDirectoryKey));

    sampleChooser = std::make_unique<juce::FileChooser> (
        "Choose an SF2 file",
        remembered.isDirectory() ? remembered : getSampleBrowseDirectory(),
        "*.sf2");

    sampleChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                [this, onChosen = std::move (onChosen)] (const juce::FileChooser& chooser)
    {
        if (const auto soundFont = sandbox::claimChosen (chooser.getURLResult()); soundFont.existsAsFile())
        {
            engine.getPropertyStorage().getPropertiesFile()
                .setValue (lastSoundFontDirectoryKey, soundFont.getParentDirectory().getFullPathName());
            onChosen (soundFont);
        }
    });
}

void GeneratorController::addSoundFontGenerator (const juce::File& soundFont)
{
    auto generator = addGenerator (soundFont.getFileNameWithoutExtension(),
                                   model::Generator::soundFontType, nullptr);
    generator.setSoundFontFile (soundFont, &undoManager);
    rememberMedia (MediaKind::soundFont, soundFont);
    refresh();
}

void GeneratorController::assignSoundFont (model::Generator generator, const juce::File& soundFont)
{
    if (! generator.isSoundFont())
        return;

    undoManager.beginNewTransaction();
    const bool wasEmpty = generator.getSounds().empty();
    generator.setSoundFontFile (soundFont, &undoManager);
    nameAfterFirstMedia (generator, wasEmpty, soundFont.getFileNameWithoutExtension());
    rememberMedia (MediaKind::soundFont, soundFont);
    refresh();
}

void GeneratorController::chooseSoundFontForSelected()
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isSoundFont())
        return;

    const auto generatorId = generator->getId();

    launchSoundFontChooser ([this, generatorId] (const juce::File& soundFont)
    {
        if (auto target = song.findGenerator (generatorId))
            assignSoundFont (*target, soundFont);
    });
}

void GeneratorController::soundFontDroppedOnSelected (const juce::File& soundFont)
{
    if (auto generator = getSelectedGenerator(); generator && soundFont.existsAsFile())
        assignSoundFont (*generator, soundFont);
}

void GeneratorController::setSoundFontPresetForSelected (int bank, int program)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isSoundFont())
        return;

    if (generator->getSoundFontBank() == bank && generator->getSoundFontProgram() == program)
        return;

    undoManager.beginNewTransaction();
    generator->setSoundFontPreset (bank, program, &undoManager);
}

//==============================================================================
// Voicebanks

static constexpr const char* lastVoicebankDirectoryKey = "lastVoicebankDirectory";

void GeneratorController::launchVoicebankChooser (std::function<void (const juce::File&)> onChosen)
{
    const juce::File remembered (engine.getPropertyStorage().getPropertiesFile()
                                     .getValue (lastVoicebankDirectoryKey));

    // Where voicebanks usually are: OpenUtau installs them here, and someone
    // with UTAU voicebanks on a Mac most likely has OpenUtau.
    const auto openUtauSingers = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                     .getChildFile ("Library/OpenUtau/Singers");

    const auto start = remembered.isDirectory()       ? remembered
                     : openUtauSingers.isDirectory()  ? openUtauSingers
                                                      : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    sampleChooser = std::make_unique<juce::FileChooser> ("Choose a voicebank folder", start);

    sampleChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                [this, onChosen = std::move (onChosen)] (const juce::FileChooser& chooser)
    {
        if (const auto folder = sandbox::claimChosen (chooser.getURLResult()); folder.isDirectory())
        {
            engine.getPropertyStorage().getPropertiesFile()
                .setValue (lastVoicebankDirectoryKey, folder.getParentDirectory().getFullPathName());
            onChosen (folder);
        }
    });
}

void GeneratorController::addUtauGenerator (const juce::File& folder)
{
    auto generator = addGenerator (voice::Voicebank::readName (folder), model::Generator::utauType, nullptr);
    generator.setVoicebankFolder (folder, &undoManager);
    rememberMedia (MediaKind::voicebank, folder);
    refresh();
}

void GeneratorController::assignVoicebank (model::Generator generator, const juce::File& folder)
{
    if (! generator.isUtau() || ! folder.isDirectory())
        return;

    undoManager.beginNewTransaction();
    const bool wasEmpty = generator.getSounds().empty();
    generator.setVoicebankFolder (folder, &undoManager);
    nameAfterFirstMedia (generator, wasEmpty, voice::Voicebank::readName (folder));
    rememberMedia (MediaKind::voicebank, folder);
    refresh();
}

void GeneratorController::chooseVoicebankForSelected()
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isUtau())
        return;

    const auto generatorId = generator->getId();

    launchVoicebankChooser ([this, generatorId] (const juce::File& folder)
    {
        if (auto target = song.findGenerator (generatorId))
            assignVoicebank (*target, folder);
    });
}

void GeneratorController::setVoiceStyleValueForSelected (const juce::Identifier& id, double value)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isUtau())
        return;

    undoManager.beginNewTransaction();
    generator->setVoiceStyleValue (id, value, &undoManager);
}

void GeneratorController::voicebankDroppedOnSelected (const juce::File& folder)
{
    if (auto generator = getSelectedGenerator())
        assignVoicebank (*generator, folder);
}

//==============================================================================
// Break slicer

static constexpr const char* lastBreakDirectoryKey = "lastBreakDirectory";

void GeneratorController::launchBreakChooser (std::function<void (const juce::File&)> onChosen)
{
    const juce::File remembered (engine.getPropertyStorage().getPropertiesFile()
                                     .getValue (lastBreakDirectoryKey));

    sampleChooser = std::make_unique<juce::FileChooser> (
        "Choose a drum loop",
        remembered.isDirectory() ? remembered : getSampleBrowseDirectory(),
        engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());

    sampleChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                [this, onChosen = std::move (onChosen)] (const juce::FileChooser& chooser)
    {
        if (const auto loop = sandbox::claimChosen (chooser.getURLResult()); loop.existsAsFile())
        {
            engine.getPropertyStorage().getPropertiesFile()
                .setValue (lastBreakDirectoryKey, loop.getParentDirectory().getFullPathName());
            onChosen (loop);
        }
    });
}

double GeneratorController::getBreakLoopSeconds (const model::Generator& generator)
{
    const auto file = generator.getBreakFile();

    if (file != breakLengthFile || file.getLastModificationTime() != breakLengthStamp)
    {
        breakLengthFile = file;
        breakLengthStamp = file.getLastModificationTime();
        breakLengthSeconds = model::readAudioFileLengthSeconds (file);
    }

    return breakLengthSeconds;
}

void GeneratorController::assignBreak (model::Generator generator, const juce::File& loop)
{
    if (! generator.isBreak() || ! engine.getAudioFileFormatManager().canOpen (loop))
        return;

    const auto seconds = model::readAudioFileLengthSeconds (loop);

    if (seconds <= 0.0)
        return;

    const auto beatsPerBar = song.getTimeSigAt (0.0).getBeatsPerBar();

    // The patterns that played the old loop as recorded play the new one as
    // recorded too; found before the old cuts are replaced, since that is
    // what they are compared with.
    std::vector<model::Pattern> asRecorded;

    if (const auto oldSeconds = getBreakLoopSeconds (generator); oldSeconds > 0.0)
        for (const auto& pattern : generator.getPatterns())
            if (generator.playsBreakAsRecorded (pattern, oldSeconds, beatsPerBar))
                asRecorded.push_back (pattern);

    undoManager.beginNewTransaction();

    const bool wasEmpty = generator.getSounds().empty();
    generator.setBreakFile (loop, seconds, beatsPerBar, &undoManager);
    nameAfterFirstMedia (generator, wasEmpty, loop.getFileNameWithoutExtension());
    rememberMedia (MediaKind::breakLoop, loop);

    // Otherwise the loop as recorded goes in the pattern the slicer was made
    // with, while that one is still empty, or in a new one beside the user's
    // own. Either way the first thing a loaded break does is sound like
    // itself.
    if (asRecorded.empty())
        for (const auto& pattern : generator.getPatterns())
            if (pattern.getId() == selectedPatternId && pattern.isEmpty())
                asRecorded.push_back (pattern);

    if (asRecorded.empty())
        selectedPatternId = generator.createBreakPattern (seconds, beatsPerBar, &undoManager).getId();

    for (auto& pattern : asRecorded)
        generator.fillBreakAsRecorded (pattern, seconds, beatsPerBar, &undoManager);

    selectedGeneratorId = generator.getId();
    refresh();
    fireSelectionChanged();
}

void GeneratorController::chooseBreakForSelected()
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isBreak())
        return;

    const auto generatorId = generator->getId();

    launchBreakChooser ([this, generatorId] (const juce::File& loop)
    {
        if (auto target = song.findGenerator (generatorId))
            assignBreak (*target, loop);
    });
}

void GeneratorController::breakDroppedOnSelected (const juce::File& loop)
{
    if (auto generator = getSelectedGenerator())
        assignBreak (*generator, loop);
}

void GeneratorController::editBreak (const std::function<void (model::Generator&, double)>& edit)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isBreak())
        return;

    const auto seconds = getBreakLoopSeconds (*generator);
    const auto beatsPerBar = song.getTimeSigAt (0.0).getBeatsPerBar();

    std::vector<model::Pattern> asRecorded;

    for (const auto& pattern : generator->getPatterns())
        if (generator->playsBreakAsRecorded (pattern, seconds, beatsPerBar))
            asRecorded.push_back (pattern);

    undoManager.beginNewTransaction();
    edit (*generator, seconds);

    for (auto& pattern : asRecorded)
        generator->fillBreakAsRecorded (pattern, seconds, beatsPerBar, &undoManager);
}

void GeneratorController::setBreakCutsForSelected (const std::vector<double>& cuts)
{
    editBreak ([&] (model::Generator& generator, double seconds)
    {
        generator.setBreakSlices (cuts, seconds, &undoManager);
    });
}

void GeneratorController::moveBreakCutForSelected (int index, double seconds)
{
    editBreak ([&] (model::Generator& generator, double loopSeconds)
    {
        generator.moveBreakSlice (index, seconds, loopSeconds, &undoManager);
    });
}

void GeneratorController::addBreakCutForSelected (double seconds)
{
    editBreak ([&] (model::Generator& generator, double loopSeconds)
    {
        generator.insertBreakSlice (seconds, loopSeconds, &undoManager);
    });
}

void GeneratorController::removeBreakCutForSelected (int index)
{
    editBreak ([&] (model::Generator& generator, double)
    {
        generator.removeBreakSlice (index, &undoManager);
    });
}

void GeneratorController::setBreakBarsForSelected (int bars)
{
    if (auto generator = getSelectedGenerator(); ! generator || generator->getBreakBars() == bars)
        return;

    editBreak ([&] (model::Generator& generator, double)
    {
        generator.setBreakBars (bars, &undoManager);
    });
}

void GeneratorController::setBreakRepitchForSelected (bool repitch)
{
    if (auto generator = getSelectedGenerator(); generator && generator->isBreak()
                                                 && generator->isBreakRepitched() != repitch)
    {
        undoManager.beginNewTransaction();
        generator->setBreakRepitched (repitch, &undoManager);
    }
}

//==============================================================================
// History

namespace
{
    const char* recentKeyFor (MediaKind kind)
    {
        switch (kind)
        {
            case MediaKind::sample:     return "recentSamples";
            case MediaKind::soundFont:  return "recentSoundFonts";
            case MediaKind::voicebank:  return "recentVoicebanks";
            case MediaKind::breakLoop:  return "recentBreaks";
        }

        return "recentSamples";
    }

    constexpr int maxRecentMedia = 10;
}

void GeneratorController::rememberMedia (MediaKind kind, const juce::File& file)
{
    auto& properties = engine.getPropertyStorage().getPropertiesFile();

    juce::RecentlyOpenedFilesList recent;
    recent.setMaxNumberOfItems (maxRecentMedia);
    recent.restoreFromString (properties.getValue (recentKeyFor (kind)));
    recent.addFile (file);
    properties.setValue (recentKeyFor (kind), recent.toString());
}

std::vector<RecentMediaList::Item> GeneratorController::getRecentMedia (MediaKind kind) const
{
    juce::RecentlyOpenedFilesList recent;
    recent.setMaxNumberOfItems (maxRecentMedia);
    recent.restoreFromString (engine.getPropertyStorage().getPropertiesFile().getValue (recentKeyFor (kind)));

    std::vector<RecentMediaList::Item> items;

    for (int i = 0; i < recent.getNumFiles(); ++i)
    {
        const auto file = recent.getFile (i);

        if (kind == MediaKind::voicebank ? ! file.isDirectory() : ! file.existsAsFile())
            continue;   // moved or deleted since: nothing to load

        items.push_back ({ file, kind == MediaKind::voicebank ? voice::Voicebank::readName (file)
                                                              : file.getFileName() });
    }

    return items;
}

void GeneratorController::recentMediaChosen (MediaKind kind, const juce::File& file)
{
    auto generator = getSelectedGenerator();

    if (! generator)
        return;

    switch (kind)
    {
        case MediaKind::sample:     assignSample (*generator, file); break;
        case MediaKind::soundFont:  assignSoundFont (*generator, file); break;
        case MediaKind::voicebank:  assignVoicebank (*generator, file); break;
        case MediaKind::breakLoop:  assignBreak (*generator, file); break;
    }
}

void GeneratorController::nameAfterFirstMedia (model::Generator& generator, bool wasEmpty, const juce::String& newName)
{
    if (! wasEmpty || newName.isEmpty())
        return;

    const auto name = generator.getName();

    if (givenNameStem (name, generator.getType()).isEmpty())
        return;

    // Its first pattern was named from the placeholder too ("Si1" for
    // "Singer 1"); while that one is still empty and still so named, it
    // follows the generator's new name the way it would have been named
    // from it. A pattern with notes in it is the user's by now.
    for (auto pattern : generator.getPatterns())
        if (pattern.getNumNotes() == 0
             && pattern.getName() == model::patternStemForGenerator (name) + "1")
            pattern.setName (model::patternStemForGenerator (newName) + "1", &undoManager);

    generator.setName (newName, &undoManager);
}

bool GeneratorController::canImportAudioFiles (const juce::StringArray& files) const
{
    for (const auto& path : files)
        if (engine.getAudioFileFormatManager().canOpen (juce::File (path)))
            return true;

    return false;
}

void GeneratorController::sampleFilesDropped (const juce::StringArray& files)
{
    auto generator = getSelectedGenerator();

    if (! generator || ! generator->isSampler() || generator->isDrumKit())
        return;

    for (const auto& path : files)
    {
        if (const juce::File sample (path); engine.getAudioFileFormatManager().canOpen (sample))
        {
            assignSample (*generator, sample);
            return;
        }
    }
}

void GeneratorController::loadSampleIntoSelected (const juce::File& sample)
{
    if (! engine.getAudioFileFormatManager().canOpen (sample))
        return;

    auto generator = getSelectedGenerator();

    // A slicer takes the file as its loop.
    if (generator && generator->isBreak())
    {
        assignBreak (*generator, sample);
        return;
    }

    if (generator && generator->isDrumKit())
    {
        // The first pad with nothing on it: filling a kit from the browser
        // is one double-click per pad, in pad order. A full kit takes
        // nothing -- replacing a pad by accident is what the pad menu is for.
        for (int pad = 0; pad < drumkit::numPads; ++pad)
        {
            if (! drumkit::findSoundForPad (*generator, pad))
            {
                assignPadSample (*generator, pad, sample);
                return;
            }
        }

        return;
    }

    if (generator && generator->isSampler())
    {
        assignSample (*generator, sample);
        return;
    }

    addSamplerGenerator (sample);
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
    lastPatternByGenerator.clear();   // ids from another song mean nothing here
    refresh();
    fireSelectionChanged();
}

void GeneratorController::selectGenerator (const juce::String& generatorId)
{
    if (generatorId == selectedGeneratorId || ! song.findGenerator (generatorId))
        return;

    // Selecting a generator selects the pattern it was last left on (its
    // first, the first time) unless the current pattern already belongs to
    // it; refresh does that.
    selectedGeneratorId = generatorId;
    refresh();
    fireSelectionChanged();
}

std::vector<GeneratorController::CurrentPattern> GeneratorController::getCurrentPatterns() const
{
    std::vector<CurrentPattern> current;

    for (const auto& generator : song.getGenerators())
    {
        if (generator.getNumPatterns() == 0)
            continue;

        // The selected generator's pattern is read from the selection itself:
        // the map is only written by ensureValidSelection, and a caller
        // reached from a selection change wants what was just picked.
        auto patternId = generator.getId() == selectedGeneratorId ? selectedPatternId : juce::String();

        if (patternId.isEmpty())
            if (const auto remembered = lastPatternByGenerator.find (generator.getId());
                remembered != lastPatternByGenerator.end() && generator.findPattern (remembered->second))
                patternId = remembered->second;

        if (patternId.isEmpty())
            patternId = generator.getPattern (0).getId();

        current.push_back ({ generator.getId(), patternId });
    }

    return current;
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

bool GeneratorController::capturePattern (const std::vector<model::capture::PlayedNote>& played)
{
    // A phrase is over once nothing has sounded for this many bars, which is
    // what tells the last idea from the one before it.
    constexpr double gapBars = 2.0;

    auto generator = getSelectedGenerator();

    if (! generator || generator->isAudio() || played.empty())
        return false;

    const auto beatsPerBar = newPatternLengthBeats();
    const auto captured = model::capture::toPattern (model::capture::lastPhrase (played, gapBars * beatsPerBar),
                                                     beatsPerBar);

    undoManager.beginNewTransaction ("Capture");
    auto pattern = generator->addPattern (generator->makeUniquePatternName ("Capture"),
                                          captured.lengthBeats, &undoManager);

    for (const auto& note : captured.notes)
        pattern.addNote (note.startBeat, note.lengthBeats, note.pitch, note.velocity, &undoManager);

    selectPattern (pattern.getId());
    return true;
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

    forThisPlatform (menu).showMenuAsync (juce::PopupMenu::Options()
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

void GeneratorController::showGeneratorMenu (const juce::String& generatorId,
                                             juce::Rectangle<int> screenArea)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader (generator->getName());
    menu.addItem (1, "Rename...");
    menu.addItem (3, "Duplicate");

    // Every instrument the add menu offers, the one it has now ticked: the
    // patterns, clips, effects and mixer stay, and only what plays the
    // notes changes. An audio row plays files, not notes, so it has none.
    juce::Array<juce::PluginDescription> instruments;

    if (generator->canReplaceInstrument())
    {
        juce::PopupMenu replaceMenu;
        instruments = instrumentPlugins (engine);
        const auto current = generator->getPluginDescription();
        addInstrumentMenus (replaceMenu, true, instruments, generator->getType(),
                            current ? &*current : nullptr);

        menu.addSubMenu ("Replace Instrument", replaceMenu);
    }

    menu.addSeparator();
    menu.addItem (2, "Delete");

    // Looked up again inside: the menu is asynchronous, and the song may have
    // been replaced by then.
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, generatorId, instruments] (int result)
    {
        if (result == 1)
            renameGenerator (generatorId);
        else if (result == 2)
            deleteGenerator (generatorId);
        else if (result == 3)
            duplicateGenerator (generatorId);
        else if (const auto* choice = findChoice (result); choice != nullptr && playsPatterns (*choice))
            replaceInstrument (generatorId, choice->type, newNameStem (*choice), nullptr, choice->opensSetup);
        else if (result >= firstPluginItem && result < firstPluginItem + instruments.size())
        {
            const auto& description = instruments.getReference (result - firstPluginItem);
            replaceInstrument (generatorId, "plugin", description.name, &description, false);
        }
    });
}

void GeneratorController::duplicateGenerator (const juce::String& generatorId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    // The knobs turned since the last save are only in the live plugins; the
    // copy is made from the song, so they go into it first.
    if (onCaptureLivePluginState)
        onCaptureLivePluginState();

    undoManager.beginNewTransaction();
    auto copy = song.duplicateGenerator (*generator, &undoManager);

    // The copy is selected, on the pattern its source was showing.
    selectedGeneratorId = copy.getId();
    selectedPatternId = {};

    if (auto shown = lastPatternByGenerator.find (generatorId); shown != lastPatternByGenerator.end())
        for (int i = 0; i < generator->getNumPatterns() && i < copy.getNumPatterns(); ++i)
            if (generator->getPattern (i).getId() == shown->second)
                selectedPatternId = copy.getPattern (i).getId();

    refresh();
    fireSelectionChanged();
}

void GeneratorController::replaceInstrument (const juce::String& generatorId, const juce::String& type,
                                             const juce::String& stem, const juce::PluginDescription* description,
                                             bool opensSetup)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator || ! generator->canReplaceInstrument())
        return;

    // A name the app gave it ("Slate Synth 3", or a plugin's own name) goes with
    // the instrument it named: "Piano 3". One the user gave it stays.
    const auto oldStem = [&]() -> juce::String
    {
        if (generator->getType() == "plugin")
            if (auto current = generator->getPluginDescription())
                return current->name;

        return givenNameStem (generator->getName(), generator->getType());
    }();

    const auto name = generator->getName();
    juce::String newName;

    if (oldStem.isNotEmpty() && name.startsWith (oldStem))
    {
        const auto rest = name.substring (oldStem.length());

        if (rest.isEmpty() || (rest.startsWithChar (' ') && rest.substring (1).containsOnly ("0123456789")
                                && rest.length() > 1))
            newName = stem + rest;
    }

    // The old instrument's knobs into the song first, so an undo brings it
    // back as it was rather than as it was last saved.
    if (onCaptureLivePluginState)
        onCaptureLivePluginState();

    undoManager.beginNewTransaction();
    generator->replaceInstrument (type, description, &undoManager);

    if (newName.isNotEmpty() && newName != name)
        generator->setName (newName, &undoManager);

    selectedGeneratorId = generatorId;
    refresh();
    fireSelectionChanged();

    if (opensSetup && onOpenInstrumentForSetup)
        onOpenInstrumentForSetup();
}

void GeneratorController::renameGenerator (const juce::String& generatorId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    renameWindow = std::make_unique<juce::AlertWindow> ("Rename generator", juce::String(),
                                                        juce::MessageBoxIconType::NoIcon);
    renameWindow->addTextEditor ("name", generator->getName(), "Name");
    renameWindow->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    renameWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    renameWindow->enterModalState (true, juce::ModalCallbackFunction::create (
        [this, generatorId] (int result)
    {
        // Read before the window goes; see renamePattern for the shape.
        const auto name = result == 1 ? renameWindow->getTextEditorContents ("name").trim()
                                      : juce::String();

        juce::MessageManager::callAsync ([this] { renameWindow.reset(); });

        if (name.isEmpty())
            return;

        if (auto target = song.findGenerator (generatorId))
        {
            undoManager.beginNewTransaction();
            target->setName (name, &undoManager);
        }
    }));
}

void GeneratorController::deleteGenerator (const juce::String& generatorId)
{
    auto generator = song.findGenerator (generatorId);

    if (! generator)
        return;

    // No confirmation: it is one press of cmd-Z away, and the model takes
    // the generator's clips and sidechains with it so nothing dangles.
    undoManager.beginNewTransaction();
    song.removeGenerator (*generator, &undoManager);
    refresh();   // ensureValidSelection moves the selection off it
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

        auto current = song.findGenerator (generatorId);

        if (! current)
            return;

        if (auto currentPattern = current->findPattern (patternId))
        {
            undoManager.beginNewTransaction();
            currentPattern->setName (name, &undoManager);
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
    const auto name = generator->getName() + " - " + pattern->getName();
    const auto suggested = juce::File::createLegalFileName (name + ".mid");

    auto write = [this, generatorId, patternId] (juce::File destination)
    {
        if (destination == juce::File())
            return false;

        if (destination.getFileExtension().isEmpty())
            destination = destination.withFileExtension ("mid");

        // Looked up again rather than captured: the chooser is asynchronous,
        // and the song may have been replaced while it stood open.
        auto current = song.findGenerator (generatorId);

        if (! current)
            return false;

        auto currentPattern = current->findPattern (patternId);

        if (! currentPattern)
            return false;

        // The song's tempo and meter, not the pattern's: a pattern has neither,
        // and these are what it is being played at here.
        if (! model::midiio::writePattern (*currentPattern, destination, song.getTempo(),
                                           song.getTimeSigAt (0.0)))
        {
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                         "Export failed",
                                                         "Could not write " + appfiles::describe (destination));
            return false;
        }

        return true;
    };

    // The iPad writes into the app's own folder and says where (AppFiles.h).
    if (appfiles::writesIntoDocuments)
    {
        const auto destination = appfiles::freshChild (appfiles::exports(), name, ".mid");

        if (write (destination))
            juce::NativeMessageBox::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Exported",
                                                         "Saved to " + appfiles::describe (destination));
        return;
    }

    midiChooser = std::make_unique<juce::FileChooser> (
        "Export pattern as MIDI",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (suggested),
        model::midiio::fileWildcard);

    midiChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [write] (const juce::FileChooser& chooser) { write (chooser.getResult()); });
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
        const auto source = sandbox::claimChosen (chooser.getURLResult());

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

    // The piano roll follows this id, so it must never dangle: fall back to
    // the pattern this generator was last left on, then to its first.
    if (generator && ! generator->findPattern (selectedPatternId))
    {
        selectedPatternId.clear();

        if (const auto remembered = lastPatternByGenerator.find (generator->getId());
            remembered != lastPatternByGenerator.end() && generator->findPattern (remembered->second))
            selectedPatternId = remembered->second;
        else if (generator->getNumPatterns() > 0)
            selectedPatternId = generator->getPattern (0).getId();
    }

    // Every path that changes the selection comes through here before it is
    // announced, so this is the one place the generator's pattern is noted.
    if (generator && selectedPatternId.isNotEmpty())
        lastPatternByGenerator[generator->getId()] = selectedPatternId;
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
