#pragma once

#include <cmath>
#include <iterator>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PianoRollComponent.h"
#include "ShortcutHelpBar.h"

namespace orionish::app
{

// Content of the pattern editor window: a toolbar over a scrolling piano roll,
// with a shortcut help bar along the bottom.
//
// The toolbar mixes two kinds of setting. The pattern name and length belong to
// the song, so they go through the model and the UndoManager; the tool, the
// zoom, the grid unit and the snap toggle are view state that only the roll
// cares about, so they never touch the model and are not undoable.
class PianoRollContent : public juce::Component,
                         private juce::ValueTree::Listener
{
public:
    explicit PianoRollContent (juce::UndoManager& um)
        : undoManager (um), pianoRoll (um)
    {
        nameLabel.setEditable (false, true, false);
        nameLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff2c2c31));
        nameLabel.setColour (juce::Label::outlineColourId, juce::Colour (0xff3a3a40));
        nameLabel.onTextChange = [this] { applyName(); };

        // Steppers rather than a plain field: nudging the pattern a bar longer
        // is the common edit, and typing a number is the rare one.
        lengthSlider.setSliderStyle (juce::Slider::IncDecButtons);
        lengthSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        lengthSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 30, 22);
        lengthSlider.setRange (1.0, maxBars, 1.0);
        lengthSlider.setNumDecimalPlacesToDisplay (0);
        lengthSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff2c2c31));
        lengthSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3a40));
        lengthSlider.onValueChange = [this] { applyLength(); };

        lengthHeader.setText ("Bars", juce::dontSendNotification);
        lengthHeader.setJustificationType (juce::Justification::centredRight);

        gridHeader.setText ("Grid", juce::dontSendNotification);
        gridHeader.setJustificationType (juce::Justification::centredRight);

        for (int i = 0; i < numGridOptions; ++i)
            gridBox.addItem (gridOptions[i].name, i + 1);

        gridBox.onChange = [this]
        {
            const auto index = gridBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (index, numGridOptions))
                pianoRoll.setGridBeats (gridOptions[index].beats);
        };

        snapButton.setToggleState (pianoRoll.isSnapEnabled(), juce::dontSendNotification);
        snapButton.onClick = [this] { pianoRoll.setSnapEnabled (snapButton.getToggleState()); };

        drawToolButton.setTooltip ("Draw (D): click empty grid to add a note");
        selectToolButton.setTooltip ("Select (E): rubber-band notes, then move or delete them");
        zoomOutButton.setTooltip ("Zoom out (- key, or cmd-scroll)");
        zoomInButton.setTooltip ("Zoom in (= key, or cmd-scroll)");

        for (auto* b : { &drawToolButton, &selectToolButton, &zoomOutButton, &zoomInButton })
        {
            b->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe08a3c));
            b->setColour (juce::TextButton::textColourOnId, juce::Colours::black);
            // Buttons never take focus themselves: the roll owns the tool keys
            // and Backspace, and clicking one must not steal the keyboard.
            b->setWantsKeyboardFocus (false);
        }

        drawToolButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        selectToolButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
        zoomOutButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        zoomInButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

        drawToolButton.onClick = [this] { pianoRoll.setTool (PianoRollComponent::Tool::draw); };
        selectToolButton.onClick = [this] { pianoRoll.setTool (PianoRollComponent::Tool::select); };
        zoomOutButton.onClick = [this] { pianoRoll.zoomBy (1.0 / 1.5); };
        zoomInButton.onClick = [this] { pianoRoll.zoomBy (1.5); };

        // The tool can also change from the keyboard, and the modifiers change
        // without any click at all, so the strip and the help bar are refreshed
        // from the roll rather than from whatever was pressed.
        pianoRoll.onShortcutContextChanged = [this] { updateToolStrip(); updateShortcutBar(); };

        // Zooming rescales the roll under the ruler, and nothing else tells the
        // ruler that the arithmetic it copies has changed.
        pianoRoll.onViewChanged = [this] { updateRuler(); ruler.repaint(); };

        // select the entry matching the roll's own default rather than
        // assuming an index, so the two cannot drift apart
        for (int i = 0; i < numGridOptions; ++i)
            if (std::abs (gridOptions[i].beats - pianoRoll.getGridBeats()) < 1.0e-9)
                gridBox.setSelectedId (i + 1, juce::dontSendNotification);

        viewport.setViewedComponent (&pianoRoll, false);
        viewport.setScrollBarsShown (true, true);
        viewport.onVisibleAreaChanged = [this] { updateRuler(); };

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &drawToolButton, &selectToolButton, &zoomOutButton, &zoomInButton,
                 &lengthHeader, &lengthSlider, &gridHeader, &gridBox,
                 &snapButton, &ruler, &viewport, &shortcutBar })
            addAndMakeVisible (c);

        updateToolStrip();
        refreshFromPattern();
    }

    ~PianoRollContent() override
    {
        if (pattern)
            pattern->state.removeListener (this);
    }

    // fired after a rename so the window can refresh its title bar
    std::function<void (const juce::String&)> onPatternRenamed;

    // The roll knows nothing about the engine, so whoever does supplies this.
    void setPreviewNoteCallback (std::function<void (int, int)> callback)
    {
        pianoRoll.onPreviewNote = std::move (callback);
    }

    void setPattern (std::optional<model::Pattern> newPattern)
    {
        if (pattern)
            pattern->state.removeListener (this);

        pattern = newPattern;

        if (pattern)
            pattern->state.addListener (this);

        pianoRoll.setPattern (std::move (newPattern));
        refreshFromPattern();
    }

    // Start scrolled to the middle of the pitch range, where the notes usually
    // are. Only meaningful once the viewport has a size.
    void scrollToMiddleOfPitchRange()
    {
        viewport.setViewPosition (0, juce::jmax (0, pianoRoll.getHeight() / 2 - 200));
    }

    // The roll owns the tool keys, Backspace and the zoom keys, so it starts
    // with the keyboard rather than making the user click the grid first.
    // Only works once the window is on screen.
    void focusRoll()
    {
        pianoRoll.grabKeyboardFocus();
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (juce::Colour (0xff2a2a2f));
        g.fillRect (getLocalBounds().removeFromTop (toolbarHeight));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto toolbar = area.removeFromTop (toolbarHeight).reduced (6, 5);
        shortcutBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));
        area.removeFromTop (PianoRollRuler::preferredHeight);
        viewport.setBounds (area);
        updateRuler();

        auto place = [&toolbar] (juce::Component& c, int width, int gap = 6)
        {
            c.setBounds (toolbar.removeFromLeft (width));
            toolbar.removeFromLeft (gap);
        };

        place (nameLabel, 180, 18);
        place (drawToolButton, 52, 0);
        place (selectToolButton, 52, 12);
        place (zoomOutButton, 24, 0);
        place (zoomInButton, 24, 18);
        place (lengthHeader, 34, 4);
        place (lengthSlider, 64, 18);
        place (gridHeader, 34, 4);
        place (gridBox, 74, 18);
        place (snapButton, 70);
    }

private:
    static constexpr int toolbarHeight = 34;

    // Keeps the ruler over the part of the roll that is actually on screen.
    // Its width follows the viewport's visible area rather than the whole
    // component, so a vertical scrollbar appearing cannot push it out of
    // alignment with the grid underneath.
    void updateRuler()
    {
        ruler.setBounds (viewport.getX(), viewport.getY() - PianoRollRuler::preferredHeight,
                         viewport.getMaximumVisibleWidth(), PianoRollRuler::preferredHeight);
        ruler.setScrollOffset (viewport.getViewPositionX());
    }

    // juce::Viewport only reports scrolling through this virtual, so the ruler
    // needs a subclass to hear about it.
    struct RollViewport : juce::Viewport
    {
        std::function<void()> onVisibleAreaChanged;

        void visibleAreaChanged (const juce::Rectangle<int>&) override
        {
            if (onVisibleAreaChanged)
                onVisibleAreaChanged();
        }
    };

    // The model has no time signature, and the roll already draws its bar
    // lines every four beats, so the toolbar counts bars the same way - taken
    // from the roll rather than restated, so the two cannot drift apart.
    static constexpr double beatsPerBar = PianoRollComponent::beatsPerBar;
    static constexpr double maxBars = 256.0;

    struct GridOption { const char* name; double beats; };
    static constexpr GridOption gridOptions[] = {
        { "1/4",   1.0 },
        { "1/8",   0.5 },
        { "1/16",  0.25 },
        { "1/32",  0.125 },
        { "1/8T",  1.0 / 3.0 },
        { "1/16T", 1.0 / 6.0 },
    };
    static constexpr int numGridOptions = (int) std::size (gridOptions);

    // Only the pattern's own properties matter here; note edits arrive on the
    // NOTE children and would just churn the toolbar text.
    void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (pattern && tree == pattern->state
             && (property == model::ids::name || property == model::ids::lengthBeats))
            refreshFromPattern();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override         {}
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override  {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override          {}
    void valueTreeParentChanged (juce::ValueTree&) override                        {}

    void refreshFromPattern()
    {
        // this runs on exactly the changes the ruler cares about - a different
        // pattern, or a new length - so it is also where the ruler is refreshed
        ruler.repaint();

        const auto hasPattern = pattern.has_value();
        nameLabel.setEnabled (hasPattern);
        lengthSlider.setEnabled (hasPattern);
        updateShortcutBar();

        if (! hasPattern)
        {
            nameLabel.setText ("(no pattern)", juce::dontSendNotification);
            return;
        }

        // dontSendNotification so writing the value back does not re-enter
        // applyName()/applyLength()
        if (! nameLabel.isBeingEdited())
            nameLabel.setText (pattern->getName(), juce::dontSendNotification);

        // A length the spinner cannot represent (an old off-bar pattern) shows
        // rounded; it is only rewritten if the user actually nudges it.
        lengthSlider.setValue (pattern->getLengthBeats() / beatsPerBar, juce::dontSendNotification);
    }

    void updateToolStrip()
    {
        const auto tool = pianoRoll.getTool();
        drawToolButton.setToggleState (tool == PianoRollComponent::Tool::draw, juce::dontSendNotification);
        selectToolButton.setToggleState (tool == PianoRollComponent::Tool::select, juce::dontSendNotification);
    }

    // The help bar describes what works *here, now*: the entries change with
    // the tool, with the selection, and with a modifier the user is already
    // holding, because that modifier decides what the next drag will do.
    void updateShortcutBar()
    {
        if (! pattern)
        {
            shortcutBar.setEntries ({ { "double click", "a playlist clip to edit its pattern" } });
            return;
        }

        const auto mods = juce::ModifierKeys::getCurrentModifiers();
        const bool extending = mods.isCommandDown() || mods.isShiftDown();
        const bool selectTool = pianoRoll.getTool() == PianoRollComponent::Tool::select;

        std::vector<ShortcutHelpBar::Entry> entries;

        if (mods.isAltDown())
        {
            // alt beats the tool: whichever one is active, the drag erases
            entries.push_back ({ "drag", "erase notes" });
        }
        else if (selectTool)
        {
            entries.push_back ({ "drag", extending ? "add to selection" : "select notes" });
            entries.push_back ({ "click note", extending ? "add / remove" : "select" });
            entries.push_back ({ "drag note", "move selection" });
        }
        else
        {
            entries.push_back ({ "click", "add note" });
            entries.push_back ({ "drag note", extending ? "extend selection" : "move" });
        }

        if (! mods.isAltDown())
            entries.push_back ({ "drag edge", "resize" });

        if (pianoRoll.getNumSelectedNotes() > 0)
            entries.push_back ({ "Backspace", "delete selected" });

        entries.push_back ({ selectTool ? "D" : "E", selectTool ? "draw tool" : "select tool" });

        if (! mods.isAltDown())
            entries.push_back ({ "Alt+drag", "erase" });

        entries.push_back ({ "Cmd+scroll", "zoom" });

        shortcutBar.setEntries (std::move (entries));
    }

    void applyName()
    {
        if (! pattern)
            return;

        const auto name = nameLabel.getText().trim();

        if (name.isNotEmpty() && name != pattern->getName())
        {
            undoManager.beginNewTransaction();
            pattern->setName (name, &undoManager);

            if (onPatternRenamed)
                onPatternRenamed (name);
        }

        refreshFromPattern();   // rewrite the text if the edit was rejected
    }

    void applyLength()
    {
        if (! pattern)
            return;

        // The slider clamps to its own range, so anything that arrives here is
        // already a legal bar count.
        const auto beats = lengthSlider.getValue() * beatsPerBar;

        // Shrinking leaves notes past the new end in the pattern: they are kept
        // deliberately, so that undoing a mistaken shorten restores everything.
        // One transaction per step of the spinner, so each nudge undoes on its
        // own rather than joining whatever gesture came before it.
        if (! juce::exactlyEqual (beats, pattern->getLengthBeats()))
        {
            undoManager.beginNewTransaction();
            pattern->setLengthBeats (beats, &undoManager);
        }

        refreshFromPattern();
    }

    juce::UndoManager& undoManager;
    std::optional<model::Pattern> pattern;

    juce::Label nameLabel, lengthHeader, gridHeader;
    juce::Slider lengthSlider;
    juce::ComboBox gridBox;
    juce::ToggleButton snapButton { "Snap" };
    juce::TextButton drawToolButton { "Draw" }, selectToolButton { "Select" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" };
    RollViewport viewport;
    PianoRollComponent pianoRoll;
    PianoRollRuler ruler { pianoRoll };
    ShortcutHelpBar shortcutBar;
};

// Floating pattern editor (Orion-style): the playlist stays in the main
// window and each pattern is edited in this popup. Shows whichever pattern
// is currently selected.
class PianoRollWindow : public juce::DocumentWindow
{
public:
    PianoRollWindow (juce::UndoManager& um,
                     std::function<void()> onCloseCallback,
                     std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Pattern Editor",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          onKey (std::move (keyHandler)),
          content (um)
    {
        content.onPatternRenamed = [this] (const juce::String& name) { updateTitle (name); };
        // the extra height is the ruler and the help bar, so the roll itself
        // keeps its old size
        content.setSize (860, 554 + PianoRollRuler::preferredHeight + ShortcutHelpBar::preferredHeight);

        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);

        content.scrollToMiddleOfPitchRange();
        content.focusRoll();
    }

    void setPreviewNoteCallback (std::function<void (int, int)> callback)
    {
        content.setPreviewNoteCallback (std::move (callback));
    }

    void setPattern (std::optional<model::Pattern> pattern, const juce::String& title)
    {
        // The owner builds the title as "<generator> / <pattern>". Keep the
        // part in front of the pattern name so that renaming from in here can
        // refresh the title bar without knowing about generators.
        titlePrefix = {};
        juce::String name;

        if (pattern)
        {
            name = pattern->getName();
            if (name.isNotEmpty() && title.endsWith (name))
                titlePrefix = title.dropLastCharacters (name.length());
        }

        content.setPattern (std::move (pattern));
        updateTitle (name);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (onKey != nullptr && onKey (key))
            return true;
        return juce::DocumentWindow::keyPressed (key);
    }

private:
    void updateTitle (const juce::String& patternName)
    {
        // ASCII only: juce::String (const char*) asserts above 127.
        setName (patternName.isNotEmpty() ? "Pattern Editor - " + titlePrefix + patternName
                                          : "Pattern Editor");
    }

    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    juce::String titlePrefix;
    PianoRollContent content;
};

} // namespace orionish::app
