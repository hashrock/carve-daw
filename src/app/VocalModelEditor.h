#pragma once

#include <functional>
#include <memory>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "model/SongModel.h"
#include "plugins/VocalModelPlugin.h"

namespace carve::app
{

// The modelled singer's editor: its knobs, grouped by the part of the body
// they act on, as on the prototype's "voice design" panel -- the body's size,
// the folds and the breath, the throat, the nose, the mouth, how it sings,
// and the microphone and room after it. Above them, the characters (a set of
// knob positions each), the vowel shapes, and the lyric a live key sings.
//
// The words of the song are the notes' lyrics, typed in the piano roll.
class VocalModelEditor : public juce::Component,
                         private juce::Timer
{
public:
    using Plugin = plugins::VocalModelPlugin;

    explicit VocalModelEditor (Plugin& singer)
        : plugin (&singer), presetBar (singer)
    {
        auto isAlive = [this] { return plugin != nullptr; };
        using S = KnobSize;

        auto knob = [&isAlive] (te::AutomatableParameter::Ptr parameter, const juce::String& name, KnobSize size)
        {
            return std::make_unique<ParameterKnob> (*parameter, name, size, isAlive);
        };

        {
            auto& s = page.add (std::make_unique<KnobSection> ("Body"));
            s.addKnob (knob (singer.tractLength, "Tract", S::large));
            s.addKnob (knob (singer.fold, "Folds", S::medium));
            s.addKnob (knob (singer.resonance, "Resonance", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Folds & Breath"));
            s.addKnob (knob (singer.breath, "Breath", S::large));
            s.addKnob (knob (singer.press, "Press", S::medium));
            s.addKnob (knob (singer.registerMix, "Register", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Throat"));
            s.addKnob (knob (singer.throat, "Width", S::medium));
            s.addKnob (knob (singer.twang, "Twang", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Nose"));
            s.addKnob (knob (singer.nasal, "Nasal", S::medium));
        }

        page.newRow();

        {
            auto& s = page.add (std::make_unique<KnobSection> ("Mouth"));
            s.addKnob (knob (singer.clarity, "Clarity", S::medium));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Singing"));
            s.addKnob (knob (singer.strength, "Expression", S::large));
            s.addKnob (knob (singer.vibrato, "Vibrato", S::medium));
            s.addKnob (knob (singer.vibratoRate, "Vib Rate", S::small));
            s.addKnob (knob (singer.drift, "Drift", S::small));
        }
        {
            auto& s = page.add (std::make_unique<KnobSection> ("Mic & Room"));
            s.addKnob (knob (singer.brightness, "Bright", S::medium));
            s.addKnob (knob (singer.body, "Body", S::small));
            s.addKnob (knob (singer.ring, "Ring", S::small));
            s.addKnob (knob (singer.air, "Air", S::small));
            s.addKnob (knob (singer.reverb, "Room", S::medium));
            s.addKnob (knob (singer.volume, "Volume", S::large));
        }

        addAndMakeVisible (presetBar);
        addAndMakeVisible (page);

        auto setUpLabel = [this] (juce::Label& label, const juce::String& text)
        {
            label.setText (text, juce::dontSendNotification);
            label.setFont (uiFont (fonts::small));
            label.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
            addAndMakeVisible (label);
        };

        setUpLabel (characterLabel, "Character");
        setUpLabel (vowelLabel, "Vowels");
        setUpLabel (lyricLabel, "Keys sing");

        for (int i = 0; i < voice::vocal::numCharacters; ++i)
            characterBox.addItem (voice::vocal::getCharacter (i).name, i + 1);

        characterBox.setTextWhenNothingSelected ("(edited)");
        characterBox.setWantsKeyboardFocus (false);
        characterBox.onChange = [this]
        {
            if (refreshing)
                return;

            if (auto* target = getSinger())
                if (const int index = characterBox.getSelectedItemIndex(); index >= 0)
                    target->applyCharacter (index);
        };
        addAndMakeVisible (characterBox);

        for (int i = 0; i < voice::vocal::numVowelPresets; ++i)
            vowelBox.addItem (voice::vocal::getVowelPreset (i).name, i + 1);

        vowelBox.setWantsKeyboardFocus (false);
        vowelBox.onChange = [this]
        {
            if (refreshing)
                return;

            if (auto* target = getSinger())
            {
                auto& parameter = *target->vowelPreset;
                parameter.parameterChangeGestureBegin();
                parameter.setParameter ((float) juce::jmax (0, vowelBox.getSelectedItemIndex()), juce::sendNotification);
                parameter.parameterChangeGestureEnd();
            }
        };
        addAndMakeVisible (vowelBox);

        lyricEditor.setFont (uiFont (fonts::small));
        lyricEditor.setTextToShowWhenEmpty (model::Note::getDefaultLyric(), juce::Colour (0xff6e6e78));
        lyricEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1c20));
        lyricEditor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a3a42));
        lyricEditor.setIndents (4, 0);
        lyricEditor.setJustification (juce::Justification::centredLeft);
        lyricEditor.setTooltip ("What a key on a MIDI keyboard (or a preview in the piano roll) sings: "
                                "kana or romaji, a mora per key press, round and round");
        lyricEditor.setText (singer.liveLyric.get(), juce::dontSendNotification);
        lyricEditor.onReturnKey = [this] { commitLyric(); unfocusAllComponents(); };
        lyricEditor.onEscapeKey = [this]
        {
            if (auto* target = getSinger())
                lyricEditor.setText (target->liveLyric.get(), juce::dontSendNotification);

            unfocusAllComponents();
        };
        lyricEditor.onFocusLost = [this] { commitLyric(); };
        addAndMakeVisible (lyricEditor);

        hint.setText ("Lyrics: double-click a note in the Pianoroll and type kana or romaji (Tab moves on). "
                      "A note without one sings " + model::Note::getDefaultLyric() + ".",
                      juce::dontSendNotification);
        hint.setFont (uiFont (fonts::small));
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
        addAndMakeVisible (hint);

        status.setFont (uiFont (fonts::small));
        status.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
        addAndMakeVisible (status);

        setSize (juce::jmax (minWidth, page.getPreferredWidth()),
                 PresetBar::height + stripHeight + page.getPreferredHeight() + hintHeight);

        refreshControls();
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));

        auto strip = area.removeFromTop (stripHeight).reduced (margin, 4);
        auto place = [&strip] (juce::Component& c, int width)
        {
            c.setBounds (strip.removeFromLeft (width));
            strip.removeFromLeft (6);
        };

        place (characterLabel, 64);
        place (characterBox, 120);
        place (vowelLabel, 50);
        place (vowelBox, 100);
        place (lyricLabel, 66);
        place (lyricEditor, 140);
        status.setBounds (strip);

        page.setBounds (area.removeFromTop (page.getPreferredHeight()));
        hint.setBounds (area.reduced (margin, 0));
    }

private:
    static constexpr int minWidth = 640;
    static constexpr int stripHeight = 32;
    static constexpr int hintHeight = 24;
    static constexpr int margin = 8;

    Plugin* getSinger()  { return dynamic_cast<Plugin*> (plugin.get()); }

    void commitLyric()
    {
        if (auto* singer = getSinger())
            if (lyricEditor.getText().trim() != singer->liveLyric.get())
                singer->liveLyric = lyricEditor.getText().trim();
    }

    void refreshControls()
    {
        auto* singer = getSinger();

        if (singer == nullptr)
            return;

        const juce::ScopedValueSetter<bool> svs (refreshing, true);
        const int character = singer->findCurrentCharacter();

        if (character != characterBox.getSelectedItemIndex())
        {
            if (character >= 0)
                characterBox.setSelectedItemIndex (character, juce::dontSendNotification);
            else
                characterBox.setSelectedId (0, juce::dontSendNotification);
        }

        const int vowels = juce::roundToInt (singer->vowelPreset->getCurrentValue());

        if (vowels != vowelBox.getSelectedItemIndex())
            vowelBox.setSelectedItemIndex (vowels, juce::dontSendNotification);

        if (! lyricEditor.hasKeyboardFocus (true) && lyricEditor.getText() != singer->liveLyric.get())
            lyricEditor.setText (singer->liveLyric.get(), juce::dontSendNotification);

        const auto hz = singer->getSungHz();
        status.setText (hz > 0.0f ? "Singing " + juce::MidiMessage::getMidiNoteName (
                                        juce::roundToInt (69.0 + 12.0 * std::log2 ((double) hz / 440.0)), true, true, 4)
                                      + "  (" + juce::String (hz, 1) + " Hz)"
                                  : juce::String ("Silent"),
                        juce::dontSendNotification);
    }

    void timerCallback() override
    {
        if (getSinger() == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to be
            // closed, so stop touching anything until it is.
            setEnabled (false);
            stopTimer();
            return;
        }

        page.refresh();
        refreshControls();
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    KnobPage page;
    juce::Label characterLabel, vowelLabel, lyricLabel, hint, status;
    juce::ComboBox characterBox, vowelBox;
    juce::TextEditor lyricEditor;
    bool refreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalModelEditor)
};

} // namespace carve::app
