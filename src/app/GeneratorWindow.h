#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "DrumPadGrid.h"
#include "FourOscEditor.h"
#include "IconButton.h"
#include "PianoRollWindow.h"
#include "PresetManager.h"
#include "model/SongModel.h"

namespace carve::app
{

// The pattern picker: which of the generator's patterns is being edited, and
// the gestures that make more of them.
//
// Patterns are named things now rather than cells of a fixed A1..D9 grid, so
// the list is as long as the user has made it and every entry says what it is.
// What the grid was actually good for -- reaching another pattern, or a copy
// of this one, without stopping to think -- is kept by the two buttons: New
// and Clone both name their result themselves and select it on the spot, so
// neither ever opens a dialog.
class PatternPicker : public juce::Component
{
public:
    PatternPicker()
    {
        patterns.setTextWhenNothingSelected ("(no pattern)");
        patterns.onChange = [this]
        {
            const int index = patterns.getSelectedItemIndex();

            if (index >= 0 && index < patternIds.size() && onPatternPicked)
                onPatternPicked (patternIds[index]);
        };

        newButton.onClick   = [this] { if (onNewPattern) onNewPattern(); };
        cloneButton.onClick = [this] { if (onClonePattern) onClonePattern(); };

        newButton.setTooltip ("New pattern");
        cloneButton.setTooltip ("Clone this pattern (cmd-D)");
        menuButton.setTooltip ("Rename, delete, copy, MIDI in/out");

        menuButton.onClick = [this]
        {
            if (onPatternMenu)
                onPatternMenu (localAreaToGlobal (menuButton.getBounds()));
        };

        for (auto* child : std::initializer_list<juce::Component*> { &patterns, &newButton, &cloneButton, &menuButton })
            addAndMakeVisible (child);
    }

    std::function<void (const juce::String& patternId)> onPatternPicked;
    std::function<void()> onNewPattern;
    std::function<void()> onClonePattern;
    std::function<void (juce::Rectangle<int> screenArea)> onPatternMenu;   // screen coords

    static constexpr int preferredHeight = 26;

    void setFromGenerator (const std::optional<model::Generator>& generator,
                           const juce::String& selectedPatternId)
    {
        patternIds.clear();
        patterns.clear (juce::dontSendNotification);

        // An audio generator's clips are the files placed on it: it has no
        // patterns, so there is nothing here to pick or to make.
        const bool hasPatterns = generator.has_value() && ! generator->isAudio();

        if (hasPatterns)
        {
            const auto list = generator->getPatterns();

            for (int i = 0; i < (int) list.size(); ++i)
            {
                const auto name = list[(size_t) i].getName();

                patternIds.add (list[(size_t) i].getId());
                patterns.addItem (name.isNotEmpty() ? name : "(unnamed)", i + 1);
            }

            if (const int index = patternIds.indexOf (selectedPatternId); index >= 0)
                patterns.setSelectedItemIndex (index, juce::dontSendNotification);
        }

        patterns.setEnabled (hasPatterns);
        newButton.setEnabled (hasPatterns);

        // Nothing selected means nothing to copy or to act on, which is the
        // state a song with no generators at all opens in.
        const bool hasSelection = hasPatterns && patterns.getSelectedItemIndex() >= 0;
        cloneButton.setEnabled (hasSelection);
        menuButton.setEnabled (hasSelection);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1c20));
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 3);

        // The buttons keep their width and the list takes the rest, so a long
        // pattern name gets the room and Clone never moves out from under the
        // pointer.
        menuButton.setBounds (area.removeFromRight (28));
        area.removeFromRight (4);
        cloneButton.setBounds (area.removeFromRight (74));
        area.removeFromRight (4);
        newButton.setBounds (area.removeFromRight (64));
        area.removeFromRight (8);
        patterns.setBounds (area.removeFromLeft (juce::jmin (area.getWidth(), 220)));
    }

private:
    juce::ComboBox patterns;
    IconButton newButton { "New", Icon::plus }, cloneButton { "Clone", Icon::clone },
               menuButton { "", Icon::menu };
    juce::StringArray patternIds;   // parallel to the combo's items

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatternPicker)
};

//==============================================================================
// The Inst tab's content: whichever editing surface the generator offers.
// A 4OSC gets our editor, an external plugin its own hosted inline, a drum
// kit its 16 pads, a plain sampler its Load Sample chooser; an audio row just
// says what it is. The model edits behind the pads and the sample button live
// in GeneratorController -- this view only reports gestures and shows state.
class InstrumentView : public juce::Component
{
public:
    // Pad gestures, forwarded from the grid with the pad's screen area so the
    // controller can hang its replace/clear menu off the pad itself.
    std::function<void (int pad, juce::Rectangle<int> screenArea)> onPadClicked;
    std::function<void (int pad)> onPadTriggered;
    std::function<void (int startPad, const juce::StringArray&)> onPadFilesDropped;
    std::function<bool (const juce::StringArray&)> isInterestedInPadFiles;
    std::function<void()> onLoadSample;

    InstrumentView()
    {
        info.setJustificationType (juce::Justification::centred);
        info.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
        addAndMakeVisible (info);

        pads.onPadClicked = [this] (int pad)
        {
            if (onPadClicked)
                onPadClicked (pad, pads.localAreaToGlobal (pads.getPadBounds (pad)));
        };
        pads.onPadTriggered = [this] (int pad)
        {
            if (onPadTriggered)
                onPadTriggered (pad);
        };
        pads.onFilesDropped = [this] (int startPad, const juce::StringArray& files)
        {
            if (onPadFilesDropped)
                onPadFilesDropped (startPad, files);
        };
        pads.isInterestedInFiles = [this] (const juce::StringArray& files)
        {
            return isInterestedInPadFiles && isInterestedInPadFiles (files);
        };
        addChildComponent (pads);

        loadSampleButton.onClick = [this]
        {
            if (onLoadSample)
                onLoadSample();
        };
        addChildComponent (loadSampleButton);

        sampleName.setJustificationType (juce::Justification::centred);
        sampleName.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
        addChildComponent (sampleName);
    }

    ~InstrumentView() override
    {
        clearEditors();
    }

    // The static half of a retarget: which surface to show. The dynamic half
    // (pad states, the sampler's sound name) is refresh(), called on every
    // model change as well.
    void setGenerator (const std::optional<model::Generator>& generator, te::Plugin* instrument)
    {
        const auto kind = kindFor (generator);

        // The editor components hold pointers into the plugin, so tearing them
        // down before pointing anywhere else is not optional.
        if (kind == shownKind && instrument == shownInstrument)
            return;

        clearEditors();
        shownKind = kind;
        shownInstrument = instrument;

        pads.setVisible (kind == Kind::drumKit);
        loadSampleButton.setVisible (kind == Kind::sampler);
        sampleName.setVisible (kind == Kind::sampler);
        info.setVisible (false);

        if (kind == Kind::instrument)
        {
            if (auto synth = dynamic_cast<te::FourOscPlugin*> (instrument))
            {
                fourOsc = std::make_unique<FourOscEditor> (*synth);
                viewport.setViewedComponent (fourOsc.get(), false);
                viewport.setScrollBarsShown (true, true);
                addAndMakeVisible (viewport);
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
                }

                if (externalEditor == nullptr)
                    showInfo ("This plugin has no editor");
            }
            else
            {
                showInfo ("No instrument on this generator");
            }
        }
        else if (kind == Kind::audio)
        {
            showInfo ("Audio track: its clips are the files placed on the playlist");
        }
        else if (kind == Kind::none)
        {
            showInfo ("No generator selected");
        }

        resized();
    }

    // Pad states and the sampler's sound name, pushed on every model refresh
    // so a drop or an undo shows up while the window is open.
    void refresh (const std::optional<model::Generator>& generator,
                  const juce::String& selectedPatternId)
    {
        if (shownKind == Kind::drumKit && generator)
        {
            // Which pads the pattern being edited plays, so the kit and the
            // piano roll line up without switching tabs.
            std::array<bool, (size_t) drumkit::numPads> used {};

            if (auto pattern = generator->findPattern (selectedPatternId))
                for (const auto& note : pattern->getNotes())
                    if (auto pad = drumkit::getPadForNote (note.getPitch()))
                        used[(size_t) *pad] = true;

            for (int pad = 0; pad < drumkit::numPads; ++pad)
            {
                const auto sound = drumkit::findSoundForPad (*generator, pad);
                pads.setPadState (pad, sound ? sound->getName() : juce::String(),
                                  used[(size_t) pad]);
            }

            pads.repaint();
        }

        if (shownKind == Kind::sampler && generator)
        {
            const auto sounds = generator->getSounds();
            sampleName.setText (sounds.empty() ? "No sample loaded"
                                               : sounds.front().getName(),
                                juce::dontSendNotification);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        info.setBounds (area);

        if (pads.isVisible())
            pads.setBounds (area.reduced (16).withHeight (
                juce::jmin (area.getHeight() - 32, DrumPadGrid::getPreferredHeight())));

        if (loadSampleButton.isVisible())
        {
            auto centre = area.withSizeKeepingCentre (juce::jmin (area.getWidth() - 32, 320), 64);
            loadSampleButton.setBounds (centre.removeFromTop (28));
            centre.removeFromTop (8);
            sampleName.setBounds (centre);
        }

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
    // A drum kit's instrument is a sampler plugin too, so the view is picked
    // from the generator's type, never from the plugin's.
    enum class Kind { none, instrument, sampler, drumKit, audio };

    static Kind kindFor (const std::optional<model::Generator>& generator)
    {
        if (! generator)
            return Kind::none;
        if (generator->isDrumKit())
            return Kind::drumKit;
        if (generator->isSampler())
            return Kind::sampler;
        if (generator->isAudio())
            return Kind::audio;
        return Kind::instrument;
    }

    void showInfo (const juce::String& text)
    {
        info.setVisible (true);
        info.setText (text, juce::dontSendNotification);
    }

    void clearEditors()
    {
        externalEditor.reset();
        presetBar.reset();
        viewport.setViewedComponent (nullptr, false);
        removeChildComponent (&viewport);
        fourOsc.reset();
        shownInstrument = nullptr;
        shownKind = Kind::none;
    }

    Kind shownKind = Kind::none;
    te::Plugin* shownInstrument = nullptr;
    juce::Label info;
    DrumPadGrid pads;
    juce::TextButton loadSampleButton { "Load Sample..." };
    juce::Label sampleName;
    juce::Viewport viewport;
    std::unique_ptr<FourOscEditor> fourOsc;
    std::unique_ptr<PresetBar> presetBar;
    std::unique_ptr<juce::AudioProcessorEditor> externalEditor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InstrumentView)
};

//==============================================================================
// One window per selected generator, Orion-style: a header with an
// Inst / Pianoroll tab pair and the pattern picker, over whichever view the
// tab picks. Replaces the separate pattern-editor and instrument windows.
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

        rollContent.onSeekPatternBeat = [this] (double beat)
        {
            if (onSeekPatternBeat)
                onSeekPatternBeat (beat);
        };

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

    PatternPicker& getPatternPicker()    { return content.header.patterns; }
    InstrumentView& getInstrumentView()  { return instrumentView; }

    // The roll's ruler asked for the transport to move to this pattern beat;
    // mapping that into a song position is the owner's job.
    std::function<void (double patternBeat)> onSeekPatternBeat;

    // Where in the edited pattern the transport currently is, or nothing
    // while it plays somewhere this pattern is not placed.
    void setPlayheadPatternBeat (std::optional<double> beat)
    {
        rollContent.setPlayheadBeat (beat);
    }

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

    // The dynamic state: the picker's list and selection, and the Inst tab's
    // pad states and sample name. Cheap and editor-free, so it is safe to call
    // on every model refresh -- a pattern made, cloned, renamed or deleted
    // while the window is open shows up here.
    void updatePatterns (const std::optional<model::Generator>& generator,
                         const juce::String& selectedPatternId)
    {
        content.header.patterns.setFromGenerator (generator, selectedPatternId);
        instrumentView.refresh (generator, selectedPatternId);
    }

    // The whole retarget in one call: the picker's list, the Inst view's
    // editor, and the roll's pattern.
    void setGenerator (const std::optional<model::Generator>& generator,
                       te::Plugin* instrument,
                       std::optional<model::Pattern> pattern,
                       const juce::String& title)
    {
        instrumentView.setGenerator (generator, instrument);
        updatePatterns (generator, pattern ? pattern->getId() : juce::String());

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
    static constexpr int headerHeight = tabRowHeight + PatternPicker::preferredHeight;

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

            addAndMakeVisible (patterns);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            auto tabRow = area.removeFromTop (tabRowHeight).reduced (4, 2);
            instTab.setBounds (tabRow.removeFromLeft (84));
            tabRow.removeFromLeft (4);
            rollTab.setBounds (tabRow.removeFromLeft (108));
            patterns.setBounds (area);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff26262c));
        }

        IconButton instTab { "Inst", Icon::instrument }, rollTab { "Pianoroll", Icon::pianoRoll };
        PatternPicker patterns;
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
