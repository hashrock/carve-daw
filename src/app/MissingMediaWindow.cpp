#include "MissingMediaWindow.h"

namespace carve::app
{

namespace
{
    constexpr int rowHeight = 44;
    constexpr int maxVisibleRows = 8;
    constexpr int windowWidth = 560;
} // namespace

MissingMediaComponent::Row::Row (MissingMediaComponent& owner, juce::ValueTree n)
    : node (std::move (n))
{
    nameLabel.setText (model::FileRef::getFileName (node), juce::dontSendNotification);
    nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    // Where the song last saw it: the folder is the clue to where it went.
    const auto file = model::FileRef::getFile (node);
    pathLabel.setText (file != juce::File() ? file.getParentDirectory().getFullPathName()
                                            : node[model::ids::relPath].toString(),
                       juce::dontSendNotification);
    pathLabel.setFont (juce::FontOptions (11.0f));
    pathLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    pathLabel.setMinimumHorizontalScale (0.5f);

    locateButton.onClick = [&owner, node = node] { owner.locate (node); };

    addAndMakeVisible (nameLabel);
    addAndMakeVisible (pathLabel);
    addAndMakeVisible (locateButton);
}

void MissingMediaComponent::Row::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2b2b30));
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void MissingMediaComponent::Row::resized()
{
    auto area = getLocalBounds().reduced (8, 4);
    locateButton.setBounds (area.removeFromRight (84).withSizeKeepingCentre (84, 24));
    area.removeFromRight (8);
    nameLabel.setBounds (area.removeFromTop (18));
    pathLabel.setBounds (area);
}

MissingMediaComponent::MissingMediaComponent (model::Song s, std::function<void()> done)
    : song (std::move (s)), onDone (std::move (done))
{
    header.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    header.setColour (juce::Label::textColourId, juce::Colours::white);

    note.setText ("Locate each file, or search a folder for all of them by name. "
                  "The song plays without them until they are found.",
                  juce::dontSendNotification);
    note.setFont (juce::FontOptions (12.0f));
    note.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));

    viewport.setViewedComponent (&rowHolder, false);
    viewport.setScrollBarsShown (true, false);

    searchButton.onClick = [this] { searchInFolder(); };
    ignoreButton.onClick = [this] { if (onDone) onDone(); };

    for (auto* c : std::initializer_list<juce::Component*> { &header, &note, &viewport, &searchButton, &ignoreButton })
        addAndMakeVisible (c);

    refresh();
}

void MissingMediaComponent::refresh()
{
    rows.clear();
    rowHolder.removeAllChildren();

    const auto missing = song.findMissingMedia();

    // Rebuilt rather than kept: a relink through either button can resolve
    // several rows at once (two placements of one file), and the list is
    // short enough that keeping it in step by hand would only add ways to
    // be wrong.
    for (const auto& node : missing)
    {
        auto row = std::make_unique<Row> (*this, node);
        rowHolder.addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    header.setText (juce::String (missing.size()) + (missing.size() == 1 ? " file" : " files")
                        + " could not be found",
                    juce::dontSendNotification);

    // Sized to the list, up to a screenful: the owner's window wraps this
    // component, so a change here moves the window's bottom edge.
    const auto visibleRows = juce::jlimit (1, maxVisibleRows, (int) rows.size());
    setSize (windowWidth, 12 + 22 + 18 + 10 + visibleRows * rowHeight + 10 + 26 + 12);
    resized();
}

void MissingMediaComponent::locate (juce::ValueTree node)
{
    const auto name = model::FileRef::getFileName (node);
    const auto extension = name.fromLastOccurrenceOf (".", true, false);

    chooser = std::make_shared<juce::FileChooser> ("Locate " + name,
                                                   model::FileRef::getFile (node).getParentDirectory(),
                                                   extension.isNotEmpty() ? "*" + extension : "*");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer (this), node] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        if (safe == nullptr || file == juce::File())
            return;

        // Not undoable: this is the load being finished, not an edit of the
        // song, and the same choice resolveMediaPaths makes.
        model::FileRef::setFile (node, file, nullptr);
        safe->refresh();

        if (safe->rows.empty() && safe->onDone)
            safe->onDone();
    });
}

void MissingMediaComponent::searchInFolder()
{
    chooser = std::make_shared<juce::FileChooser> ("Search for missing files in...", juce::File());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& fc)
    {
        const auto folder = fc.getResult();

        if (safe == nullptr || folder == juce::File())
            return;

        safe->song.relinkMissingMedia (folder, nullptr);
        safe->refresh();

        if (safe->rows.empty() && safe->onDone)
            safe->onDone();
    });
}

void MissingMediaComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    // A frame around the list so it reads as one, with the rows separated
    // inside it.
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawRect (viewport.getBounds().expanded (1));
}

void MissingMediaComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    header.setBounds (area.removeFromTop (22));
    note.setBounds (area.removeFromTop (18));
    area.removeFromTop (10);

    auto footer = area.removeFromBottom (26);
    area.removeFromBottom (10);

    ignoreButton.setBounds (footer.removeFromRight (80));
    footer.removeFromRight (8);
    searchButton.setBounds (footer.removeFromRight (140));

    viewport.setBounds (area);

    const auto rowWidth = viewport.getMaximumVisibleWidth();
    rowHolder.setSize (rowWidth, (int) rows.size() * rowHeight);

    int y = 0;

    for (auto& row : rows)
    {
        row->setBounds (0, y, rowWidth, rowHeight);
        y += rowHeight;
    }
}

} // namespace carve::app
