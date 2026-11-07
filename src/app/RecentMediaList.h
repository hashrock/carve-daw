#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// What an empty instrument is waiting for: a sample, an .sf2, a voicebank
// folder, a drum loop to slice. Each kind keeps its own history (see
// GeneratorController) -- the loops a slicer was given are not the one-shots
// a sampler was.
enum class MediaKind { sample, soundFont, voicebank, breakLoop };

// The files (or voicebank folders) of one kind used most recently, for an
// empty instrument's Inst tab: a click loads one, so filling a new sampler
// with the sample used a minute ago is one click rather than a trip through
// a file chooser. The chooser is still a button away, for anything new.
class RecentMediaList : public juce::Component,
                        private juce::ListBoxModel
{
public:
    struct Item
    {
        juce::File file;
        juce::String title;   // a file's name, a voicebank's own name
    };

    std::function<void (const juce::File&)> onChosen;

    RecentMediaList()
    {
        heading.setText ("Recent", juce::dontSendNotification);
        heading.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
        addAndMakeVisible (heading);

        emptyNote.setText ("Nothing used yet", juce::dontSendNotification);
        emptyNote.setColour (juce::Label::textColourId, juce::Colour (0xff5e5e68));
        emptyNote.setJustificationType (juce::Justification::centred);
        addChildComponent (emptyNote);

        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1c1c20));
        list.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff3a3a40));
        list.setOutlineThickness (1);
        list.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        list.addMouseListener (this, true);   // the hover light: the rows are the list's children
        addAndMakeVisible (list);
        emptyNote.toFront (false);   // over the empty list, not under it
    }

    ~RecentMediaList() override
    {
        list.setModel (nullptr);
    }

    void setItems (std::vector<Item> newItems)
    {
        items = std::move (newItems);
        list.updateContent();
        list.deselectAllRows();
        list.repaint();
        emptyNote.setVisible (items.empty());
    }

    void resized() override
    {
        auto area = getLocalBounds();
        heading.setBounds (area.removeFromTop (20));
        area.removeFromTop (4);
        list.setBounds (area);
        emptyNote.setBounds (area);
    }

private:
    int getNumRows() override  { return (int) items.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
    {
        if (row < 0 || row >= (int) items.size())
            return;

        const auto& item = items[(size_t) row];

        if (row == hoverRow)
            g.fillAll (juce::Colour (0xff2c3a4e));
        else if (row % 2 == 1)
            g.fillAll (juce::Colour (0xff202024));

        g.setColour (juce::Colour (0xffe0e0e6));
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (item.title, 10, 2, width - 20, height / 2, juce::Justification::bottomLeft, true);

        g.setColour (juce::Colour (0xff6e6e78));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (item.file.getParentDirectory().getFullPathName(), 10, height / 2, width - 20, height / 2 - 2,
                    juce::Justification::topLeft, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        if (row >= 0 && row < (int) items.size() && onChosen)
            onChosen (items[(size_t) row].file);
    }

    // The row under the pointer lights up, so the list reads as a set of
    // things to click rather than a record of what happened.
    juce::String getTooltipForRow (int row) override
    {
        return row >= 0 && row < (int) items.size() ? items[(size_t) row].file.getFullPathName() : juce::String();
    }

    void mouseMove (const juce::MouseEvent& e) override  { setHoverRow (list.getRowContainingPosition (e.getEventRelativeTo (&list).x, e.getEventRelativeTo (&list).y)); }
    void mouseExit (const juce::MouseEvent&) override    { setHoverRow (-1); }

    void setHoverRow (int row)
    {
        if (hoverRow != row)
        {
            hoverRow = row;
            list.repaint();
        }
    }

    std::vector<Item> items;
    int hoverRow = -1;

    juce::Label heading, emptyNote;
    juce::ListBox list;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RecentMediaList)
};

} // namespace carve::app
