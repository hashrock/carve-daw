#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "KnobPanel.h"
#include "RecentMediaList.h"
#include "model/Singing.h"
#include "plugins/UtauPlugin.h"

namespace carve::app
{

// The singer's Inst tab: which voicebank, and how the synthesis is getting on.
// There is nothing to play with here -- what a singer sings is its lyrics,
// which are typed into the piano roll -- so this says where to type them.
//
// Like the SoundFont editor, it reports gestures and reads the plugin's state
// on a timer; the voicebank itself is the model's (GeneratorController).
class UtauEditor : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   private juce::Timer
{
public:
    std::function<void()> onLoadVoicebank;
    std::function<void (const juce::File& folder)> onVoicebankDropped;

    // A voice knob let go of: which one (ids::voiceGender etc.) and where.
    // Only on release -- every value is a re-synthesis of the whole part.
    std::function<void (const juce::Identifier&, double)> onVoiceStyleChanged;
    std::function<void (const juce::File& folder)> onRecentChosen;

    // The voicebanks sung with lately, offered while this singer has none.
    void setRecentItems (std::vector<RecentMediaList::Item> items)  { recent.setItems (std::move (items)); }

    explicit UtauEditor (plugins::UtauPlugin& singerToShow) : plugin (&singerToShow)
    {
        // Gender, Breath, Brightness, Modulation: see model::Generator::VoiceStyle.
        const model::Generator::VoiceStyle defaults;
        addKnob (model::ids::voiceGender, "Gender", -100.0, 100.0, defaults.gender,
                 "Formants: + a deeper, older voice, - a lighter, younger one. The pitch stays where it is.");
        addKnob (model::ids::voiceBreath, "Breath", 0.0, 100.0, defaults.breath,
                 "How much breath is in the voice. 50 is as recorded.");
        addKnob (model::ids::voiceBrightness, "Brightness", -100.0, 100.0, defaults.brightness,
                 "Tilts the tone: + brighter and more forward, - darker and softer.");
        addKnob (model::ids::voiceModulation, "Modulation", 0.0, 100.0, defaults.modulation,
                 "How much of the recording's own pitch wobble is kept. 0 sings dead on the note.");

        loadButton.onClick = [this] { if (onLoadVoicebank) onLoadVoicebank(); };
        addAndMakeVisible (loadButton);

        nameLabel.setFont (juce::FontOptions (20.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe8e8ee));
        addAndMakeVisible (nameLabel);

        for (auto* label : { &folderLabel, &detailLabel, &statusLabel, &silentLabel, &hintLabel })
        {
            label->setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
            label->setMinimumHorizontalScale (0.7f);
            addAndMakeVisible (label);
        }

        silentLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0604f));

        hintLabel.setText ("Double-click a note in the Pianoroll to type its lyric (Tab moves to the next note).\n"
                           "A note without one sings the default lyric.",
                           juce::dontSendNotification);
        hintLabel.setJustificationType (juce::Justification::topLeft);
        hintLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6e6e78));

        recent.onChosen = [this] (const juce::File& folder) { if (onRecentChosen) onRecentChosen (folder); };
        addChildComponent (recent);

        // Last: setSize lays the children out, and the knobs only exist once
        // addKnob has made them. Sized first, they kept no bounds at all.
        setSize (520, 330);

        refreshFromPlugin();
        startTimerHz (4);
    }

    // The values the model holds, shown unless a knob is being dragged.
    void setVoiceStyle (const model::Generator::VoiceStyle& style)
    {
        const double values[] = { style.gender, style.breath, style.brightness, style.modulation };

        for (size_t i = 0; i < knobs.size(); ++i)
            knobs[i].knob->setValue (values[i]);
    }

    // The pattern on screen, whose notes without a recording are counted
    // here -- the roll marks them, this says how many there are.
    void setPattern (std::optional<model::Pattern> newPattern)
    {
        pattern = std::move (newPattern);
        refreshFromPlugin();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));

        if (dragOver)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 2.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);

        auto top = area.removeFromTop (28);
        loadButton.setBounds (top.removeFromRight (150));
        top.removeFromRight (10);
        nameLabel.setBounds (top);

        area.removeFromTop (4);
        folderLabel.setBounds (area.removeFromTop (18));
        detailLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (6);
        statusLabel.setBounds (area.removeFromTop (20));
        silentLabel.setBounds (area.removeFromTop (20));
        area.removeFromTop (8);

        // Without a voicebank there is nothing to colour or to count: the
        // history takes the knobs' place.
        recent.setBounds (area);

        auto knobRow = area.removeFromTop (110);
        const auto knobWidth = knobRow.getWidth() / juce::jmax (1, (int) knobs.size());

        for (auto& entry : knobs)
        {
            auto cell = knobRow.removeFromLeft (knobWidth);
            entry.knob->setBounds (cell.withSizeKeepingCentre (entry.knob->getPreferredWidth(),
                                                               entry.knob->getPreferredHeight()));
        }

        area.removeFromTop (8);
        hintLabel.setBounds (area);
    }

    // A folder: a voicebank is one. Whether it holds an oto.ini is the
    // plugin's to say, after the drop.
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && juce::File (files[0]).isDirectory();
    }

    void fileDragEnter (const juce::StringArray&, int, int) override  { setDragOver (true); }
    void fileDragExit (const juce::StringArray&) override             { setDragOver (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDragOver (false);

        if (onVoicebankDropped && ! files.isEmpty())
            onVoicebankDropped (juce::File (files[0]));
    }

private:
    void timerCallback() override  { refreshFromPlugin(); }

    void refreshFromPlugin()
    {
        auto* singer = dynamic_cast<plugins::UtauPlugin*> (plugin.get());

        if (singer == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        const auto bank = singer->getVoicebank();
        const auto loadError = singer->getLoadError();

        nameLabel.setText (bank != nullptr ? bank->getName() : juce::String ("No voicebank"), juce::dontSendNotification);
        folderLabel.setText (bank != nullptr ? bank->getFolder().getFullPathName() : juce::String(), juce::dontSendNotification);
        detailLabel.setText (bank != nullptr ? juce::String (bank->getNumAliases()) + " aliases" : juce::String(),
                             juce::dontSendNotification);

        juce::String status;
        auto colour = juce::Colour (0xff9a9aa4);

        if (loadError.isNotEmpty())
        {
            status = loadError;
            colour = juce::Colour (0xffe0a060);
        }
        else if (bank == nullptr)
        {
            status = "Choose a voicebank folder, or drop one here";
        }
        else if (const auto pending = singer->getNumPending(); pending > 0)
        {
            status = "Synthesising " + juce::String (pending) + (pending == 1 ? " note..." : " notes...");
        }
        else if (const auto failed = singer->getNumFailed(); failed > 0)
        {
            status = juce::String (failed) + (failed == 1 ? " note" : " notes") + " could not be sung: "
                     + singer->getLastRenderError();
            colour = juce::Colour (0xffe0a060);
        }
        else
        {
            status = "Ready";
            colour = juce::Colour (0xff4fc27a);
        }

        statusLabel.setText (status, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, colour);

        const bool empty = bank == nullptr;
        recent.setVisible (empty);
        hintLabel.setVisible (! empty);

        for (auto& entry : knobs)
            entry.knob->setVisible (! empty);

        const auto silent = countSilentNotes (bank.get());
        silentLabel.setText (silent == 0 ? juce::String()
                                         : juce::String (silent) + (silent == 1 ? " note" : " notes")
                                               + " in this pattern have no recording for their lyric and are silent"
                                               + " (struck through in the Pianoroll)",
                             juce::dontSendNotification);
    }

    int countSilentNotes (const voice::Voicebank* bank) const
    {
        if (bank == nullptr || ! pattern)
            return 0;

        const auto ordered = model::singing::inSingingOrder (pattern->getNotes());
        int silent = 0;

        for (size_t i = 0; i < ordered.size(); ++i)
        {
            const auto lyric = ordered[i].getLyric();

            if (bank->resolve (lyric.isNotEmpty() ? lyric : model::Note::getDefaultLyric(),
                               model::singing::previousLyricOf (ordered, i), ordered[i].getPitch()) == nullptr)
                ++silent;
        }

        return silent;
    }

    void setDragOver (bool over)
    {
        if (dragOver != over)
        {
            dragOver = over;
            repaint();
        }
    }

    // The app's own knob, the one the synth and drum editors are made of,
    // committing on release (see ValueKnob). The ones that go both ways --
    // Gender, Brightness -- light their ring from the top.
    struct VoiceKnob
    {
        juce::Identifier id;
        std::unique_ptr<ValueKnob> knob;
    };

    void addKnob (const juce::Identifier& id, const juce::String& name, double low, double high,
                  double defaultValue, const juce::String& tip)
    {
        VoiceKnob entry { id, std::make_unique<ValueKnob> (name, KnobSize::large, low, high, 1.0,
                                                           defaultValue, low < 0.0 && high > 0.0) };
        entry.knob->setDialTooltip (tip);
        entry.knob->onCommit = [this, id] (double value)
        {
            if (onVoiceStyleChanged)
                onVoiceStyleChanged (id, value);
        };

        addAndMakeVisible (*entry.knob);
        knobs.push_back (std::move (entry));
    }

    std::vector<VoiceKnob> knobs;
    RecentMediaList recent;

    // What each knob does, on hover. The app has no tooltip window of its
    // own, so this one is, for this editor's children only -- the way the
    // clip panel keeps its own (see ClipPropertiesPanel).
    class OwnTooltips : public juce::TooltipWindow
    {
    public:
        explicit OwnTooltips (juce::Component& ownerToServe)
            : juce::TooltipWindow (nullptr, 500), owner (ownerToServe) {}

        juce::String getTipFor (juce::Component& c) override
        {
            return owner.isParentOf (&c) ? juce::TooltipWindow::getTipFor (c) : juce::String();
        }

    private:
        juce::Component& owner;
    };

    OwnTooltips tooltips { *this };
    te::SafeSelectable<te::Plugin> plugin;   // gone with its track
    bool dragOver = false;

    juce::TextButton loadButton { "Load voicebank..." };
    juce::Label nameLabel, folderLabel, detailLabel, statusLabel, silentLabel, hintLabel;
    std::optional<model::Pattern> pattern;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UtauEditor)
};

} // namespace carve::app
