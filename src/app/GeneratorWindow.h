#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FourOscEditor.h"
#include "PianoRollWindow.h"
#include "PresetManager.h"
#include "model/SongModel.h"

namespace carve::app
{

// The Orion-style bank switcher: one row of bank letters, one row of slot
// numbers, replacing the grid the generator panel used to carry. Selection
// itself stays the panel's job -- a click only reports which slot was asked
// for, and the switcher redraws when the model answers.
class SlotSwitcher : public juce::Component
{
public:
    SlotSwitcher() = default;

    std::function<void (model::PatternSlot)> onSlotClicked;
    std::function<void (model::PatternSlot, juce::Rectangle<int>)> onSlotMenu;   // screen coords

    static constexpr int preferredHeight = 24;

    void setFromGenerator (const std::optional<model::Generator>& generator,
                           const juce::String& selectedPatternId)
    {
        for (auto& state : slotStates)
            state = SlotState::unused;

        selected.reset();
        enabled = generator.has_value() && ! generator->isAudio();

        if (generator)
        {
            for (const auto& pattern : generator->getPatterns())
            {
                if (auto slot = pattern.getSlot())
                {
                    slotStates[(size_t) slot->toFlatIndex()] = pattern.isEmpty()
                                                                  ? SlotState::empty
                                                                  : SlotState::hasNotes;
                    if (pattern.getId() == selectedPatternId)
                        selected = *slot;
                }
            }
        }

        // Follow the selection into its bank, so the numbers shown are the
        // ones the highlighted slot belongs to.
        if (selected)
            visibleBank = selected->bank;

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1c20));

        if (! enabled)
            return;

        for (int bank = 0; bank < model::PatternSlot::numBanks; ++bank)
            drawCell (g, bankBounds (bank),
                      juce::String::charToString ((juce::juce_wchar) ('A' + bank)),
                      bank == visibleBank,
                      selected && selected->bank == bank,
                      bankHoldsNotes (bank));

        for (int index = 0; index < model::PatternSlot::slotsPerBank; ++index)
        {
            const model::PatternSlot slot { visibleBank, index };
            const auto state = slotStates[(size_t) slot.toFlatIndex()];

            drawCell (g, numberBounds (index), juce::String (index + 1),
                      selected && *selected == slot,
                      false,
                      state == SlotState::hasNotes,
                      state == SlotState::empty);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! enabled)
            return;

        for (int bank = 0; bank < model::PatternSlot::numBanks; ++bank)
        {
            if (bankBounds (bank).toFloat().contains (e.position))
            {
                visibleBank = bank;
                repaint();
                return;
            }
        }

        for (int index = 0; index < model::PatternSlot::slotsPerBank; ++index)
        {
            const auto bounds = numberBounds (index);

            if (! bounds.toFloat().contains (e.position))
                continue;

            const model::PatternSlot slot { visibleBank, index };

            if (e.mods.isPopupMenu())
            {
                if (onSlotMenu)
                    onSlotMenu (slot, localAreaToGlobal (bounds));
            }
            else if (onSlotClicked)
            {
                onSlotClicked (slot);
            }

            return;
        }
    }

private:
    enum class SlotState { unused, empty, hasNotes };

    static constexpr int cellWidth = 24;
    static constexpr int bankGap = 10;

    juce::Rectangle<int> bankBounds (int bank) const
    {
        return { 6 + bank * cellWidth, 2, cellWidth - 2, getHeight() - 4 };
    }

    juce::Rectangle<int> numberBounds (int index) const
    {
        const auto left = 6 + model::PatternSlot::numBanks * cellWidth + bankGap;
        return { left + index * cellWidth, 2, cellWidth - 2, getHeight() - 4 };
    }

    bool bankHoldsNotes (int bank) const
    {
        for (int index = 0; index < model::PatternSlot::slotsPerBank; ++index)
            if (slotStates[(size_t) model::PatternSlot { bank, index }.toFlatIndex()] == SlotState::hasNotes)
                return true;

        return false;
    }

    void drawCell (juce::Graphics& g, juce::Rectangle<int> bounds, const juce::String& text,
                   bool isCurrent, bool marksSelection, bool filled, bool outlined = false)
    {
        auto area = bounds.toFloat();

        if (isCurrent)
            g.setColour (juce::Colour (0xff35608a));
        else if (filled)
            g.setColour (juce::Colour (0xff474730));
        else
            g.setColour (juce::Colour (0xff2b2b30));

        g.fillRoundedRectangle (area, 3.0f);

        if (outlined || marksSelection)
        {
            g.setColour (juce::Colour (0xff707078));
            g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);
        }

        g.setColour (isCurrent ? juce::Colours::white : juce::Colour (0xffb8b8c0));
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (text, bounds, juce::Justification::centred);
    }

    std::array<SlotState, (size_t) model::PatternSlot::numSlots> slotStates {};
    std::optional<model::PatternSlot> selected;
    int visibleBank = 0;
    bool enabled = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SlotSwitcher)
};

//==============================================================================
// The Inst tab's content: whichever editor the generator's instrument has.
// A 4OSC gets ours; an external plugin gets its own editor hosted inline; the
// sampler types and audio rows get a note about where they are edited, until
// they grow inline editors of their own.
class InstrumentView : public juce::Component
{
public:
    InstrumentView()
    {
        info.setJustificationType (juce::Justification::centred);
        info.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
        addAndMakeVisible (info);
    }

    ~InstrumentView() override
    {
        clearEditors();
    }

    void setInstrument (te::Plugin* instrument)
    {
        // The editor components hold pointers into the plugin, so tearing them
        // down before pointing anywhere else is not optional.
        if (instrument == shownInstrument)
            return;

        clearEditors();
        shownInstrument = instrument;

        if (auto synth = dynamic_cast<te::FourOscPlugin*> (instrument))
        {
            fourOsc = std::make_unique<FourOscEditor> (*synth);
            viewport.setViewedComponent (fourOsc.get(), false);
            viewport.setScrollBarsShown (true, true);
            addAndMakeVisible (viewport);
            info.setVisible (false);
        }
        else if (auto external = dynamic_cast<te::ExternalPlugin*> (instrument))
        {
            if (auto instance = external->getAudioPluginInstance())
            {
                presetBar = std::make_unique<PresetBar> (*external);
                externalEditor.reset (instance->createEditorIfNeeded());
                addAndMakeVisible (presetBar.get());

                if (externalEditor != nullptr)
                    addAndMakeVisible (externalEditor.get());

                info.setVisible (externalEditor == nullptr);
                info.setText ("This plugin has no editor", juce::dontSendNotification);
            }
        }
        else
        {
            info.setVisible (true);
            info.setText (instrument == nullptr
                              ? "No instrument on this generator"
                              : "Edited from the generator panel",
                          juce::dontSendNotification);
        }

        resized();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        info.setBounds (area);

        if (fourOsc != nullptr)
        {
            viewport.setBounds (area);
            fourOsc->setSize (FourOscEditor::width, FourOscEditor::height);
        }

        if (externalEditor != nullptr || presetBar != nullptr)
        {
            if (presetBar != nullptr)
                presetBar->setBounds (area.removeFromTop (PresetBar::height));

            if (externalEditor != nullptr)
            {
                // The plugin picks its own size; centre it rather than
                // stretching a fixed-layout editor.
                const auto w = juce::jmin (area.getWidth(), externalEditor->getWidth());
                const auto h = juce::jmin (area.getHeight(), externalEditor->getHeight());
                externalEditor->setTopLeftPosition (area.getX() + (area.getWidth() - w) / 2,
                                                    area.getY() + (area.getHeight() - h) / 2);
            }
        }
    }

private:
    void clearEditors()
    {
        externalEditor.reset();
        presetBar.reset();
        viewport.setViewedComponent (nullptr, false);
        removeChildComponent (&viewport);
        fourOsc.reset();
        shownInstrument = nullptr;
    }

    te::Plugin* shownInstrument = nullptr;
    juce::Label info;
    juce::Viewport viewport;
    std::unique_ptr<FourOscEditor> fourOsc;
    std::unique_ptr<PresetBar> presetBar;
    std::unique_ptr<juce::AudioProcessorEditor> externalEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentView)
};

//==============================================================================
// One window per selected generator, Orion-style: a header with an
// Inst / Pianoroll tab pair and the pattern-slot switcher, over whichever view
// the tab picks. Replaces the separate pattern-editor and instrument windows.
class GeneratorWindow : public juce::DocumentWindow
{
public:
    enum class Tab { instrument, pianoRoll };

    GeneratorWindow (juce::UndoManager& um,
                     std::function<void()> onCloseCallback,
                     std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Generator",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          onKey (std::move (keyHandler)),
          rollContent (um)
    {
        rollContent.onPatternRenamed = [this] (const juce::String& name) { updateTitle (name); };

        content.header.instTab.onClick = [this] { showTab (Tab::instrument); };
        content.header.rollTab.onClick = [this] { showTab (Tab::pianoRoll); };

        content.body.addAndMakeVisible (instrumentView);
        content.body.addChildComponent (rollContent);
        content.onLayout = [this] { layoutBody(); };

        // The roll's size, so the bigger of the two views fits without a
        // resize on every tab switch.
        content.setSize (1000, headerHeight + 554 + PianoRollRuler::preferredHeight
                                             + PianoRollVelocityLane::preferredHeight
                                             + ShortcutHelpBar::preferredHeight);

        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);

        rollContent.scrollToMiddleOfPitchRange();
        showTab (Tab::pianoRoll);
    }

    SlotSwitcher& getSlotSwitcher()  { return content.header.slots; }

    void showTab (Tab tab)
    {
        activeTab = tab;
        instrumentView.setVisible (tab == Tab::instrument);
        rollContent.setVisible (tab == Tab::pianoRoll);
        content.header.instTab.setToggleState (tab == Tab::instrument, juce::dontSendNotification);
        content.header.rollTab.setToggleState (tab == Tab::pianoRoll, juce::dontSendNotification);

        if (tab == Tab::pianoRoll)
            rollContent.focusRoll();
    }

    void setPreviewNoteCallback (std::function<void (int, int)> callback)
    {
        rollContent.setPreviewNoteCallback (std::move (callback));
    }

    void setSong (model::Song song)
    {
        rollContent.setSong (std::move (song));
    }

    // Just the switcher's states: cheap, safe to call on every model refresh,
    // so notes landing in a slot recolour it while the window is open.
    void updateSlots (const std::optional<model::Generator>& generator,
                      const juce::String& selectedPatternId)
    {
        content.header.slots.setFromGenerator (generator, selectedPatternId);
    }

    // The whole retarget in one call: the switcher's states, the Inst view's
    // editor, and the roll's pattern.
    void setGenerator (const std::optional<model::Generator>& generator,
                       te::Plugin* instrument,
                       std::optional<model::Pattern> pattern,
                       const juce::String& title)
    {
        updateSlots (generator, pattern ? pattern->getId() : juce::String());
        instrumentView.setInstrument (instrument);

        titlePrefix = {};
        juce::String name;

        if (pattern)
        {
            name = pattern->getName();
            if (name.isNotEmpty() && title.endsWith (name))
                titlePrefix = title.dropLastCharacters (name.length());
        }

        rollContent.setPattern (std::move (pattern));
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
    static constexpr int tabRowHeight = 26;
    static constexpr int headerHeight = tabRowHeight + SlotSwitcher::preferredHeight;

    void updateTitle (const juce::String& patternName)
    {
        setName (patternName.isNotEmpty() ? titlePrefix + patternName : "Generator");
    }

    void layoutBody()
    {
        const auto area = content.body.getLocalBounds();
        instrumentView.setBounds (area);
        rollContent.setBounds (area);
    }

    struct Header : public juce::Component
    {
        Header()
        {
            for (auto* tab : { &instTab, &rollTab })
            {
                tab->setClickingTogglesState (false);
                tab->setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff35608a));
                addAndMakeVisible (tab);
            }

            addAndMakeVisible (slots);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            auto tabRow = area.removeFromTop (tabRowHeight).reduced (4, 2);
            instTab.setBounds (tabRow.removeFromLeft (72));
            tabRow.removeFromLeft (4);
            rollTab.setBounds (tabRow.removeFromLeft (72));
            slots.setBounds (area);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff26262c));
        }

        juce::TextButton instTab { "Inst" }, rollTab { "Pianoroll" };
        SlotSwitcher slots;
    };

    struct Content : public juce::Component
    {
        Content()
        {
            addAndMakeVisible (header);
            addAndMakeVisible (body);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            header.setBounds (area.removeFromTop (headerHeight));
            body.setBounds (area);

            if (onLayout)
                onLayout();
        }

        Header header;
        juce::Component body;
        std::function<void()> onLayout;
    };

    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    juce::String titlePrefix;
    Tab activeTab = Tab::pianoRoll;

    Content content;
    InstrumentView instrumentView;
    PianoRollContent rollContent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratorWindow)
};

} // namespace carve::app
