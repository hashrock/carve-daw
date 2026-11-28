#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "KnobPanel.h"
#include "RecentMediaList.h"
#include "model/Singing.h"
#include "plugins/UtauPlugin.h"

namespace carve::app
{

// The singer's Inst tab: who sings, and how.
//
//   the singer   a menu of the voicebanks at hand -- the sound library's and
//                the ones sung with lately -- with a folder of one's own and
//                the library itself at its foot
//   Voice        the singer's colour: Gender, Breath, Brightness, Modulation
//   Expression   how the pitch moves: Vibrato and its Rate, and Portamento,
//                the glide into a note that follows on
//
// Nothing else is shown unless it needs doing something about: notes being
// synthesised, notes that could not be, notes with nothing to sing their
// lyric with. Lyrics themselves are typed in the piano roll.
//
// Like the SoundFont editor, it reports gestures and reads the plugin's state
// on a timer; the voicebank itself is the model's (GeneratorController).
class UtauEditor : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   private juce::Timer
{
public:
    std::function<void()> onLoadVoicebank;
    std::function<void()> onOpenSoundLibrary;
    std::function<void (const juce::File& folder)> onVoicebankDropped;

    // A voice knob let go of: which one (ids::voiceGender etc.) and where.
    // Only on release -- every value is a re-synthesis of the whole part.
    std::function<void (const juce::Identifier&, double)> onVoiceStyleChanged;
    std::function<void (const juce::File& folder)> onRecentChosen;

    // The singers on offer while there is none: the library's and the ones
    // sung with lately.
    void setRecentItems (std::vector<RecentMediaList::Item> items)  { recent.setItems (std::move (items)); }

    // The same list, asked for as the singer menu opens: reading the names
    // means reading each voicebank's character.txt, which is not for every
    // model change.
    std::function<std::vector<RecentMediaList::Item>()> getSingers;

    explicit UtauEditor (plugins::UtauPlugin& singerToShow) : plugin (&singerToShow)
    {
        const model::Generator::VoiceStyle defaults;

        addKnob (voice, model::ids::voiceGender, "Gender", -100.0, 100.0, 1.0, defaults.gender, {},
                 "Formants: + a deeper, older voice, - a lighter, younger one. The pitch stays where it is.");
        addKnob (voice, model::ids::voiceBreath, "Breath", 0.0, 100.0, 1.0, defaults.breath, {},
                 "How much breath is in the voice. 50 is as recorded.");
        addKnob (voice, model::ids::voiceBrightness, "Brightness", -100.0, 100.0, 1.0, defaults.brightness, {},
                 "Tilts the tone: + brighter and more forward, - darker and softer.");
        addKnob (voice, model::ids::voiceModulation, "Modulation", 0.0, 100.0, 1.0, defaults.modulation, {},
                 "How much of the recording's own pitch wobble is kept. 0 sings dead on the note.");

        addKnob (expression, model::ids::voiceVibrato, "Vibrato", 0.0, 100.0, 1.0, defaults.vibrato, {},
                 "A vibrato that grows in once a note is held. 100 is half a semitone either way.");
        addKnob (expression, model::ids::voiceVibratoRate, "Rate", 3.0, 8.0, 0.1, defaults.vibratoRate, " Hz",
                 "How fast the vibrato goes.");
        addKnob (expression, model::ids::voicePortamento, "Portamento", 0.0, 200.0, 1.0, defaults.portamento, " ms",
                 "How long the glide into a note that follows straight on takes, either side of its start.");

        for (auto* section : { &voice, &expression })
        {
            section->setDrawsBackground (true);
            addAndMakeVisible (*section);
        }

        singerButton.onClick = [this] { showSingerMenu(); };
        addAndMakeVisible (singerButton);

        statusLabel.setMinimumHorizontalScale (0.7f);
        statusLabel.setFont (uiFont (fonts::small));
        addAndMakeVisible (statusLabel);

        recent.onChosen = [this] (const juce::File& folder) { if (onRecentChosen) onRecentChosen (folder); };
        addChildComponent (recent);

        // Last: setSize lays the children out, and the knobs only exist once
        // addKnob has made them. Sized first, they kept no bounds at all.
        setSize (margin * 2 + voice.getPreferredWidth() + sectionGap + expression.getPreferredWidth(),
                 margin * 2 + singerHeight + 4 + statusHeight + 6
                     + juce::jmax (voice.getPreferredHeight(), expression.getPreferredHeight()));

        refreshFromPlugin();
        startTimerHz (4);
    }

    // The values the model holds, shown unless a knob is being dragged.
    void setVoiceStyle (const model::Generator::VoiceStyle& style)
    {
        const std::pair<juce::Identifier, double> values[] = {
            { model::ids::voiceGender, style.gender }, { model::ids::voiceBreath, style.breath },
            { model::ids::voiceBrightness, style.brightness }, { model::ids::voiceModulation, style.modulation },
            { model::ids::voiceVibrato, style.vibrato }, { model::ids::voiceVibratoRate, style.vibratoRate },
            { model::ids::voicePortamento, style.portamento } };

        for (const auto& [id, value] : values)
            for (auto& entry : knobs)
                if (entry.id == id)
                    entry.knob->setValue (value);
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
        auto area = getLocalBounds().reduced (margin);

        singerButton.setBounds (area.removeFromTop (singerHeight));
        area.removeFromTop (4);
        statusLabel.setBounds (area.removeFromTop (statusHeight));
        area.removeFromTop (6);

        // Without a voicebank there is nothing to colour: the singers on
        // offer take the knobs' place.
        recent.setBounds (area);

        voice.setBounds (area.removeFromLeft (voice.getPreferredWidth()).withHeight (voice.getPreferredHeight()));
        area.removeFromLeft (sectionGap);
        expression.setBounds (area.removeFromLeft (expression.getPreferredWidth())
                                  .withHeight (expression.getPreferredHeight()));
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
    static constexpr int margin = 12, singerHeight = 44, statusHeight = 18, sectionGap = 10;

    void timerCallback() override  { refreshFromPlugin(); }

    // The singer's name, large, and a caret: the way to another singer.
    struct SingerButton : juce::Button
    {
        SingerButton() : juce::Button ("Singer")
        {
            setTooltip ("Choose who sings: a voicebank from the sound library, one sung with lately, or a folder");
            setWantsKeyboardFocus (false);
        }

        juce::String name;

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            const auto area = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (juce::Colour (down ? 0xff34343c : highlighted ? 0xff303037 : 0xff2a2a31));
            g.fillRoundedRectangle (area, 6.0f);
            g.setColour (juce::Colour (highlighted ? 0xff5a5a66 : 0xff3a3a44));
            g.drawRoundedRectangle (area, 6.0f, 1.0f);

            auto inner = getLocalBounds().reduced (14, 0);
            auto caret = inner.removeFromRight (14).toFloat();
            juce::Path chevron;
            const auto c = caret.getCentre();
            chevron.startNewSubPath (c.x - 5.0f, c.y - 2.5f);
            chevron.lineTo (c.x, c.y + 2.5f);
            chevron.lineTo (c.x + 5.0f, c.y - 2.5f);
            g.setColour (juce::Colour (0xff9a9aa4));
            g.strokePath (chevron, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            const bool none = name.isEmpty();
            g.setColour (none ? juce::Colour (0xff9a9aa4) : juce::Colour (0xfff0f0f4));
            g.setFont (uiFont (none ? fonts::title : 20.0f, none ? juce::Font::plain : juce::Font::bold));
            g.drawText (none ? juce::String ("Choose a singer") : name, inner.withTrimmedRight (8),
                        juce::Justification::centredLeft, true);
        }
    };

    void showSingerMenu()
    {
        juce::PopupMenu menu;
        const auto current = currentFolder();
        const auto offered = getSingers ? getSingers() : std::vector<RecentMediaList::Item>();

        // The library's singers first, under their own names, then the ones
        // sung with lately -- leaving out a copy of a library singer kept
        // elsewhere, and any name already listed: two lines saying the same
        // thing are no choice at all.
        std::vector<RecentMediaList::Item> singers;
        juce::StringArray namesShown;

        for (const auto& item : offered)
            if (item.fromLibrary)
            {
                singers.push_back (item);
                namesShown.add (voice::Voicebank::readName (item.file));
            }

        const auto libraryCount = singers.size();

        for (const auto& item : offered)
            if (! item.fromLibrary && ! namesShown.contains (item.title))
            {
                singers.push_back (item);
                namesShown.add (item.title);
            }

        int id = 1;

        for (size_t i = 0; i < singers.size(); ++i)
        {
            if (i == 0 && libraryCount > 0)
                menu.addSectionHeader ("Sound Library");

            if (i == libraryCount)
                menu.addSectionHeader ("Recent");

            menu.addItem (id++, singers[i].title, true, singers[i].file == current);
        }

        if (! singers.empty())
            menu.addSeparator();

        constexpr int chooseFolderId = 10000, libraryId = 10001;
        menu.addItem (chooseFolderId, "Choose Folder...");
        menu.addItem (libraryId, "Get More Singers...");

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&singerButton),
                            [safe = juce::Component::SafePointer (this), choices = singers] (int result)
        {
            if (safe == nullptr || result == 0)
                return;

            if (result == chooseFolderId)
            {
                if (safe->onLoadVoicebank)
                    safe->onLoadVoicebank();
            }
            else if (result == libraryId)
            {
                if (safe->onOpenSoundLibrary)
                    safe->onOpenSoundLibrary();
            }
            else if (juce::isPositiveAndBelow (result - 1, (int) choices.size()) && safe->onRecentChosen)
            {
                safe->onRecentChosen (choices[(size_t) (result - 1)].file);
            }
        });
    }

    juce::File currentFolder() const
    {
        if (auto* singer = dynamic_cast<plugins::UtauPlugin*> (plugin.get()))
            if (const auto bank = singer->getVoicebank())
                return bank->getFolder();

        return {};
    }

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

        if (const auto name = bank != nullptr ? bank->getName() : juce::String(); name != singerButton.name)
        {
            singerButton.name = name;
            singerButton.repaint();
        }

        // Only what needs something done about it, or is still under way.
        juce::String status;
        auto colour = juce::Colour (0xff9a9aa4);

        if (loadError.isNotEmpty())
        {
            status = loadError;
            colour = juce::Colour (0xffe0a060);
        }
        else if (bank == nullptr)
        {
            status = "Pick a singer below, or drop a voicebank folder here";
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
        else if (const auto silent = countSilentNotes (bank.get()); silent > 0)
        {
            status = juce::String (silent) + (silent == 1 ? " note has" : " notes have")
                     + " no recording for its lyric and stays silent (struck through in the Pianoroll)";
            colour = juce::Colour (0xffe0604f);
        }

        statusLabel.setText (status, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, colour);

        const bool empty = bank == nullptr;
        recent.setVisible (empty);
        voice.setVisible (! empty);
        expression.setVisible (! empty);
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
        ValueKnob* knob;
    };

    void addKnob (KnobSection& section, const juce::Identifier& id, const juce::String& name,
                  double low, double high, double step, double defaultValue,
                  const juce::String& suffix, const juce::String& tip)
    {
        auto knob = std::make_unique<ValueKnob> (name, KnobSize::large, low, high, step,
                                                 defaultValue, low < 0.0 && high > 0.0);
        knob->setDialTooltip (tip);
        knob->setValueSuffix (suffix);
        knob->onCommit = [this, id] (double value)
        {
            if (onVoiceStyleChanged)
                onVoiceStyleChanged (id, value);
        };

        knobs.push_back ({ id, knob.get() });
        section.addKnob (std::move (knob));
    }

    KnobSection voice { "Voice" }, expression { "Expression" };
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

    SingerButton singerButton;
    juce::Label statusLabel;
    std::optional<model::Pattern> pattern;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (UtauEditor)
};

} // namespace carve::app
