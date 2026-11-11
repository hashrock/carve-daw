#pragma once

#include <array>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "TouchUi.h"
#include "Fonts.h"
#include "IconButton.h"
#include "PianoRollComponent.h"
#include "PlayModeGlyphs.h"
#include "ShortcutHelpBar.h"

namespace carve::app
{

// The quantise settings, shown in a call-out from the toolbar button.
//
// Not in the toolbar itself: two sliders and a button do not fit beside
// everything already there, and quantise is a command rather than a mode --
// the panel is opened, aimed, fired and dismissed. What it aims is kept on the
// roll rather than here, so the Q key can repeat it with the panel shut.
class QuantisePanel : public juce::Component
{
public:
    QuantisePanel (double strength, double swing, const juce::String& gridName)
    {
        strengthHeader.setText ("Strength", juce::dontSendNotification);
        swingHeader.setText ("Swing", juce::dontSendNotification);

        for (auto* header : { &strengthHeader, &swingHeader })
        {
            header->setFont (uiFont (fonts::small));
            header->setJustificationType (juce::Justification::centredRight);
        }

        for (auto* slider : { &strengthSlider, &swingSlider })
        {
            slider->setSliderStyle (juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
            slider->setTextValueSuffix ("%");
        }

        strengthSlider.setRange (0.0, 100.0, 1.0);

        // Swing on the scale every sequencer prints it on: 50% is straight
        // and 66% the triplet feel (see gestures::QuantiseSettings). The top
        // is the dotted feel, which is as far as anyone calls it swing.
        swingSlider.setRange (50.0, 75.0, 1.0);

        strengthSlider.setValue (strength * 100.0, juce::dontSendNotification);
        swingSlider.setValue (swing * 100.0, juce::dontSendNotification);

        // The unit quantising moves notes to is the toolbar's grid, not a
        // setting of its own: the grid is already what the user has been
        // drawing against, and two units to keep in step would be one too many.
        unitLabel.setText ("to the " + gridName + " grid", juce::dontSendNotification);
        unitLabel.setFont (uiFont (fonts::small));
        unitLabel.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));

        applyButton.onClick = [this]
        {
            if (onApply)
                onApply (strengthSlider.getValue() / 100.0, swingSlider.getValue() / 100.0);

            // a call-out is a one-shot: it has done what it was opened for
            if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
                box->dismiss();
        };

        for (auto* c : std::initializer_list<juce::Component*> {
                 &strengthHeader, &strengthSlider, &swingHeader, &swingSlider,
                 &unitLabel, &applyButton })
            addAndMakeVisible (c);

        setSize (290, 96);
    }

    std::function<void (double strength, double swing)> onApply;

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);

        auto row = [&area] (juce::Label& header, juce::Slider& slider)
        {
            auto line = area.removeFromTop (24);
            header.setBounds (line.removeFromLeft (56));
            line.removeFromLeft (4);
            slider.setBounds (line);
            area.removeFromTop (4);
        };

        row (strengthHeader, strengthSlider);
        row (swingHeader, swingSlider);

        auto footer = area.removeFromTop (24);
        applyButton.setBounds (footer.removeFromRight (72));
        footer.removeFromRight (6);
        unitLabel.setBounds (footer);
    }

private:
    juce::Label strengthHeader, swingHeader, unitLabel;
    juce::Slider strengthSlider, swingSlider;
    juce::TextButton applyButton { "Apply" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuantisePanel)
};

// Content of the pattern editor window: a toolbar over a scrolling piano roll,
// with a velocity lane under it and a shortcut help bar along the bottom.
//
// The toolbar mixes two kinds of setting. The pattern name and length belong to
// the song, so they go through the model and the UndoManager; the tool, the
// zoom, the grid unit and the snap toggle are view state that only the roll
// cares about, so they never touch the model and are not undoable. Snap is
// remembered across launches, in a settings file of its own beside the
// mixer's and the browser's: whoever turns it off means it as a way of
// working, not as a choice about one pattern.
//
// The length is stepped in bars, which is how a pattern is thought about, but
// bars are not a property of a pattern: the model stores a length in beats, and
// the same pattern can be placed under two different time signatures. So the
// spinner names the signature it counts in -- the roll's, i.e. the one the song
// opens in -- the beat count is always spelled out beside it, and a song that
// does change signature says that its bars vary.
// A break slicer's strip under the toolbar: how notes play their slices.
// Three groups, left to right, in the order a note is shaped:
//
//   Play  how the slice sounds -- one button per mode, each showing the
//         mark the note will carry; a buzz's note and ramp beside it,
//         shown only while a buzz is what is chosen
//   Roll  restarting it -- the rate, and the hairpin, shown only when
//         there is a roll to shape
//   Tune  up or down, in semitones
//
// and at the right, what all of it applies to: the selected notes, or with
// nothing selected the next note drawn, plus a reset back to a plain note.
// Reports choices; the roll applies them.
class PlayModeBar : public juce::Component
{
public:
    static constexpr int preferredHeight = 30;

    using Mode = model::Note::PlayMode;
    using Dynamics = model::Note::RollDynamics;

    std::function<void (Mode)> onPlayModeChosen;
    std::function<void (int divisionsPerBeat)> onRetriggerChosen;
    std::function<void (int midiNote)> onBuzzPitchChosen;
    std::function<void (bool)> onBuzzRampChosen;
    std::function<void (int semitones)> onTuneChosen;
    std::function<void (Dynamics)> onRollDynamicsChosen;
    std::function<void()> onReset;

    PlayModeBar()
    {
        for (auto* header : { &playHeader, &rollHeader, &tuneHeader })
        {
            header->setFont (uiFont (fonts::small));
            header->setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
            header->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (header);
        }

        playHeader.setText ("Play", juce::dontSendNotification);
        rollHeader.setText ("Roll", juce::dontSendNotification);
        tuneHeader.setText ("Tune", juce::dontSendNotification);

        // Play: the modes, as the marks they leave on a note.
        const char* modeTips[] = {
            "Forward: the slice as recorded",
            "Reverse: the slice backwards, from the next cut to its own",
            "Tape Stop: slowing to a halt over the note, like a tape machine losing power",
            "Fade In: rising from silence to full over the note",
            "Fade Out: falling from full to silence over the note",
            "Buzz: the slice's head restarted so fast it sounds as a note",
        };

        for (size_t i = 0; i < modes.size(); ++i)
        {
            const auto mode = modes[i];
            auto button = std::make_unique<playmode::GlyphButton> (modeTips[i],
                [this, mode] (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour ink)
                {
                    playmode::drawGlyph (g, mode, box, mode == Mode::buzz && buzzRampToggle.getToggleState(), ink);
                });
            styleSegment (*button, i, modes.size());
            button->onClick = [this, mode] { if (onPlayModeChosen) onPlayModeChosen (mode); };
            addAndMakeVisible (*button);
            modeButtons.push_back (std::move (button));
        }

        // The buzz's own settings, beside it.
        for (int note = model::Note::lowestBuzzPitch; note <= model::Note::highestBuzzPitch; ++note)
            buzzPitchBox.addItem (juce::MidiMessage::getMidiNoteName (note, true, true, 3), note);

        buzzPitchBox.setTooltip ("The note the buzz sounds, the same at any tempo");
        buzzPitchBox.setWantsKeyboardFocus (false);
        buzzPitchBox.onChange = [this]
        {
            if (onBuzzPitchChosen && buzzPitchBox.getSelectedId() > 0)
                onBuzzPitchChosen (buzzPitchBox.getSelectedId());
        };
        addChildComponent (buzzPitchBox);

        buzzRampToggle.setTooltip ("Start as a sixteenth-note roll and speed up into the note over its length");
        buzzRampToggle.setWantsKeyboardFocus (false);
        buzzRampToggle.onClick = [this] { if (onBuzzRampChosen) onBuzzRampChosen (buzzRampToggle.getToggleState()); };
        addChildComponent (buzzRampToggle);

        // Roll: the rate, then the hairpin.
        rollBox.addItem ("Off", rollIdFor (0));
        const char* rollNames[] = { "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64" };

        for (size_t i = 0; i < std::size (model::Note::retriggerDivisions); ++i)
            rollBox.addItem (rollNames[i], rollIdFor (model::Note::retriggerDivisions[i]));

        rollBox.setTooltip ("Restart the slice at this rate for as long as the note lasts");
        rollBox.setWantsKeyboardFocus (false);
        rollBox.onChange = [this]
        {
            if (onRetriggerChosen && rollBox.getSelectedId() > 0)
                onRetriggerChosen (rollBox.getSelectedId() - 1);
        };
        addAndMakeVisible (rollBox);

        const char* dynamicsTips[] = { "Every restart as loud", "Crescendo: rising out of near silence",
                                       "Decrescendo: dying away" };

        for (size_t i = 0; i < dynamics.size(); ++i)
        {
            const auto shape = dynamics[i];
            auto button = std::make_unique<playmode::GlyphButton> (dynamicsTips[i],
                [shape] (juce::Graphics& g, juce::Rectangle<float> box, juce::Colour ink)
                {
                    playmode::drawDynamicsGlyph (g, shape, box, ink);
                });
            styleSegment (*button, i, dynamics.size());
            button->onClick = [this, shape] { if (onRollDynamicsChosen) onRollDynamicsChosen (shape); };
            addChildComponent (*button);
            dynamicsButtons.push_back (std::move (button));
        }

        // Tune: a stepper, double-click for none.
        tuneSlider.setSliderStyle (juce::Slider::IncDecButtons);
        tuneSlider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        tuneSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 34, 22);
        tuneSlider.setRange (-model::Note::maxTune, model::Note::maxTune, 1.0);
        tuneSlider.setDoubleClickReturnValue (true, 0.0);
        tuneSlider.textFromValueFunction = [] (double value)
        {
            const auto semitones = juce::roundToInt (value);
            return (semitones > 0 ? "+" : "") + juce::String (semitones);
        };
        tuneSlider.valueFromTextFunction = [] (const juce::String& text) { return text.getDoubleValue(); };
        tuneSlider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff2c2c31));
        tuneSlider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a3a40));
        tuneSlider.setTooltip ("This note's slice higher or lower, in semitones -- faster or slower, as a sampler "
                               "would. Double-click for none");
        tuneSlider.onValueChange = [this]
        {
            if (! updating && onTuneChosen)
                onTuneChosen (juce::roundToInt (tuneSlider.getValue()));
        };
        addAndMakeVisible (tuneSlider);

        // What it all applies to, and the way back to plain.
        target.setFont (uiFont (fonts::small));
        target.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
        target.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (target);

        resetButton.setTooltip ("Back to a plain note: forward, no roll, no tune");
        resetButton.setWantsKeyboardFocus (false);
        resetButton.onClick = [this] { if (onReset) onReset(); };
        addAndMakeVisible (resetButton);
    }

    // What the roll says is shown: nothing lit for a setting the selected
    // notes differ on.
    void show (std::optional<Mode> mode, std::optional<int> retrigger, std::optional<int> buzzPitch,
               std::optional<bool> buzzRamped, std::optional<int> tune, std::optional<Dynamics> shape,
               int numSelected)
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);

        for (size_t i = 0; i < modes.size(); ++i)
            modeButtons[i]->setToggleState (mode && *mode == modes[i], juce::dontSendNotification);

        buzzPitchBox.setSelectedId (buzzPitch ? *buzzPitch : 0, juce::dontSendNotification);
        buzzRampToggle.setToggleState (buzzRamped.value_or (false), juce::dontSendNotification);

        rollBox.setSelectedId (retrigger ? rollIdFor (*retrigger) : 0, juce::dontSendNotification);

        for (size_t i = 0; i < dynamics.size(); ++i)
            dynamicsButtons[i]->setToggleState (shape && *shape == dynamics[i], juce::dontSendNotification);

        tuneSlider.setValue (tune.value_or (0), juce::dontSendNotification);
        tuneSlider.setAlpha (tune ? 1.0f : 0.5f);   // differs across the selection

        target.setText (numSelected == 0 ? "next note"
                                         : numSelected == 1 ? "1 note" : juce::String (numSelected) + " notes",
                        juce::dontSendNotification);

        // Only what the choice above it makes meaningful.
        showsBuzzOptions = mode && *mode == Mode::buzz;
        showsDynamics = ! retrigger || *retrigger != 0;

        for (auto& button : modeButtons)
            button->repaint();   // the buzz mark follows the ramp

        resized();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff26262b));

        // A rule between the groups.
        g.setColour (juce::Colour (0xff3a3a40));

        for (auto x : dividers)
            g.drawVerticalLine (x, 6.0f, (float) getHeight() - 6.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 4);
        dividers.clear();

        auto divide = [&]
        {
            area.removeFromLeft (8);
            dividers.push_back (area.getX());
            area.removeFromLeft (9);
        };

        // The right end first: it is always there, and whatever is left in
        // the middle is what the groups get.
        resetButton.setBounds (area.removeFromRight (52));
        area.removeFromRight (6);
        target.setBounds (area.removeFromRight (64));

        playHeader.setBounds (area.removeFromLeft (30));
        area.removeFromLeft (6);

        for (auto& button : modeButtons)
            button->setBounds (area.removeFromLeft (30));

        buzzPitchBox.setVisible (showsBuzzOptions);
        buzzRampToggle.setVisible (showsBuzzOptions);

        if (showsBuzzOptions)
        {
            area.removeFromLeft (6);
            buzzPitchBox.setBounds (area.removeFromLeft (62));
            area.removeFromLeft (4);
            buzzRampToggle.setBounds (area.removeFromLeft (60));
        }

        divide();
        rollHeader.setBounds (area.removeFromLeft (28));
        area.removeFromLeft (6);
        rollBox.setBounds (area.removeFromLeft (72));

        for (auto& button : dynamicsButtons)
            button->setVisible (showsDynamics);

        if (showsDynamics)
        {
            area.removeFromLeft (6);

            for (auto& button : dynamicsButtons)
                button->setBounds (area.removeFromLeft (28));
        }

        divide();
        tuneHeader.setBounds (area.removeFromLeft (30));
        area.removeFromLeft (6);
        tuneSlider.setBounds (area.removeFromLeft (84));

        repaint();
    }

private:
    static int rollIdFor (int divisionsPerBeat)  { return divisionsPerBeat + 1; }

    static void styleSegment (juce::Button& button, size_t index, size_t count)
    {
        button.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe08a3c));
        button.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
        button.setConnectedEdges ((index > 0 ? juce::Button::ConnectedOnLeft : 0)
                                  | (index + 1 < count ? juce::Button::ConnectedOnRight : 0));
    }

    const std::array<Mode, 6> modes { Mode::forward, Mode::reverse, Mode::tapeStop,
                                      Mode::fadeIn, Mode::fadeOut, Mode::buzz };
    const std::array<Dynamics, 3> dynamics { Dynamics::flat, Dynamics::crescendo, Dynamics::decrescendo };

    juce::Label playHeader, rollHeader, tuneHeader, target;
    std::vector<std::unique_ptr<playmode::GlyphButton>> modeButtons, dynamicsButtons;
    juce::ComboBox buzzPitchBox, rollBox;
    juce::ToggleButton buzzRampToggle { "Ramp" };
    juce::Slider tuneSlider;
    juce::TextButton resetButton { "Reset" };

    bool showsBuzzOptions = false, showsDynamics = false, updating = false;
    std::vector<int> dividers;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayModeBar)
};

class PianoRollContent : public juce::Component,
                         private juce::ValueTree::Listener
{
public:
    PianoRollContent (te::Engine& engine, juce::UndoManager& um)
        : undoManager (um), settings (createSettingsFile (engine)), pianoRoll (um)
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

        // Text filled in by refreshFromPattern(), which is the only place that
        // knows the signature; it is a header rather than a suffix on the
        // spinner so that the unit reads before the number.
        lengthHeader.setJustificationType (juce::Justification::centredRight);

        lengthNote.setFont (uiFont (fonts::small));
        lengthNote.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));

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

        pianoRoll.setSnapEnabled (settings->getBoolValue (snapKey, pianoRoll.isSnapEnabled()));
        snapButton.setToggleState (pianoRoll.isSnapEnabled(), juce::dontSendNotification);
        snapButton.onClick = [this]
        {
            pianoRoll.setSnapEnabled (snapButton.getToggleState());
            settings->setValue (snapKey, snapButton.getToggleState());
            updateShortcutBar();   // the override key only means something while snap is on
        };

        drawToolButton.setTooltip ("Draw (D): click empty grid to add a note");
        selectToolButton.setTooltip ("Select (E): rubber-band notes, then move or delete them");
        zoomOutButton.setTooltip ("Zoom out (- key, or cmd-scroll)");
        zoomInButton.setTooltip ("Zoom in (= key, or cmd-scroll)");
        quantiseButton.setTooltip ("Quantise (Q): pull the whole pattern's note starts onto the grid, with swing");

        quantiseButton.setWantsKeyboardFocus (false);
        quantiseButton.onClick = [this] { showQuantisePanel(); };

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
        // The selection also decides what the velocity lane will let a drag
        // touch, and it says so by dimming the rest, so it redraws from here too.
        pianoRoll.onShortcutContextChanged = [this]
        {
            refreshPlayModeBar();
            updateToolStrip();
            updateShortcutBar();
            velocityLane.repaint();
        };

        // Zooming rescales the roll under the ruler and the lane, and nothing
        // else tells them that the arithmetic they copy has changed. The roll
        // fires this for a signature change too, for the same reason.
        pianoRoll.onViewChanged = [this] { updateStrips(); ruler.repaint(); velocityLane.repaint(); };

        ruler.onSeek = [this] (double beat)
        {
            if (onSeekPatternBeat)
                onSeekPatternBeat (beat);
        };

        // The lane is a sibling of the roll rather than a child of it, so the
        // roll's own repaint does not reach it.
        pianoRoll.onNotesChanged = [this] { velocityLane.repaint(); refreshPlayModeBar(); };

        playModeBar.onPlayModeChosen = [this] (model::Note::PlayMode mode)
        {
            pianoRoll.setPlayMode (mode);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onBuzzPitchChosen = [this] (int note)
        {
            pianoRoll.setBuzzPitch (note);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onBuzzRampChosen = [this] (bool ramped)
        {
            pianoRoll.setBuzzRamped (ramped);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onReset = [this]
        {
            pianoRoll.resetPlayModes();
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onTuneChosen = [this] (int semitones)
        {
            pianoRoll.setTune (semitones);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onRollDynamicsChosen = [this] (model::Note::RollDynamics dynamics)
        {
            pianoRoll.setRollDynamics (dynamics);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        playModeBar.onRetriggerChosen = [this] (int divisions)
        {
            pianoRoll.setRetrigger (divisions);
            refreshPlayModeBar();
            pianoRoll.grabKeyboardFocus();
        };
        addChildComponent (playModeBar);

        // A signature change moves what a bar means, so the spinner's value,
        // its unit and the beat count beside it all have to be rewritten.
        timeSigWatcher.onChanged = [this] { refreshFromPattern(); };

        // select the entry matching the roll's own default rather than
        // assuming an index, so the two cannot drift apart
        for (int i = 0; i < numGridOptions; ++i)
            if (std::abs (gridOptions[i].beats - pianoRoll.getGridBeats()) < 1.0e-9)
                gridBox.setSelectedId (i + 1, juce::dontSendNotification);

        viewport.setViewedComponent (&pianoRoll, false);
        viewport.setScrollBarsShown (true, true);
        leaveDragsToContent (viewport);
        viewport.onVisibleAreaChanged = [this] { updateStrips(); };

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &drawToolButton, &selectToolButton, &zoomOutButton, &zoomInButton,
                 &lengthHeader, &lengthSlider, &lengthNote, &gridHeader, &gridBox,
                 &snapButton, &quantiseButton, &ruler, &viewport, &velocityLane, &shortcutBar })
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

    // A press on the ruler asked for the transport to move to this pattern
    // beat. Where that is in the song is the owner's problem: the editor has
    // no idea which placements the pattern has.
    std::function<void (double patternBeat)> onSeekPatternBeat;

    // Where in this pattern the transport currently is, worked out by the
    // owner from the playlist; nothing while it plays elsewhere.
    void setPlayheadBeat (std::optional<double> beat)
    {
        pianoRoll.setPlayheadBeat (beat);
    }

    // The roll knows nothing about the engine, so whoever does supplies this.
    void setPreviewNoteCallback (std::function<void (int, int)> callback)
    {
        pianoRoll.onPreviewNote = std::move (callback);
    }

    // The song, for its time signatures alone -- the roll needs them for its
    // bar lines and the toolbar for the length spinner's unit. Until one
    // arrives both count 4/4, so the editor works with no song set.
    void setSong (model::Song newSong)
    {
        song = newSong;
        timeSigWatcher.setSong (song->state);
        pianoRoll.setSong (std::move (newSong));
        refreshFromPattern();
    }

    // Lyrics on the notes, for a singing generator.
    void setShowsLyrics (bool shouldShow)
    {
        pianoRoll.setShowsLyrics (shouldShow);
    }

    void setLyricCheck (PianoRollComponent::LyricCheck check)
    {
        pianoRoll.setLyricCheck (std::move (check));
    }

    // The play-mode bar and the notes' play-mode marks, for a break slicer.
    void setShowsPlayModes (bool shouldShow)
    {
        if (pianoRoll.showsPlayModes() == shouldShow)
            return;

        pianoRoll.setShowsPlayModes (shouldShow);
        playModeBar.setVisible (shouldShow);
        refreshPlayModeBar();
        resized();
    }

    // The rows' names, for a drum generator; empty for everything else.
    void setDrumMap (std::map<int, PianoRollComponent::DrumRow> map)
    {
        const auto before = pianoRoll.getDrumPitchRange();
        pianoRoll.setDrumMap (std::move (map));

        // The window scrolls itself when it opens, which is before it has been
        // told what it is showing; a map arriving is the first moment there is
        // anything to aim at. The range only moves when the generator does, so
        // this does not fight the user's own scrolling afterwards.
        if (const auto after = pianoRoll.getDrumPitchRange(); after && after != before)
            scrollToMiddleOfPitchRange();
    }

    void setPattern (std::optional<model::Pattern> newPattern)
    {
        // Retargeted to the pattern already on screen -- which the host does
        // after every sync -- there is nothing to refresh: the listener below
        // has been following its edits all along.
        if (pattern && newPattern && pattern->state == newPattern->state)
            return;

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
        // A drum generator's rows are its sounds, and they sit in one band an
        // octave or two wide: open on that rather than in the middle of a
        // keyboard whose interesting part is somewhere off the bottom.
        if (const auto drums = pianoRoll.getDrumPitchRange())
        {
            const auto top = pianoRoll.pitchToViewY (drums->getEnd());
            const auto bottom = pianoRoll.pitchToViewY (drums->getStart());
            const auto centre = (top + bottom) / 2;

            viewport.setViewPosition (0, juce::jmax (0, centre - viewport.getMaximumVisibleHeight() / 2));
            return;
        }

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
        auto toolbar = area.removeFromTop (toolbarHeight).reduced (6, toolStrip::verticalPadding);
        shortcutBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));

        if (playModeBar.isVisible())
            playModeBar.setBounds (area.removeFromTop (PlayModeBar::preferredHeight));

        // The ruler and the lane are placed by updateStrips(), which has to
        // keep them to the viewport's *visible* width; here they only reserve
        // their strips above and below it.
        area.removeFromBottom (PianoRollVelocityLane::preferredHeight);
        area.removeFromTop (PianoRollRuler::preferredHeight);
        viewport.setBounds (area);
        updateStrips();

        auto place = [&toolbar] (juce::Component& c, int width, int gap = 6)
        {
            c.setBounds (toolbar.removeFromLeft (width));
            toolbar.removeFromLeft (gap);
        };

        place (nameLabel, 180, 18);
        place (drawToolButton, toolStrip::toolWidth, 0);
        place (selectToolButton, toolStrip::toolWidth, 12);
        place (zoomOutButton, toolStrip::zoomWidth, 0);
        place (zoomInButton, toolStrip::zoomWidth, 18);
        place (lengthHeader, 74, 4);
        place (lengthSlider, 64, 6);
        place (lengthNote, 130, 18);
        place (gridHeader, 34, 4);
        place (gridBox, 74, 18);
        place (snapButton, 70, 12);
        place (quantiseButton, 74);
    }

private:
    static constexpr int toolbarHeight = toolStrip::height;
    static constexpr const char* snapKey = "snap";

    // Its own file rather than a second handle on the mixer's or the browser's:
    // two PropertiesFile objects on one path would each write the other's keys
    // back out stale. Same folder, so every setting the app has is in one
    // place (see EngineSetup.h).
    static std::unique_ptr<juce::PropertiesFile> createSettingsFile (te::Engine& engine)
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 500;

        return std::make_unique<juce::PropertiesFile> (
            engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("pianoroll.settings"),
            options);
    }

    // Keeps the ruler and the velocity lane over the part of the roll that is
    // actually on screen. Their width follows the viewport's visible area
    // rather than the whole component, so a vertical scrollbar appearing
    // cannot push either out of alignment with the grid between them.
    void updateStrips()
    {
        const auto visibleWidth = viewport.getMaximumVisibleWidth();

        ruler.setBounds (viewport.getX(), viewport.getY() - PianoRollRuler::preferredHeight,
                         visibleWidth, PianoRollRuler::preferredHeight);
        velocityLane.setBounds (viewport.getX(), viewport.getBottom(),
                                visibleWidth, PianoRollVelocityLane::preferredHeight);

        const auto offsetX = viewport.getViewPositionX();
        ruler.setScrollOffset (offsetX);
        velocityLane.setScrollOffset (offsetX);
    }

    // Opened from the toolbar button. The roll keeps the settings, so this is
    // only a way of aiming them: what it applies, Q afterwards repeats.
    void showQuantisePanel()
    {
        auto panel = std::make_unique<QuantisePanel> (pianoRoll.getQuantiseStrength(),
                                                      pianoRoll.getQuantiseSwing(),
                                                      gridBox.getText());

        panel->onApply = [safe = juce::Component::SafePointer<PianoRollContent> (this)]
                         (double strength, double swing)
        {
            if (safe == nullptr)
                return;

            safe->pianoRoll.setQuantiseSettings (strength, swing);
            safe->pianoRoll.quantiseNotes();

            // the call-out had the keyboard; the roll owns Q and Backspace
            safe->pianoRoll.grabKeyboardFocus();
        };

        juce::CallOutBox::launchAsynchronously (std::move (panel), quantiseButton.getBounds(), this);
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

    // Taken from the roll rather than restated, so the spinner and the bar
    // lines under it cannot disagree about where a bar ends.
    double getBeatsPerBar() const  { return pianoRoll.getBeatsPerBar(); }

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
        // this runs on exactly the changes the ruler and the lane care about -
        // a different pattern, or a new length - so it is where both refresh
        ruler.repaint();
        velocityLane.repaint();

        const auto hasPattern = pattern.has_value();
        nameLabel.setEnabled (hasPattern);
        lengthSlider.setEnabled (hasPattern);
        quantiseButton.setEnabled (hasPattern);
        updateLengthUnit();
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

        // A length the spinner cannot represent -- an off-bar pattern, or one
        // whose beats do not divide into the current signature's bar -- shows
        // rounded; it is only rewritten if the user actually nudges it, and the
        // exact beat count is spelled out beside it either way.
        lengthSlider.setValue (pattern->getLengthBeats() / getBeatsPerBar(), juce::dontSendNotification);
    }

    // Says what the spinner's bars are bars *of*, and what the length is in the
    // unit the model actually stores. Bars are the natural way to nudge a
    // pattern longer, but they are a reading of a length rather than the length
    // itself: this pattern may also be placed under a signature whose bar is a
    // different number of beats, and when the song has any signature changes at
    // all the note says so rather than implying the bar count travels with it.
    void updateLengthUnit()
    {
        const auto sig = pianoRoll.getGridTimeSig();

        lengthHeader.setText ("Bars of " + timeSigText (sig), juce::dontSendNotification);

        if (! pattern)
        {
            lengthNote.setText ({}, juce::dontSendNotification);
            return;
        }

        // The bars the spinner counts are the song's opening ones. If the song
        // changes signature later, this pattern may well be placed there, where
        // the same length is a different number of bars -- say so rather than
        // let the spinner imply its bar count travels with the pattern.
        const bool varies = song && songSignatureVaries (*song);

        lengthNote.setText ("= " + trimmedNumber (pattern->getLengthBeats()) + " beats"
                                + (varies ? ", bars vary" : ""),
                            juce::dontSendNotification);
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
        const bool extending = selection::isExtendModifier (mods);
        const bool bandExtending = selection::isRubberBandExtendModifier (mods);

        // Shift is the select tool while it is held, so the bar has to say what
        // the roll would actually do, not what the toolbar is set to.
        const bool selectTool = pianoRoll.getEffectiveTool (mods) == PianoRollComponent::Tool::select;

        std::vector<ShortcutHelpBar::Entry> entries;

        if (mods.isAltDown())
        {
            // alt beats the tool: whichever one is active, the drag erases
            entries.push_back ({ "drag", "erase notes" });
        }
        else if (selectTool)
        {
            // Cmd is the only key that makes a band add: Shift asked for the
            // select tool, and a Shift-band starts a fresh selection.
            entries.push_back ({ "drag", bandExtending ? "add to selection" : "select notes" });
            entries.push_back ({ "click note", extending ? "add / remove" : "select" });
            entries.push_back ({ "drag note", "move selection" });
            entries.push_back ({ "Cmd+drag note", "duplicate" });
        }
        else
        {
            entries.push_back ({ "click", "add note" });
            entries.push_back ({ "drag note", extending ? "extend selection" : "move" });
            entries.push_back ({ "Cmd+drag note", "duplicate" });
            entries.push_back ({ "Shift", "select tool" });
        }

        if (! mods.isAltDown())
            entries.push_back ({ "drag edge", "resize" });

        // Only while the toolbar has snap on: with it off there is nothing for
        // the key to override, and listing it would suggest otherwise.
        if (pianoRoll.isSnapEnabled() && ! mods.isAltDown())
            entries.push_back ({ "Ctrl", pianoRoll.isSnapActive (mods) ? "ignore snap" : "ignoring snap" });

        const bool hasSelection = pianoRoll.getNumSelectedNotes() > 0;

        // The lane edits the selection when there is one and whatever it
        // sweeps when there is not, so it is worth saying which is happening.
        entries.push_back ({ "drag lane", hasSelection ? "velocity of selection" : "velocity" });

        if (hasSelection)
        {
            entries.push_back ({ "Backspace", "delete selected" });
            entries.push_back ({ "right-click", "delete selected" });
            entries.push_back ({ "Cmd+C/X", "copy / cut" });
        }

        entries.push_back ({ "Q", "quantise pattern" });

        // Listed whether or not there is anything to paste: this bar is
        // rebuilt on every selection change, so a rubber-band drag would be
        // reading the system clipboard once per mouse event to find out.
        entries.push_back ({ "Cmd+V", "paste at pointer" });
        entries.push_back ({ "Cmd+A", "select all" });

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
        // already a legal bar count -- of the roll's signature, which is what
        // the header beside it says it is counting.
        const auto beats = lengthSlider.getValue() * getBeatsPerBar();

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

    void refreshPlayModeBar()
    {
        if (playModeBar.isVisible())
            playModeBar.show (pianoRoll.getShownPlayMode(), pianoRoll.getShownRetrigger(),
                              pianoRoll.getShownBuzzPitch(), pianoRoll.getShownBuzzRamped(),
                              pianoRoll.getShownTune(), pianoRoll.getShownRollDynamics(),
                              pianoRoll.getNumSelectedNotes());
    }

    juce::UndoManager& undoManager;
    std::unique_ptr<juce::PropertiesFile> settings;   // flushes itself on destruction
    std::optional<model::Pattern> pattern;
    std::optional<model::Song> song;
    TimeSigWatcher timeSigWatcher;

    juce::Label nameLabel, lengthHeader, lengthNote, gridHeader;
    juce::Slider lengthSlider;
    juce::ComboBox gridBox;
    juce::ToggleButton snapButton { "Snap" };
    IconButton drawToolButton { "Draw", Icon::pencil }, selectToolButton { "Select", Icon::select };
    IconButton zoomOutButton { "", Icon::zoomOut }, zoomInButton { "", Icon::zoomIn };
    juce::TextButton quantiseButton { "Quantise" };
    RollViewport viewport;
    PianoRollComponent pianoRoll;
    PianoRollRuler ruler { pianoRoll };
    PianoRollVelocityLane velocityLane { pianoRoll };
    ShortcutHelpBar shortcutBar;
    PlayModeBar playModeBar;
};

} // namespace carve::app
