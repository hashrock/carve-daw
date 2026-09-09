#include "SampleBrowser.h"

namespace carve::app
{

namespace
{
    const juce::Colour panelColour   (0xff1c1c20);
    const juce::Colour headerColour  (0xff26262c);
    const juce::Colour lineColour    (0xff3a3a40);
    const juce::Colour textColour    (0xffd8d8de);
    const juce::Colour dimColour     (0xff8a8a94);
    const juce::Colour accentColour  (0xffe08a3c);
    const juce::Colour selectColour  (0xff35608a);

    // Below this a press is a click, not the start of a drag: the same
    // allowance JUCE's own drag-and-drop uses.
    constexpr int dragThreshold = 5;
} // namespace

//==============================================================================
// SamplePreviewPlayer

SamplePreviewPlayer::SamplePreviewPlayer (te::Engine& engineToUse)
    : engine (engineToUse)
{
    readThread.startThread();
    player.setSource (&transport);
    engine.getDeviceManager().deviceManager.addAudioCallback (&player);
}

SamplePreviewPlayer::~SamplePreviewPlayer()
{
    engine.getDeviceManager().deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
    transport.setSource (nullptr);
    readerSource.reset();
    readThread.stopThread (1000);
}

bool SamplePreviewPlayer::play (const juce::File& file)
{
    stop();

    std::unique_ptr<juce::AudioFormatReader> reader (
        engine.getAudioFileFormatManager().readFormatManager.createReaderFor (file));

    if (reader == nullptr)
        return false;

    const auto sampleRate = reader->sampleRate;
    readerSource = std::make_unique<juce::AudioFormatReaderSource> (reader.release(), true);

    // Read ahead on the thread: a long file previewed straight off the disk
    // would glitch on the audio thread. The rate is the file's, so a 48k
    // sample previews at pitch on a 44.1k device.
    transport.setSource (readerSource.get(), 32768, &readThread, sampleRate);
    transport.setGain (0.8f);   // samples are recorded hot; this is an audition, not a mix
    transport.setPosition (0.0);
    transport.start();
    return true;
}

void SamplePreviewPlayer::stop()
{
    transport.stop();
    transport.setSource (nullptr);
    readerSource.reset();
}

bool SamplePreviewPlayer::isPlaying() const
{
    return transport.isPlaying();
}

//==============================================================================
// BrowserList

void BrowserList::setEntries (std::vector<Entry> newEntries)
{
    // Keep the selection on the same files across a rescan, so a folder that
    // gained a file under the pointer does not lose the row being auditioned.
    const auto focusedFile = focusedRow >= 0 && focusedRow < (int) entries.size()
                                 ? entries[(size_t) focusedRow].file : juce::File();
    auto selectedFiles = getSelectedFiles();

    entries = std::move (newEntries);
    selectedRows.clear();
    focusedRow = -1;

    for (int i = 0; i < (int) entries.size(); ++i)
    {
        const auto& file = entries[(size_t) i].file;

        if (std::find (selectedFiles.begin(), selectedFiles.end(), file) != selectedFiles.end())
            selectedRows.push_back (i);

        if (file == focusedFile)
            focusedRow = i;
    }

    repaint();
}

const BrowserList::Entry* BrowserList::getEntry (int row) const
{
    if (row >= 0 && row < (int) entries.size())
        return &entries[(size_t) row];

    return nullptr;
}

std::vector<juce::File> BrowserList::getSelectedFiles() const
{
    std::vector<juce::File> files;

    for (int row : selectedRows)
        if (auto* entry = getEntry (row))
            files.push_back (entry->file);

    return files;
}

int BrowserList::getContentHeight() const
{
    return (int) entries.size() * rowHeight;
}

bool BrowserList::isSelected (int row) const
{
    return std::binary_search (selectedRows.begin(), selectedRows.end(), row);
}

juce::Rectangle<int> BrowserList::rowBounds (int row) const
{
    return { 0, row * rowHeight, getWidth(), rowHeight };
}

int BrowserList::rowAt (juce::Point<int> position) const
{
    const int row = position.y / rowHeight;
    return position.y >= 0 && row < (int) entries.size() ? row : -1;
}

void BrowserList::selectRow (int row, bool notify)
{
    selectedRows.clear();

    if (getEntry (row) != nullptr)
        selectedRows.push_back (row);

    focusRow (row, notify);
}

void BrowserList::focusRow (int row, bool notify)
{
    focusedRow = getEntry (row) != nullptr ? row : -1;
    ensureRowVisible (focusedRow);
    repaint();

    if (notify && focusedRow >= 0 && onRowFocused)
        onRowFocused (focusedRow);
}

void BrowserList::ensureRowVisible (int row)
{
    auto* viewport = findParentComponentOfClass<juce::Viewport>();

    if (viewport == nullptr || getEntry (row) == nullptr)
        return;

    const auto bounds = rowBounds (row);
    const auto view = viewport->getViewArea();

    if (bounds.getY() < view.getY())
        viewport->setViewPosition (view.getX(), bounds.getY());
    else if (bounds.getBottom() > view.getBottom())
        viewport->setViewPosition (view.getX(), bounds.getBottom() - view.getHeight());
}

void BrowserList::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);

    if (entries.empty())
    {
        g.setColour (dimColour);
        g.setFont (12.0f);
        g.drawFittedText (emptyText, getLocalBounds().reduced (10, 4),
                          juce::Justification::centred, 3);
        return;
    }

    const auto clip = g.getClipBounds();

    for (int row = 0; row < (int) entries.size(); ++row)
    {
        const auto bounds = rowBounds (row);

        if (! bounds.intersects (clip))
            continue;

        const auto& entry = entries[(size_t) row];
        const bool selected = isSelected (row);

        if (selected)
        {
            g.setColour (selectColour.withAlpha (hasKeyboardFocus (true) ? 1.0f : 0.6f));
            g.fillRect (bounds);
        }
        else if (row == focusedRow)
        {
            g.setColour (juce::Colours::white.withAlpha (0.05f));
            g.fillRect (bounds);
        }

        auto area = bounds.reduced (8, 0);
        const auto iconSize = 12.0f;
        const auto iconArea = area.removeFromLeft (16).toFloat()
                                  .withSizeKeepingCentre (iconSize, iconSize);

        // Folders carry the accent so they read as places to go; files stay
        // quiet, since a folder of one-shots is a wall of them.
        g.setColour (entry.isFolder ? accentColour.withAlpha (0.85f) : dimColour);
        g.fillPath (icons::make (entry.isFolder ? Icon::folder : Icon::audioFile, iconArea));

        area.removeFromLeft (6);
        g.setColour (selected ? juce::Colours::white : textColour);
        g.setFont (12.0f);
        g.drawText (entry.file.getFileName(), area, juce::Justification::centredLeft, true);
    }
}

void BrowserList::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragStarted = false;
    collapseOnMouseUp = false;

    const int row = rowAt (e.getPosition());

    if (row < 0)
    {
        if (! e.mods.isAnyModifierKeyDown())
        {
            selectedRows.clear();
            repaint();
        }

        return;
    }

    if (e.mods.isPopupMenu())
    {
        // The row under the pointer is what the menu is about; a selection
        // it is not part of is left alone only when it includes the row.
        if (! isSelected (row))
            selectRow (row, false);
        else
            focusRow (row, false);

        if (onRowMenu)
            onRowMenu (row, localAreaToGlobal (rowBounds (row)));

        return;
    }

    if (e.mods.isCommandDown())
    {
        // Toggle the row in and out of the selection.
        if (isSelected (row))
            selectedRows.erase (std::remove (selectedRows.begin(), selectedRows.end(), row),
                                selectedRows.end());
        else
            selectedRows.insert (std::upper_bound (selectedRows.begin(), selectedRows.end(), row), row);

        focusRow (row, isSelected (row));
        return;
    }

    if (e.mods.isShiftDown() && focusedRow >= 0)
    {
        // Everything between the anchor and here, the anchor staying put.
        selectedRows.clear();
        for (int i = juce::jmin (focusedRow, row); i <= juce::jmax (focusedRow, row); ++i)
            selectedRows.push_back (i);

        repaint();
        return;
    }

    if (isSelected (row) && selectedRows.size() > 1)
    {
        // A press on one of several selected rows might be the start of a
        // drag of all of them, so the selection is kept until the release
        // says it was only a click.
        collapseOnMouseUp = true;
        focusRow (row, true);
        return;
    }

    selectRow (row, true);
}

void BrowserList::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggable || dragStarted || e.mods.isPopupMenu()
         || e.getDistanceFromDragStart() < dragThreshold)
        return;

    const int row = rowAt (e.getMouseDownPosition());
    auto* entry = getEntry (row);

    if (entry == nullptr || entry->isFolder)
        return;

    // The rows being dragged: the whole selection when the press was on it,
    // the one row otherwise. Folders are left out -- no drop target takes one.
    juce::StringArray files;

    for (int selectedRow : isSelected (row) ? selectedRows : std::vector<int> { row })
        if (auto* selected = getEntry (selectedRow); selected != nullptr && ! selected->isFolder)
            files.add (selected->file.getFullPathName());

    if (files.isEmpty())
        return;

    dragStarted = true;
    collapseOnMouseUp = false;

    // An OS drag, not an in-process one: every drop target in the app takes
    // files the way the Finder hands them over, including the generator
    // window, which is a separate top-level window the in-process kind would
    // still reach but through targets the app does not have.
    juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

void BrowserList::mouseUp (const juce::MouseEvent& e)
{
    if (collapseOnMouseUp && ! dragStarted && ! e.mouseWasDraggedSinceMouseDown())
        selectRow (rowAt (e.getMouseDownPosition()), false);

    collapseOnMouseUp = false;
    dragStarted = false;
}

void BrowserList::mouseDoubleClick (const juce::MouseEvent& e)
{
    const int row = rowAt (e.getPosition());

    if (row >= 0 && onRowActivated)
        onRowActivated (row);
}

bool BrowserList::keyPressed (const juce::KeyPress& key)
{
    if (entries.empty())
        return false;

    auto step = [this] (int to)
    {
        selectRow (juce::jlimit (0, (int) entries.size() - 1, to), true);
        return true;
    };

    if (key == juce::KeyPress::upKey)
        return step (focusedRow < 0 ? 0 : focusedRow - 1);

    if (key == juce::KeyPress::downKey)
        return step (focusedRow < 0 ? 0 : focusedRow + 1);

    if (key == juce::KeyPress::homeKey)
        return step (0);

    if (key == juce::KeyPress::endKey)
        return step ((int) entries.size() - 1);

    if (key == juce::KeyPress::returnKey && focusedRow >= 0)
    {
        if (onRowActivated)
            onRowActivated (focusedRow);

        return true;
    }

    return false;
}

//==============================================================================
// SampleBrowser

SampleBrowser::SampleBrowser (te::Engine& engineToUse)
    : engine (engineToUse),
      settings (createSettingsFile (engine)),
      preview (engine)
{
    favouritesTitle.setText ("Favorites", juce::dontSendNotification);
    favouritesTitle.setColour (juce::Label::textColourId, dimColour);
    favouritesTitle.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    addAndMakeVisible (favouritesTitle);

    // Preview is a lit toggle like the transport's loop: on by default,
    // because a sample browser you cannot hear is a file list.
    previewButton.setFlat (true);
    previewButton.setClickingTogglesState (true);
    previewButton.setColour (juce::TextButton::textColourOnId, accentColour);
    previewButton.setColour (juce::TextButton::textColourOffId, dimColour);
    previewButton.setTooltip ("Preview files on click");
    previewButton.onClick = [this]
    {
        settings->setValue ("preview", previewButton.getToggleState());

        if (! previewButton.getToggleState())
            preview.stop();
    };
    addAndMakeVisible (previewButton);

    addButton.setFlat (true);
    addButton.setTooltip ("Add a folder to the favourites");
    addButton.onClick = [this] { chooseFolderToAdd(); };
    addAndMakeVisible (addButton);

    upButton.setFlat (true);
    upButton.setTooltip ("Up a folder (Backspace)");
    upButton.onClick = [this] { goUp(); };
    addAndMakeVisible (upButton);

    folderLabel.setColour (juce::Label::textColourId, textColour);
    folderLabel.setFont (juce::FontOptions (12.0f));
    folderLabel.setMinimumHorizontalScale (1.0f);   // ellipsis, not squashed text
    addAndMakeVisible (folderLabel);

    // The star is the current folder's favourite state, and the way to
    // change it: lit while the folder is in the list above.
    starButton.setFlat (true);
    starButton.setColour (juce::TextButton::textColourOnId, accentColour);
    starButton.setColour (juce::TextButton::textColourOffId, dimColour);
    starButton.setTooltip ("Favourite this folder");
    starButton.onClick = [this]
    {
        if (isFavourite (folder))
            removeFavourite (folder);
        else
            addFavourite (folder);
    };
    addAndMakeVisible (starButton);

    favouritesList.emptyText = "No favorites yet.\nPress + or drop a folder here.";
    favouritesList.onRowFocused = [this] (int row)
    {
        if (auto* entry = favouritesList.getEntry (row))
            setFolder (entry->file);
    };
    favouritesList.onRowActivated = favouritesList.onRowFocused;
    favouritesList.onRowMenu = [this] (int row, juce::Rectangle<int> area) { showFavouriteMenu (row, area); };

    folderList.draggable = true;
    folderList.onRowFocused = [this] (int row) { previewRow (row); };
    folderList.onRowActivated = [this] (int row)
    {
        auto* entry = folderList.getEntry (row);

        if (entry == nullptr)
            return;

        if (entry->isFolder)
            setFolder (entry->file);
        else if (onFileActivated)
            onFileActivated (entry->file);
    };
    folderList.onRowMenu = [this] (int row, juce::Rectangle<int> area) { showEntryMenu (row, area); };

    for (auto* viewport : { &favouritesViewport, &folderViewport })
    {
        viewport->setScrollBarsShown (true, false);
        viewport->setScrollBarThickness (8);
        addAndMakeVisible (viewport);
    }

    favouritesViewport.setViewedComponent (&favouritesList, false);
    folderViewport.setViewedComponent (&folderList, false);

    // Enter and exit are delivered to the deepest component under the
    // pointer, which is one of the lists; listening to the children is how
    // the browser as a whole hears the pointer arrive and leave.
    addMouseListener (this, true);

    loadSettings();
    refreshFavourites();
    refreshFolder (true);

    startTimer (2000);
}

SampleBrowser::~SampleBrowser()
{
    stopTimer();
    removeMouseListener (this);
}

//==============================================================================
// Settings

std::unique_ptr<juce::PropertiesFile> SampleBrowser::createSettingsFile (te::Engine& engine)
{
    // Its own file rather than a second handle on the mixer's: two
    // PropertiesFile objects on one path would each write the other's keys
    // back out stale. Same folder, so every setting the app has is in one
    // place (see EngineSetup.h).
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    options.millisecondsBeforeSaving = 500;

    return std::make_unique<juce::PropertiesFile> (
        engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("browser.settings"),
        options);
}

void SampleBrowser::loadSettings()
{
    favourites.clear();

    if (auto xml = settings->getXmlValue ("favourites"))
        for (auto* child : xml->getChildIterator())
            if (const juce::File file (child->getStringAttribute ("path")); file != juce::File())
                favourites.push_back (file);

    previewButton.setToggleState (settings->getBoolValue ("preview", true), juce::dontSendNotification);

    // The folder last looked at, then the first favourite, then the music
    // folder: whichever of them still exists.
    const juce::File remembered (settings->getValue ("folder"));

    if (remembered.isDirectory())
        folder = remembered;
    else if (! favourites.empty() && favourites.front().isDirectory())
        folder = favourites.front();
    else
        folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
}

void SampleBrowser::saveFavourites()
{
    juce::XmlElement xml ("FAVOURITES");

    for (const auto& file : favourites)
        xml.createNewChildElement ("FOLDER")->setAttribute ("path", file.getFullPathName());

    settings->setValue ("favourites", &xml);
}

bool SampleBrowser::wasShownLastSession() const
{
    return settings->getBoolValue ("visible", true);
}

void SampleBrowser::visibilityChanged()
{
    settings->setValue ("visible", isVisible());

    if (isVisible())
        refreshFolder (false);   // whatever changed on disk while it was hidden
    else
        preview.stop();
}

//==============================================================================
// Favourites

bool SampleBrowser::isFavourite (const juce::File& target) const
{
    return std::find (favourites.begin(), favourites.end(), target) != favourites.end();
}

void SampleBrowser::addFavourite (const juce::File& target)
{
    if (! target.isDirectory() || isFavourite (target))
        return;

    favourites.push_back (target);
    saveFavourites();
    refreshFavourites();
    updatePathRow();
}

void SampleBrowser::removeFavourite (const juce::File& target)
{
    favourites.erase (std::remove (favourites.begin(), favourites.end(), target), favourites.end());
    saveFavourites();
    refreshFavourites();
    updatePathRow();
}

void SampleBrowser::refreshFavourites()
{
    std::vector<BrowserList::Entry> entries;

    for (const auto& file : favourites)
        entries.push_back ({ file, true });

    favouritesList.setEntries (std::move (entries));

    // The current folder's row is the selected one, so the two panes agree.
    for (int i = 0; i < (int) favourites.size(); ++i)
        if (favourites[(size_t) i] == folder)
            favouritesList.selectRow (i, false);

    resized();   // the pane is as tall as its rows, so a change here moves the split
}

void SampleBrowser::chooseFolderToAdd()
{
    folderChooser = std::make_unique<juce::FileChooser> ("Add a folder of samples", folder);

    folderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                [this] (const juce::FileChooser& chooser)
    {
        if (const auto chosen = chooser.getResult(); chosen.isDirectory())
        {
            addFavourite (chosen);
            setFolder (chosen);
        }
    });
}

void SampleBrowser::showFavouriteMenu (int row, juce::Rectangle<int> screenArea)
{
    auto* entry = favouritesList.getEntry (row);

    if (entry == nullptr)
        return;

    const auto target = entry->file;

    juce::PopupMenu menu;
    menu.addSectionHeader (target.getFileName());
    menu.addItem (1, "Remove from favorites");
    menu.addItem (2, "Reveal in Finder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, target] (int result)
    {
        if (result == 1)
            removeFavourite (target);
        else if (result == 2)
            target.revealToUser();
    });
}

//==============================================================================
// The folder pane

bool SampleBrowser::isAudioFile (const juce::File& file) const
{
    // The engine's read formats by extension, so what is listed is what a
    // pad or the playlist will accept -- but not through canOpen, which also
    // says yes to MIDI files, and a .mid is not a sample.
    return engine.getAudioFileFormatManager().readFormatManager
               .findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

std::vector<BrowserList::Entry> SampleBrowser::scanFolder (const juce::File& target) const
{
    std::vector<BrowserList::Entry> folders, files;

    if (! target.isDirectory())
        return {};

    for (const auto& child : target.findChildFiles (juce::File::findFilesAndDirectories, false))
    {
        if (child.isHidden())
            continue;

        if (child.isDirectory())
            folders.push_back ({ child, true });
        else if (isAudioFile (child))
            files.push_back ({ child, false });
    }

    auto byName = [] (const BrowserList::Entry& a, const BrowserList::Entry& b)
    {
        return a.file.getFileName().compareNatural (b.file.getFileName()) < 0;
    };

    std::sort (folders.begin(), folders.end(), byName);
    std::sort (files.begin(), files.end(), byName);

    // Folders first, the way every browser lists them: the places to go
    // before the things to grab.
    folders.insert (folders.end(), files.begin(), files.end());
    return folders;
}

void SampleBrowser::setFolder (const juce::File& newFolder)
{
    if (newFolder == folder)
        return;

    preview.stop();
    folder = newFolder;
    settings->setValue ("folder", folder.getFullPathName());
    refreshFolder (true);
    refreshFavourites();   // reselects the row that is this folder, if any
    folderViewport.setViewPosition (0, 0);
}

void SampleBrowser::goUp()
{
    const auto parent = folder.getParentDirectory();

    if (parent != folder && parent.isDirectory())
        setFolder (parent);
}

void SampleBrowser::refreshFolder (bool force)
{
    auto entries = scanFolder (folder);

    if (! force && entries == folderList.getEntries())
        return;

    folderList.emptyText = folder.isDirectory() ? "No audio files here" : "Folder not found";
    folderList.setEntries (std::move (entries));
    updatePathRow();
    layoutLists();
}

void SampleBrowser::updatePathRow()
{
    folderLabel.setText (folder.getFileName().isNotEmpty() ? folder.getFileName()
                                                           : folder.getFullPathName(),
                         juce::dontSendNotification);
    folderLabel.setTooltip (folder.getFullPathName());

    const auto parent = folder.getParentDirectory();
    upButton.setEnabled (parent != folder && parent.isDirectory());
    starButton.setEnabled (folder.isDirectory());
    starButton.setToggleState (isFavourite (folder), juce::dontSendNotification);
}

void SampleBrowser::timerCallback()
{
    // A folder being filled from the Finder or a render shows up without a
    // reload; setEntries keeps the selection when nothing moved.
    if (isShowing())
        refreshFolder (false);
}

void SampleBrowser::previewRow (int row)
{
    auto* entry = folderList.getEntry (row);

    if (entry == nullptr || entry->isFolder || ! previewButton.getToggleState())
    {
        preview.stop();
        return;
    }

    preview.play (entry->file);
}

void SampleBrowser::stopPreview()
{
    preview.stop();
}

void SampleBrowser::showEntryMenu (int row, juce::Rectangle<int> screenArea)
{
    auto* entry = folderList.getEntry (row);

    if (entry == nullptr)
        return;

    const auto target = entry->file;
    const bool isFolder = entry->isFolder;

    juce::PopupMenu menu;
    menu.addSectionHeader (target.getFileName());

    if (isFolder)
        menu.addItem (1, isFavourite (target) ? "Remove from favorites" : "Add to favorites");
    else
        menu.addItem (3, "Load into selected generator");

    menu.addItem (2, "Reveal in Finder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (screenArea),
                        [this, target, isFolder] (int result)
    {
        if (result == 1 && isFolder)
        {
            if (isFavourite (target))
                removeFavourite (target);
            else
                addFavourite (target);
        }
        else if (result == 2)
            target.revealToUser();
        else if (result == 3 && onFileActivated)
            onFileActivated (target);
    });
}

//==============================================================================
// Layout and painting

void SampleBrowser::layoutLists()
{
    auto fit = [] (juce::Viewport& viewport, BrowserList& list)
    {
        const int content = list.getContentHeight();
        const bool scrolls = content > viewport.getHeight();
        const int width = viewport.getWidth() - (scrolls ? viewport.getScrollBarThickness() : 0);

        // At least the viewport's height, so the empty text has room and a
        // click below the last row still lands on the list.
        list.setSize (juce::jmax (1, width), juce::jmax (content, viewport.getHeight()));
    };

    fit (favouritesViewport, favouritesList);
    fit (folderViewport, folderList);
}

void SampleBrowser::resized()
{
    auto area = getLocalBounds();
    area.removeFromRight (1);   // the divider

    auto header = area.removeFromTop (headerHeight).reduced (4, 2);
    addButton.setBounds (header.removeFromRight (24));
    previewButton.setBounds (header.removeFromRight (24));
    favouritesTitle.setBounds (header.withTrimmedLeft (4));

    // The favourites pane is as tall as its rows, up to a handful, so a
    // short list leaves the room to the folder below it.
    const int favouriteRows = juce::jlimit (2, maxFavouriteRows, (int) favourites.size());
    favouritesViewport.setBounds (area.removeFromTop (favouriteRows * BrowserList::rowHeight));

    area.removeFromTop (1);   // divider

    auto pathRow = area.removeFromTop (headerHeight).reduced (4, 2);
    upButton.setBounds (pathRow.removeFromLeft (24));
    starButton.setBounds (pathRow.removeFromRight (24));
    folderLabel.setBounds (pathRow.withTrimmedLeft (2));

    folderViewport.setBounds (area);
    layoutLists();
}

void SampleBrowser::paint (juce::Graphics& g)
{
    g.fillAll (panelColour);

    g.setColour (headerColour);
    g.fillRect (0, 0, getWidth(), headerHeight);
    g.fillRect (0, favouritesViewport.getBottom() + 1, getWidth(), headerHeight);

    g.setColour (lineColour);
    g.fillRect (getWidth() - 1, 0, 1, getHeight());
    g.fillRect (0, favouritesViewport.getBottom(), getWidth(), 1);

    // A folder held over the browser is about to become a favourite.
    if (folderDragOver)
    {
        g.setColour (accentColour);
        g.drawRect (favouritesViewport.getBounds().expanded (0, 1), 2);
    }
}

bool SampleBrowser::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::backspaceKey
         || key == juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::commandModifier, 0))
    {
        goUp();
        return true;
    }

    return false;
}

void SampleBrowser::mouseEnter (const juce::MouseEvent&)
{
    if (onShortcutHelpChanged)
        onShortcutHelpChanged ({ { "click", "preview" },
                                 { "drag", "to pad / sampler / playlist" },
                                 { "double click", "open folder / load sample" },
                                 { "Backspace", "up a folder" },
                                 { "Cmd+click", "extend selection" },
                                 { "right click", "favorite, reveal" },
                                 { "Cmd+B", "hide browser" } });
}

void SampleBrowser::mouseExit (const juce::MouseEvent&)
{
    // Only when the pointer has really left: moving between the panes
    // reports an exit from the browser too, since the lists are children.
    if (! isMouseOver (true) && onShortcutHelpChanged)
        onShortcutHelpChanged ({});
}

//==============================================================================
// Folders dropped from the Finder become favourites

bool SampleBrowser::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
        if (juce::File (path).isDirectory())
            return true;

    return false;
}

void SampleBrowser::fileDragEnter (const juce::StringArray&, int, int)
{
    folderDragOver = true;
    repaint();
}

void SampleBrowser::fileDragExit (const juce::StringArray&)
{
    folderDragOver = false;
    repaint();
}

void SampleBrowser::filesDropped (const juce::StringArray& files, int, int)
{
    folderDragOver = false;
    repaint();

    juce::File first;

    for (const auto& path : files)
    {
        if (const juce::File dropped (path); dropped.isDirectory())
        {
            addFavourite (dropped);

            if (first == juce::File())
                first = dropped;
        }
    }

    // Land in the first one: dropping a folder here means "I want to browse
    // this", and a favourite that has to be clicked next is half a gesture.
    if (first != juce::File())
        setFolder (first);
}

} // namespace carve::app
