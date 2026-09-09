#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "IconButton.h"
#include "ShortcutHelpBar.h"

namespace te = tracktion;

namespace carve::app
{

//==============================================================================
// Click-to-audition playback for the browser. Runs straight on the audio
// device beside tracktion's own callback -- JUCE's device manager sums every
// callback it has -- so a preview never touches the edit, the transport, or
// any track's effect chain. That is the point: hearing a file must cost
// nothing and undo nothing.
class SamplePreviewPlayer
{
public:
    explicit SamplePreviewPlayer (te::Engine&);
    ~SamplePreviewPlayer();

    // Starts the file from the top, replacing whatever was playing. False
    // when the engine cannot read it.
    bool play (const juce::File&);
    void stop();
    bool isPlaying() const;

private:
    te::Engine& engine;
    juce::TimeSliceThread readThread { "sample preview" };
    juce::AudioTransportSource transport;
    juce::AudioSourcePlayer player;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SamplePreviewPlayer)
};

//==============================================================================
// A flat list of files and folders: the browser's two panes are both one of
// these inside a viewport. Its own component rather than a juce::ListBox
// because the rows have to start an *external* drag (see mouseDrag) -- the
// ListBox's drag hook only knows the in-process kind, and every drop target
// in the app takes files from the OS.
class BrowserList : public juce::Component
{
public:
    struct Entry
    {
        juce::File file;
        bool isFolder = false;

        bool operator== (const Entry&) const = default;
    };

    static constexpr int rowHeight = 22;

    BrowserList() = default;

    // A row was clicked or reached with the keyboard: the one row that
    // counts as "current", even when several are selected.
    std::function<void (int row)> onRowFocused;

    // Double-click or Return.
    std::function<void (int row)> onRowActivated;

    // Right-click, with the row's screen area to hang a menu off.
    std::function<void (int row, juce::Rectangle<int> screenArea)> onRowMenu;

    // Rows can be dragged out as files. Off for the favourites pane, whose
    // folders are places to go rather than things to drop.
    bool draggable = false;

    // Shown instead of rows while there are none.
    juce::String emptyText;

    void setEntries (std::vector<Entry> newEntries);
    const std::vector<Entry>& getEntries() const  { return entries; }
    const Entry* getEntry (int row) const;

    int getFocusedRow() const  { return focusedRow; }
    std::vector<juce::File> getSelectedFiles() const;
    void selectRow (int row, bool notify);

    // How tall the rows are together: the browser sizes the list from this
    // so the viewport scrolls and the empty text still has somewhere to sit.
    int getContentHeight() const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    int rowAt (juce::Point<int>) const;
    juce::Rectangle<int> rowBounds (int row) const;
    bool isSelected (int row) const;
    void ensureRowVisible (int row);
    void focusRow (int row, bool notify);

    std::vector<Entry> entries;
    std::vector<int> selectedRows;   // sorted
    int focusedRow = -1;             // the anchor of a shift-click range, and the previewed row

    // Whether this press already started a drag, so a slow drag does not
    // start a second one; and whether releasing without a drag should
    // collapse a multi-selection to the row pressed, the way every file
    // manager does.
    bool dragStarted = false;
    bool collapseOnMouseUp = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BrowserList)
};

//==============================================================================
// The sample browser: favourite folders on top, the folder being looked at
// below, and every audio file in it a drag away from a drum pad, the
// sampler's Inst tab, or the playlist.
//
// Drags leave here as OS file drags (DragAndDropContainer::
// performExternalDragDropOfFiles), so the drop targets are the ones the app
// already has for files from the Finder -- including the ones in the
// generator window, which is a separate top-level window -- and the same
// drag can leave the app altogether.
//
// Favourites, the current folder and the preview switch are the user's and
// not the song's, so they live in a settings file beside the mixer window's
// bounds rather than in the .carve.
class SampleBrowser : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    explicit SampleBrowser (te::Engine&);
    ~SampleBrowser() override;

    static constexpr int preferredWidth = 240;

    // A file was double-clicked (or Return was pressed on it): the host
    // decides what "load" means for the selected generator.
    std::function<void (const juce::File&)> onFileActivated;

    // The shortcuts that work here, pushed while the pointer is over the
    // browser and cleared (an empty list) when it leaves, so the host can
    // hand the bar back to whatever it was showing.
    std::function<void (std::vector<ShortcutHelpBar::Entry>)> onShortcutHelpChanged;

    // Whether the browser was showing when the app last ran; the host
    // decides where it goes, this only remembers.
    bool wasShownLastSession() const;

    void addFavourite (const juce::File& folder);
    void removeFavourite (const juce::File& folder);
    bool isFavourite (const juce::File& folder) const;

    void setFolder (const juce::File& folder);
    void goUp();

    void stopPreview();

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override;
    void mouseEnter (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    static constexpr int headerHeight = 26;
    static constexpr int maxFavouriteRows = 6;

    void timerCallback() override;

    bool isAudioFile (const juce::File&) const;
    std::vector<BrowserList::Entry> scanFolder (const juce::File&) const;

    // Rebuilds the folder pane. Quiet means only when the folder's contents
    // actually changed, which is what the timer wants: a rescan that replaces
    // identical rows would drop the selection every two seconds.
    void refreshFolder (bool force);
    void refreshFavourites();
    void updatePathRow();
    void layoutLists();

    void chooseFolderToAdd();
    void previewRow (int row);
    void showFavouriteMenu (int row, juce::Rectangle<int> screenArea);
    void showEntryMenu (int row, juce::Rectangle<int> screenArea);

    static std::unique_ptr<juce::PropertiesFile> createSettingsFile (te::Engine&);
    void loadSettings();
    void saveFavourites();

    te::Engine& engine;
    std::unique_ptr<juce::PropertiesFile> settings;   // flushes itself on destruction
    SamplePreviewPlayer preview;

    std::vector<juce::File> favourites;
    juce::File folder;

    juce::Label favouritesTitle, folderLabel;
    IconButton previewButton { "", Icon::speaker }, addButton { "", Icon::plus },
               upButton { "", Icon::arrowUp }, starButton { "", Icon::star };

    BrowserList favouritesList, folderList;
    juce::Viewport favouritesViewport, folderViewport;

    std::unique_ptr<juce::FileChooser> folderChooser;   // outlives launchAsync
    bool folderDragOver = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleBrowser)
};

} // namespace carve::app
