#include "GeneratorPanel.h"

namespace carve::app
{

//==============================================================================
// PatternSlotGrid

namespace
{
    // One row per bank, nine pads per row, with a narrow gutter on the left for
    // the bank letter so each pad only has to fit a single digit.
    constexpr int slotRowHeight = 20;
    constexpr int slotGap = 2;
    constexpr int slotGutterWidth = 14;
} // namespace

PatternSlotGrid::PatternSlotGrid()
{
    clearSlots();
}

int PatternSlotGrid::getPreferredHeight()
{
    return model::PatternSlot::numBanks * slotRowHeight
            + (model::PatternSlot::numBanks - 1) * slotGap;
}

void PatternSlotGrid::clearSlots()
{
    slotStates.fill (SlotState::unused);
    selectedSlot.reset();
}

void PatternSlotGrid::setSlotState (const model::PatternSlot& slot, SlotState state)
{
    if (slot.isValid())
        slotStates[(size_t) slot.toFlatIndex()] = state;
}

void PatternSlotGrid::setSelectedSlot (std::optional<model::PatternSlot> slot)
{
    selectedSlot = slot;
}

juce::Rectangle<int> PatternSlotGrid::getSlotBounds (const model::PatternSlot& slot) const
{
    const int usable = getWidth() - slotGutterWidth
                        - (model::PatternSlot::slotsPerBank - 1) * slotGap;
    const int cellWidth = juce::jmax (8, usable / model::PatternSlot::slotsPerBank);

    return { slotGutterWidth + slot.index * (cellWidth + slotGap),
             slot.bank * (slotRowHeight + slotGap),
             cellWidth, slotRowHeight };
}

std::optional<model::PatternSlot> PatternSlotGrid::getSlotAt (juce::Point<int> position) const
{
    for (int i = 0; i < model::PatternSlot::numSlots; ++i)
    {
        const auto slot = model::PatternSlot::fromFlatIndex (i);
        if (getSlotBounds (slot).contains (position))
            return slot;
    }
    return std::nullopt;
}

void PatternSlotGrid::paint (juce::Graphics& g)
{
    g.setFont (11.0f);

    for (int i = 0; i < model::PatternSlot::numSlots; ++i)
    {
        const auto slot = model::PatternSlot::fromFlatIndex (i);
        const auto bounds = getSlotBounds (slot).toFloat();
        const auto state = slotStates[(size_t) i];
        const bool isSelected = selectedSlot.has_value() && *selectedSlot == slot;

        if (slot.index == 0)
        {
            g.setColour (juce::Colour (0xff9a9aa4));
            g.drawText (juce::String::charToString ((juce::juce_wchar) ('A' + slot.bank)),
                        0, (int) bounds.getY(), slotGutterWidth - 3, slotRowHeight,
                        juce::Justification::centredRight);
        }

        // Filled = the slot has notes, outlined = materialised but still empty,
        // flat = never used. The user can tell at a glance where the music is.
        switch (state)
        {
            case SlotState::hasNotes:  g.setColour (juce::Colour (0xffe08a3c)); break;
            case SlotState::empty:     g.setColour (juce::Colour (0xff3a3a40)); break;
            case SlotState::unused:    g.setColour (juce::Colour (0xff2b2b30)); break;
        }
        g.fillRoundedRectangle (bounds, 3.0f);

        if (isSelected)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.5f);
        }

        g.setColour (state == SlotState::hasNotes ? juce::Colours::black
                                                  : juce::Colour (0xff8a8a94));
        g.drawText (juce::String (slot.index + 1), getSlotBounds (slot),
                    juce::Justification::centred);
    }
}

void PatternSlotGrid::mouseDown (const juce::MouseEvent& e)
{
    if (auto slot = getSlotAt (e.getPosition()))
        if (onSlotClicked)
            onSlotClicked (*slot);
}

//==============================================================================
// GeneratorPanel

GeneratorPanel::GeneratorPanel (te::Engine& engineToUse, model::Song songModel, juce::UndoManager& um)
    : engine (engineToUse), song (std::move (songModel)), undoManager (um)
{
    generatorList.setModel (this);
    generatorList.setRowHeight (26);

    addGeneratorButton.onClick = [this] { showAddGeneratorMenu(); };

    instrumentButton.onClick = [this]
    {
        auto generator = getSelectedGenerator();

        if (! generator)
            return;

        // A sampler has no plugin editor to open: the sample it plays is the
        // only thing there is to choose, so the button becomes that chooser.
        if (generator->isSampler())
        {
            const auto generatorId = generator->getId();

            launchSampleChooser ([this, generatorId] (const juce::File& sample)
            {
                if (auto target = song.findGenerator (generatorId))
                    assignSample (*target, sample);
            });
            return;
        }

        if (onOpenPluginEditor)
            onOpenPluginEditor (generator->getId());
    };

    editPatternButton.onClick = [this]
    {
        if (onOpenPatternEditor)
            onOpenPatternEditor();
    };

    patternHeader.setText ("Patterns", juce::dontSendNotification);
    patternHeader.setJustificationType (juce::Justification::centredLeft);

    slotGrid.onSlotClicked = [this] (model::PatternSlot slot) { slotClicked (slot); };

    padGrid.onPadClicked = [this] (int pad) { padClicked (pad); };
    padGrid.onFilesDropped = [this] (int pad, const juce::StringArray& files) { padFilesDropped (pad, files); };
    padGrid.isInterestedInFiles = [this] (const juce::StringArray& files) { return isInterestedInFileDrag (files); };

    patternBox.onChange = [this]
    {
        if (isRefreshing)
            return;

        const int index = patternBox.getSelectedItemIndex();
        auto generator = getSelectedGenerator();
        if (! generator || index < 0)
            return;

        const auto others = generator->getUnslottedPatterns();
        if (index < (int) others.size())
        {
            const auto previousId = selectedPatternId;
            undoManager.beginNewTransaction();   // discardUntouchedPattern may edit
            selectedPatternId = others[(size_t) index].getId();
            slotGrid.setSelectedSlot (std::nullopt);
            slotGrid.repaint();
            fireSelectionChanged();
            discardUntouchedPattern (previousId, generator->getId());
        }
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &generatorList, &addGeneratorButton, &instrumentButton,
             &editPatternButton, &patternHeader, &slotGrid, &patternBox })
        addAndMakeVisible (c);

    // Only a drum kit has pads; refresh below shows the grid if one is selected.
    addChildComponent (padGrid);

    song.state.addListener (this);
    refresh();

    if (song.getNumGenerators() > 0)
        generatorList.selectRow (0);
}

GeneratorPanel::~GeneratorPanel()
{
    song.state.removeListener (this);
    generatorList.setModel (nullptr);
}

void GeneratorPanel::showAddGeneratorMenu()
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

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (addGeneratorButton),
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

model::Generator GeneratorPanel::addGenerator (const juce::String& name, const juce::String& type,
                                              const juce::PluginDescription* description)
{
    undoManager.beginNewTransaction();
    auto generator = song.addGenerator (name, type, &undoManager);
    if (description != nullptr)
        generator.setPlugin (*description, &undoManager);

    // Only slot A1 is materialised: the rest of the grid stays virtual until
    // it is used, but the generator still opens with something to draw into.
    auto pattern = generator.getOrCreatePatternInSlot ({ 0, 0 }, &undoManager);
    selectedPatternId = pattern.getId();
    refresh();
    generatorList.selectRow (song.getNumGenerators() - 1);
    return generator;
}

//==============================================================================
// Samples

void GeneratorPanel::launchSampleChooser (std::function<void (const juce::File&)> onChosen)
{
    sampleChooser = std::make_unique<juce::FileChooser> (
        "Choose a sample",
        juce::File::getSpecialLocation (juce::File::userMusicDirectory),
        engine.getAudioFileFormatManager().readFormatManager.getWildcardForAllFormats());

    sampleChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectFiles,
                                [onChosen = std::move (onChosen)] (const juce::FileChooser& chooser)
    {
        if (const auto sample = chooser.getResult(); sample.existsAsFile())
            onChosen (sample);
    });
}

void GeneratorPanel::addSamplerGenerator (const juce::File& sample)
{
    auto generator = addGenerator (sample.getFileNameWithoutExtension(),
                                   model::Generator::samplerType, nullptr);
    generator.setSingleSound (sample, &undoManager);
    refresh();
}

void GeneratorPanel::assignSample (model::Generator generator, const juce::File& sample)
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

//==============================================================================
// Drum pads

void GeneratorPanel::padClicked (int pad)
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

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (padGrid)
                            .withTargetScreenArea (padGrid.localAreaToGlobal (padGrid.getPadBounds (pad))),
                        [this, generatorId, pad, chooseSample] (int result)
    {
        if (result == 1)
            chooseSample();
        else if (result == 2)
            if (auto target = song.findGenerator (generatorId))
                clearPad (*target, pad);
    });
}

void GeneratorPanel::setPadSound (model::Generator& generator, int pad, const juce::File& sample)
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

void GeneratorPanel::assignPadSample (model::Generator generator, int pad, const juce::File& sample)
{
    if (! generator.isDrumKit())
        return;

    undoManager.beginNewTransaction();
    setPadSound (generator, pad, sample);
    refresh();
}

void GeneratorPanel::clearPad (model::Generator generator, int pad)
{
    auto sound = drumkit::findSoundForPad (generator, pad);

    if (! sound)
        return;

    undoManager.beginNewTransaction();
    generator.removeSound (*sound, &undoManager);
    refresh();
}

void GeneratorPanel::padFilesDropped (int startPad, const juce::StringArray& files)
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

void GeneratorPanel::assignToFirstFreePad (model::Generator generator, const juce::File& sample)
{
    for (int pad = 0; pad < drumkit::numPads; ++pad)
        if (! drumkit::findSoundForPad (generator, pad))
        {
            assignPadSample (generator, pad, sample);
            return;
        }
}

//==============================================================================

std::optional<model::Generator> GeneratorPanel::getGeneratorAt (juce::Point<int> position) const
{
    if (! generatorList.getBounds().contains (position))
        return std::nullopt;

    const auto row = generatorList.getRowContainingPosition (position.x - generatorList.getX(),
                                                             position.y - generatorList.getY());

    if (row >= 0 && row < song.getNumGenerators())
        return song.getGenerator (row);

    return std::nullopt;
}

bool GeneratorPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (engine.getAudioFileFormatManager().canOpen (juce::File (path)))
            return true;

    return false;
}

void GeneratorPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    for (const auto& path : files)
    {
        const juce::File sample (path);

        if (! engine.getAudioFileFormatManager().canOpen (sample))
            continue;

        // Dropping onto a sampler swaps its sample; onto a drum kit it fills
        // the next free pad, because swapping would throw the kit away.
        // Anywhere else -- another generator, or the empty space below the
        // list -- makes a new sampler.
        auto generator = getGeneratorAt ({ x, y });

        if (generator && generator->isDrumKit())
            assignToFirstFreePad (*generator, sample);
        else if (generator && generator->isSampler())
            assignSample (*generator, sample);
        else
            addSamplerGenerator (sample);
    }
}

void GeneratorPanel::setSong (model::Song newSong)
{
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);
    selectedPatternId.clear();
    refresh();
    generatorList.selectRow (song.getNumGenerators() > 0 ? 0 : -1);
}

void GeneratorPanel::selectGenerator (const juce::String& generatorId)
{
    for (int i = 0; i < song.getNumGenerators(); ++i)
        if (song.getGenerator (i).getId() == generatorId)
        {
            generatorList.selectRow (i);
            return;
        }
}

void GeneratorPanel::selectPattern (const juce::String& patternId)
{
    if (patternId.isEmpty())
        return;

    // refresh falls back to the first pattern if this id isn't one of the
    // selected generator's, so a stale id can't leave a bad selection.
    selectedPatternId = patternId;
    refresh();
    fireSelectionChanged();
}

std::optional<model::Generator> GeneratorPanel::getSelectedGenerator() const
{
    const int row = generatorList.getSelectedRow();
    if (row >= 0 && row < song.getNumGenerators())
        return song.getGenerator (row);
    return std::nullopt;
}

void GeneratorPanel::slotClicked (model::PatternSlot slot)
{
    auto generator = getSelectedGenerator();
    if (! generator)
        return;

    const auto previousId = selectedPatternId;

    undoManager.beginNewTransaction();

    // The slot is materialised on being picked rather than on the first note:
    // the piano roll and the playlist address patterns by id, so a picked slot
    // has to be something real. Slots that were only browsed past are dropped
    // again below, which is what keeps the saved file free of empty nodes.
    auto pattern = generator->getOrCreatePatternInSlot (slot, &undoManager);
    selectedPatternId = pattern.getId();

    refresh();
    fireSelectionChanged();
    discardUntouchedPattern (previousId, generator->getId());
}

void GeneratorPanel::discardUntouchedPattern (const juce::String& patternId,
                                              const juce::String& generatorId)
{
    if (patternId.isEmpty() || patternId == selectedPatternId)
        return;

    auto generator = song.findGenerator (generatorId);
    if (! generator)
        return;

    auto pattern = generator->findPattern (patternId);

    // Only a slot that was materialised and then left exactly as it came:
    // no notes, still called after its slot, and not placed on the playlist.
    if (! pattern || ! pattern->getSlot() || ! pattern->isEmpty()
         || ! pattern->hasDefaultSlotName() || song.isPatternUsedInPlaylist (*pattern))
        return;

    generator->removePattern (*pattern, &undoManager);
    refresh();
}

void GeneratorPanel::ensureValidPatternSelection()
{
    auto generator = getSelectedGenerator();
    if (! generator || generator->findPattern (selectedPatternId))
        return;

    // The piano roll follows this id, so it must never dangle: fall back to the
    // generator's first pattern (its A1 slot, for anything this app created).
    selectedPatternId = generator->getNumPatterns() > 0 ? generator->getPattern (0).getId()
                                                        : juce::String();
}

juce::String GeneratorPanel::getSelectedGeneratorId() const
{
    const int row = generatorList.getSelectedRow();
    if (row >= 0 && row < song.getNumGenerators())
        return song.getGenerator (row).getId();
    return {};
}

juce::String GeneratorPanel::getSelectedPatternId() const
{
    return selectedPatternId;
}

int GeneratorPanel::getNumRows()
{
    return song.getNumGenerators();
}

void GeneratorPanel::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (selected)
        g.fillAll (findColour (juce::TextEditor::highlightColourId));

    if (row < song.getNumGenerators())
    {
        g.setColour (findColour (juce::Label::textColourId));
        g.setFont (14.0f);
        g.drawText (song.getGenerator (row).getName(),
                    8, 0, width - 12, height, juce::Justification::centredLeft);
    }
}

void GeneratorPanel::listBoxItemDoubleClicked (int, const juce::MouseEvent&)
{
    if (onOpenPatternEditor)
        onOpenPatternEditor();
}

void GeneratorPanel::selectedRowsChanged (int)
{
    if (isRefreshing)
        return;

    // Selecting a generator selects its first pattern unless the current
    // pattern already belongs to it; refresh does that.
    refresh();
    fireSelectionChanged();
}

void GeneratorPanel::refresh()
{
    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    ensureValidPatternSelection();
    generatorList.updateContent();
    generatorList.repaint();
    rebuildSlotGrid();
    rebuildPadGrid();
    rebuildPatternBox();
}

void GeneratorPanel::rebuildPadGrid()
{
    auto generator = getSelectedGenerator();
    const bool isDrumKit = generator && generator->isDrumKit();

    if (isDrumKit)
    {
        // Which pads the pattern being edited plays, so the kit and the piano
        // roll line up without opening the roll. A pattern holds few enough
        // notes that walking them once per refresh costs nothing.
        std::array<bool, (size_t) drumkit::numPads> used {};

        if (auto pattern = generator->findPattern (selectedPatternId))
            for (const auto& note : pattern->getNotes())
                if (auto pad = drumkit::getPadForNote (note.getPitch()))
                    used[(size_t) *pad] = true;

        for (int pad = 0; pad < drumkit::numPads; ++pad)
        {
            const auto sound = drumkit::findSoundForPad (*generator, pad);
            padGrid.setPadState (pad, sound ? sound->getName() : juce::String(),
                                 used[(size_t) pad]);
        }
    }
    else
    {
        padGrid.clearPads();
    }

    // The instrument button is the plain sampler's "one sample across the
    // keyboard" chooser, which is exactly what a kit must never be given: its
    // pads are the only way samples get in.
    if (isDrumKit != padGrid.isVisible())
    {
        padGrid.setVisible (isDrumKit);
        instrumentButton.setVisible (! isDrumKit);
        resized();
    }

    padGrid.repaint();
}

void GeneratorPanel::rebuildSlotGrid()
{
    slotGrid.clearSlots();

    auto generator = getSelectedGenerator();

    // An audio generator has no patterns: its clips are the files placed on it,
    // and a pattern created here would be dead weight the playlist ignores.
    const bool showsPatterns = ! (generator && generator->isAudio());

    if (showsPatterns != slotGrid.isVisible())
    {
        slotGrid.setVisible (showsPatterns);
        editPatternButton.setVisible (showsPatterns);
        resized();
    }

    if (! showsPatterns)
        return;
    std::optional<model::Pattern> selected;

    if (generator)
    {
        for (const auto& pattern : generator->getPatterns())
        {
            if (pattern.getId() == selectedPatternId)
                selected = pattern;

            if (auto slot = pattern.getSlot())
            {
                slotGrid.setSlotState (*slot, pattern.isEmpty() ? PatternSlotGrid::SlotState::empty
                                                                : PatternSlotGrid::SlotState::hasNotes);
                if (pattern.getId() == selectedPatternId)
                    slotGrid.setSelectedSlot (*slot);
            }
        }
    }

    // The pad only has room for the slot number, so the header carries the
    // name - which the piano roll may have changed away from the slot's.
    patternHeader.setText (selected ? "Pattern: " + selected->getName() : "Patterns",
                           juce::dontSendNotification);

    // Same button, different job for a sampler; see its onClick.
    instrumentButton.setButtonText (generator && generator->isSampler() ? "Load Sample..."
                                                                        : "Instrument UI");
    slotGrid.repaint();
}

void GeneratorPanel::rebuildPatternBox()
{
    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    patternBox.clear (juce::dontSendNotification);

    auto generator = getSelectedGenerator();
    const auto others = generator ? generator->getUnslottedPatterns()
                                  : std::vector<model::Pattern>();

    for (int i = 0; i < (int) others.size(); ++i)
    {
        patternBox.addItem (others[(size_t) i].getName(), i + 1);
        if (others[(size_t) i].getId() == selectedPatternId)
            patternBox.setSelectedItemIndex (i, juce::dontSendNotification);
    }

    // Patterns outside the grid only exist in songs saved before slots did (or
    // once a generator holds more than 36), so the box stays out of the way.
    if (const bool shouldShow = ! others.empty(); shouldShow != patternBox.isVisible())
    {
        patternBox.setVisible (shouldShow);
        resized();
    }
}

void GeneratorPanel::fireSelectionChanged()
{
    if (onSelectionChanged)
        onSelectionChanged (getSelectedGeneratorId(), selectedPatternId);
}

void GeneratorPanel::resized()
{
    auto area = getLocalBounds().reduced (6);
    addGeneratorButton.setBounds (area.removeFromTop (28));
    area.removeFromTop (6);

    const int gridHeight = PatternSlotGrid::getPreferredHeight();
    const int otherPatternsHeight = patternBox.isVisible() ? 34 : 0;

    // The pads take the room the instrument button gives up, plus some of the
    // generator list's: a kit needs both its pads and its pattern slots.
    const int padsHeight = padGrid.isVisible() ? DrumPadGrid::getPreferredHeight() + 6 : 0;
    const int instrumentHeight = instrumentButton.isVisible() ? 28 + 6 : 0;

    auto bottom = area.removeFromBottom (6 + padsHeight + instrumentHeight + 22
                                          + gridHeight + 6 + otherPatternsHeight + 28);
    generatorList.setBounds (area);

    bottom.removeFromTop (6);

    if (padGrid.isVisible())
    {
        padGrid.setBounds (bottom.removeFromTop (DrumPadGrid::getPreferredHeight()));
        bottom.removeFromTop (6);
    }

    if (instrumentButton.isVisible())
    {
        instrumentButton.setBounds (bottom.removeFromTop (28));
        bottom.removeFromTop (6);
    }

    patternHeader.setBounds (bottom.removeFromTop (22));
    slotGrid.setBounds (bottom.removeFromTop (gridHeight));
    bottom.removeFromTop (6);

    if (patternBox.isVisible())
    {
        patternBox.setBounds (bottom.removeFromTop (28));
        bottom.removeFromTop (6);
    }

    editPatternButton.setBounds (bottom.removeFromTop (28));
}

} // namespace carve::app
