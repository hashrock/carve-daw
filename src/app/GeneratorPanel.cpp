#include "GeneratorPanel.h"

namespace orionish::app
{

GeneratorPanel::GeneratorPanel (model::Song songModel, juce::UndoManager& um)
    : song (std::move (songModel)), undoManager (um)
{
    generatorList.setModel (this);
    generatorList.setRowHeight (26);

    addGeneratorButton.onClick = [this]
    {
        undoManager.beginNewTransaction();
        auto generator = song.addGenerator ("Synth " + juce::String (song.getNumGenerators() + 1),
                                            "internal-synth", &undoManager);
        auto pattern = generator.addPattern ("Pattern 1", 4.0, &undoManager);
        selectedPatternId = pattern.getId();
        refresh();
        generatorList.selectRow (song.getNumGenerators() - 1);
    };

    patternHeader.setText ("Patterns", juce::dontSendNotification);
    patternHeader.setJustificationType (juce::Justification::centredLeft);

    patternBox.onChange = [this]
    {
        if (isRefreshing)
            return;
        const int index = patternBox.getSelectedItemIndex();
        const int row = generatorList.getSelectedRow();
        if (row >= 0 && row < song.getNumGenerators() && index >= 0)
        {
            auto generator = song.getGenerator (row);
            if (index < generator.getNumPatterns())
            {
                selectedPatternId = generator.getPattern (index).getId();
                fireSelectionChanged();
            }
        }
    };

    addPatternButton.onClick = [this]
    {
        const int row = generatorList.getSelectedRow();
        if (row < 0 || row >= song.getNumGenerators())
            return;
        undoManager.beginNewTransaction();
        auto generator = song.getGenerator (row);
        auto pattern = generator.addPattern ("Pattern " + juce::String (generator.getNumPatterns() + 1),
                                             4.0, &undoManager);
        selectedPatternId = pattern.getId();
        refresh();
        fireSelectionChanged();
    };

    for (auto* c : std::initializer_list<juce::Component*> {
             &generatorList, &addGeneratorButton, &patternHeader,
             &patternBox, &addPatternButton })
        addAndMakeVisible (c);

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

void GeneratorPanel::selectedRowsChanged (int row)
{
    if (isRefreshing)
        return;

    // Selecting a generator selects its first pattern unless the current
    // pattern already belongs to it.
    if (row >= 0 && row < song.getNumGenerators())
    {
        auto generator = song.getGenerator (row);
        if (! generator.findPattern (selectedPatternId).has_value())
            selectedPatternId = generator.getNumPatterns() > 0 ? generator.getPattern (0).getId()
                                                               : juce::String();
    }

    rebuildPatternBox();
    fireSelectionChanged();
}

void GeneratorPanel::refresh()
{
    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    generatorList.updateContent();
    generatorList.repaint();
    rebuildPatternBox();
}

void GeneratorPanel::rebuildPatternBox()
{
    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    patternBox.clear (juce::dontSendNotification);

    const int row = generatorList.getSelectedRow();
    if (row < 0 || row >= song.getNumGenerators())
        return;

    auto generator = song.getGenerator (row);
    int selectedIndex = -1;

    for (int i = 0; i < generator.getNumPatterns(); ++i)
    {
        auto pattern = generator.getPattern (i);
        patternBox.addItem (pattern.getName(), i + 1);
        if (pattern.getId() == selectedPatternId)
            selectedIndex = i;
    }

    if (selectedIndex < 0 && generator.getNumPatterns() > 0)
    {
        selectedIndex = 0;
        selectedPatternId = generator.getPattern (0).getId();
    }

    if (selectedIndex >= 0)
        patternBox.setSelectedItemIndex (selectedIndex, juce::dontSendNotification);
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

    auto bottom = area.removeFromBottom (96);
    generatorList.setBounds (area);

    bottom.removeFromTop (6);
    patternHeader.setBounds (bottom.removeFromTop (22));
    patternBox.setBounds (bottom.removeFromTop (28));
    bottom.removeFromTop (6);
    addPatternButton.setBounds (bottom.removeFromTop (28));
}

} // namespace orionish::app
