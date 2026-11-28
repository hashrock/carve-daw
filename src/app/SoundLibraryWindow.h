#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "SoundLibrary.h"

namespace carve::app
{

// Text to read and scroll, never to edit: an author's terms. Not a read-only
// TextEditor, whose wrapping measures a line again for every character it
// tries -- a paragraph of Japanese has no spaces to break at, so it is one
// long word to it, and a few kilobytes of terms took over ten seconds on an
// iPad (forty on a Mac) every time a pack was picked. TextLayout wraps it in
// one pass.
class ReadOnlyText : public juce::Component
{
public:
    ReadOnlyText()
    {
        viewport.setViewedComponent (&body, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
    }

    void setText (const juce::String& newText)
    {
        text = newText;
        laidOutWidth = -1;
        relayout();
        viewport.setViewPosition (0, 0);
    }

    void setFont (juce::Font newFont)                 { font = newFont; laidOutWidth = -1; relayout(); }
    void setColours (juce::Colour fill, juce::Colour ink, juce::Colour edge)
    {
        background = fill;
        textColour = ink;
        outline = edge;
        laidOutWidth = -1;
        relayout();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (background);
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        g.setColour (outline);
        g.drawRect (getLocalBounds());
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds().reduced (1));
        relayout();
    }

private:
    static constexpr int indent = 8;

    // Laid out at the width there is beside a scroll bar, whether one shows
    // or not, so the text does not rewrap as it appears.
    void relayout()
    {
        const auto width = viewport.getWidth() - viewport.getScrollBarThickness() - 2 * indent;

        if (width <= 0 || width == laidOutWidth)
            return;

        laidOutWidth = width;

        juce::AttributedString attributed;
        attributed.setWordWrap (juce::AttributedString::byWord);
        attributed.append (text, font, textColour);

        layout.createLayout (attributed, (float) width);
        body.setSize (viewport.getWidth() - viewport.getScrollBarThickness(),
                      juce::roundToInt (layout.getHeight()) + 2 * indent);
        body.repaint();
    }

    struct Body : juce::Component
    {
        explicit Body (ReadOnlyText& o) : owner (o)  { setInterceptsMouseClicks (false, false); }

        void paint (juce::Graphics& g) override
        {
            owner.layout.draw (g, getLocalBounds().toFloat().reduced ((float) indent));
        }

        ReadOnlyText& owner;
    };

    juce::String text;
    juce::Font font { juce::FontOptions() };
    juce::Colour background, textColour { juce::Colours::white }, outline;
    juce::TextLayout layout;
    int laidOutWidth = -1;
    Body body { *this };
    juce::Viewport viewport;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReadOnlyText)
};

// The sound library: what can be downloaded, what is installed, and each
// pack's credits and terms. A list on the left; on the right the selected
// pack, its buttons, and the author's own terms in full, which are on screen
// before the Download button is pressed.
//
//   Download / Update / Retry / Cancel   by state
//   Use                                  a new generator with it, once installed
//   Re-download, Delete                  for an installed copy
class SoundLibraryComponent : public juce::Component,
                              private juce::ListBoxModel,
                              private juce::ChangeListener
{
public:
    SoundLibraryComponent (SoundLibrary&, std::function<void (SoundLibrary::Kind, const juce::File&)> onUse);
    ~SoundLibraryComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Opens on this pack, or on the first one.
    void select (const juce::String& id);

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuild();
    void updateDetail();
    void loadTerms();
    void primaryClicked();
    void confirmDelete();
    juce::String describeStatus (const SoundLibrary::Pack&, const SoundLibrary::Status&) const;

    SoundLibrary& library;
    std::function<void (SoundLibrary::Kind, const juce::File&)> onUse;

    std::vector<SoundLibrary::Pack> packs;
    juce::String selectedId, termsShownFor;

    juce::Label listStateLabel;
    juce::TextButton refreshButton { "Refresh" };
    juce::ListBox list;

    juce::Label nameLabel, authorLabel, statusLabel, noteLabel;
    juce::HyperlinkButton homepageLink;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::TextButton primaryButton, useButton { "Use in New Generator" },
                     redownloadButton { "Re-download" }, deleteButton { "Delete" }, revealButton;
    ReadOnlyText terms;

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundLibraryComponent)
};

class SoundLibraryWindow : public juce::DocumentWindow
{
public:
    SoundLibraryWindow (SoundLibrary&, std::function<void (SoundLibrary::Kind, const juce::File&)> onUse,
                        std::function<void()> onClose);

    void closeButtonPressed() override;
    void select (const juce::String& id)  { content.select (id); }

private:
    std::function<void()> onClose;
    SoundLibraryComponent content;
};

} // namespace carve::app
