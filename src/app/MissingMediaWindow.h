#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

namespace carve::app
{

// The files a just-loaded song names but cannot find, each with a button to
// point it at where the file went, plus one to scan a folder for all of them
// at once. Relinking writes into the model through FileRef::setFile, so the
// next save stores the new location the way it stores any other.
//
// Deliberately not modal, in the same shape as ExportWindow: loading a song
// starts the engine syncing it, and a modal loop here would sit on the message
// thread the sync needs. The rest of the app stays usable around it, and the
// song plays with whatever it could find while the user sorts out the rest.
class MissingMediaComponent : public juce::Component
{
public:
    // onDone fires once nothing is missing any more, or when the user gives
    // up on the rest; the owner closes the window from it.
    MissingMediaComponent (model::Song song, std::function<void()> onDone);

    // Re-reads the song for what is still missing and rebuilds the rows.
    void refresh();

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // One missing file: its name, where the song last saw it, and Locate...
    struct Row : public juce::Component
    {
        Row (MissingMediaComponent& owner, juce::ValueTree node);
        void paint (juce::Graphics&) override;
        void resized() override;

        juce::ValueTree node;
        juce::Label nameLabel, pathLabel;
        juce::TextButton locateButton { "Locate..." };
    };

    void locate (juce::ValueTree node);
    void searchInFolder();

    model::Song song;
    std::function<void()> onDone;

    juce::Label header, note;
    juce::Viewport viewport;
    juce::Component rowHolder;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton searchButton { "Search in folder..." }, ignoreButton { "Ignore" };

    // Kept alive across the async chooser callback, as MainComponent does.
    std::shared_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MissingMediaComponent)
};

class MissingMediaWindow : public juce::DocumentWindow
{
public:
    MissingMediaWindow (model::Song song, std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("Missing files",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (std::move (song), [this] { requestClose(); })
    {
        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setAlwaysOnTop (true);
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        requestClose();
    }

private:
    void requestClose()
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    std::function<void()> onClose;
    MissingMediaComponent content;
};

} // namespace carve::app
