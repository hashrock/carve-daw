#pragma once

#include <array>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "Fonts.h"
#include "InstrumentGraphs.h"
#include "KnobPanel.h"
#include "InstrumentFxPanels.h"
#include "PresetManager.h"
#include "plugins/ElectricBassPlugin.h"

namespace carve::app
{

namespace bass = plugins::bass;

// The Electric Bass panel, laid out as a bassist thinks about the sound:
//
//   Right Hand   the string from the bridge to the 12th fret. The dot is where
//                it is plucked -- the harmonics with a node there drop out, and
//                the comb above the string shows which -- and the shaded
//                stretch at the bridge is the palm muting it.
//   Tone         the note's harmonics as they are 150ms in, against what was
//                recorded. Three handles, the way an EQ's are: Tone tilts the
//                whole spectrum about 200Hz, Body lifts the lowest harmonics,
//                Fret Buzz the exact-harmonic rattle in its own colour.
//   Sustain      the note's level over time; the tail's handle stretches it.
//   Left Hand    what a note played over a held one does: pluck again, hammer
//                on (pull off going down) or slide, drawn as the pitch it
//                takes, with the slide's time and the fret tap's level on it.
//
// Every drawing is computed by the plugin's own decoder for the last note
// played (E1 until one is), so what it shows is what that note will do.
class ElectricBassEditor : public juce::Component,
                           private juce::Timer
{
public:
    using Plugin = plugins::ElectricBassPlugin;
    using Technique = Plugin::Technique;

    std::function<void (int note, int velocity)> onPreviewNote;

    static constexpr int defaultWidth = 920, defaultHeight = 560 + 6 + BassAmpPanel<plugins::ElectricBassPlugin>::preferredHeight;

    explicit ElectricBassEditor (Plugin& bass)
        : plugin (&bass), presetBar (bass), string (*this), spectrum (*this), envelope (*this), legato (*this), amp (bass)
    {
        addAndMakeVisible (presetBar);
        addAndMakeVisible (amp);

        auto isAlive = [this] { return plugin != nullptr; };
        auto knob = [&] (te::AutomatableParameter::Ptr p, const juce::String& name)
        {
            auto k = std::make_unique<ParameterKnob> (*p, name, KnobSize::medium, isAlive);
            addAndMakeVisible (*k);
            return k;
        };

        attackKnob = knob (bass.attack, "Attack");
        evenKnob = knob (bass.attackEq, "Evenness");
        tuneKnob = knob (bass.tune, "Tune");
        outFretsKnob = std::make_unique<ParameterKnob> (*bass.outFrets, "Out Frets", KnobSize::small, isAlive);
        addAndMakeVisible (*outFretsKnob);
        levelKnob = knob (bass.level, "Level");

        for (auto* g : std::initializer_list<juce::Component*> { &string, &spectrum, &envelope, &legato })
            addAndMakeVisible (*g);

        for (int i = 0; i < Plugin::numTechniques; ++i)
        {
            auto& b = techniqueButtons[(size_t) i];
            b.setButtonText (Plugin::getTechniqueName ((Technique) i));
            b.setClickingTogglesState (true);
            b.setRadioGroupId (1);
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (graphs::colours::accent));
            b.setColour (juce::TextButton::textColourOnId, juce::Colour (graphs::colours::background));
            b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                 | (i < Plugin::numTechniques - 1 ? juce::Button::ConnectedOnRight : 0));
            b.onClick = [this, i]
            {
                if (techniqueButtons[(size_t) i].getToggleState())
                    if (auto* p = getPlugin())
                        p->setTechnique ((Technique) i);
            };
            addAndMakeVisible (b);
        }

        techniqueButtons[0].setTooltip ("Every note plucked, overlapping or not");
        techniqueButtons[1].setTooltip ("A note started over a held one hammers on (or pulls off, going down)");
        techniqueButtons[2].setTooltip ("A note started over a held one is slid to");

        monoButton.setTooltip ("A new note stops the one before, as on one string");
        monoButton.onClick = [this]
        {
            if (auto* p = getPlugin())
                p->setMono (monoButton.getToggleState());
        };
        addAndMakeVisible (monoButton);

        fretlessButton.setTooltip ("A slide glides instead of stepping through the frets it crosses");
        fretlessButton.onClick = [this]
        {
            if (auto* p = getPlugin())
                p->setFretless (fretlessButton.getToggleState());
        };
        addAndMakeVisible (fretlessButton);

        playButton.setTooltip ("Play the note the drawings show");
        playButton.onClick = [this]
        {
            if (onPreviewNote)
                onPreviewNote (shownNote, juce::roundToInt (shownVelocity * 127.0f));
        };
        addAndMakeVisible (playButton);


        noteLabel.setFont (uiFont (fonts::small));
        noteLabel.setColour (juce::Label::textColourId, juce::Colour (graphs::colours::dimText));
        noteLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (noteLabel);

        if (auto* kit = Plugin::getKit())
        {
            spec.reserve (*kit);
            current.reserve (*kit);
            recorded.reserve (*kit);
        }

        setSize (defaultWidth, defaultHeight);
        refresh();
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (graphs::colours::background));

        for (const auto& [title, area] : panels)
            graphs::drawPanel (g, area, title);
    }

    void resized() override
    {
        panels.clear();
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));
        area.reduce (margin, margin);

        // The amp across the bottom, last in the chain as on stage.
        amp.setBounds (area.removeFromBottom (BassAmpPanel<Plugin>::preferredHeight));
        area.removeFromBottom (margin);

        auto inner = [] (juce::Rectangle<int> r) { return r.reduced (8, 0).withTrimmedTop (graphs::panelTitleHeight).withTrimmedBottom (8); };

        // Right hand: the string, and the attack's two knobs beside it.
        {
            auto row = area.removeFromTop (164);
            auto knobs = row.removeFromRight (150);
            row.removeFromRight (margin);
            panels.push_back ({ "Right Hand", row });
            panels.push_back ({ "Attack", knobs });
            string.setBounds (inner (row));

            auto k = inner (knobs).withSizeKeepingCentre (140, 64);
            attackKnob->setBounds (k.removeFromLeft (70));
            evenKnob->setBounds (k);

            playButton.setBounds (row.getRight() - 60, row.getY() + 2, 52, 18);
            noteLabel.setBounds (row.getRight() - 300, row.getY() + 2, 234, 18);
        }

        area.removeFromTop (margin);

        // Tone and sustain, side by side.
        {
            auto row = area.removeFromTop (210);
            auto left = row.removeFromLeft ((row.getWidth() - margin) * 11 / 20);
            row.removeFromLeft (margin);
            panels.push_back ({ "Tone", left });
            panels.push_back ({ "Sustain", row });
            spectrum.setBounds (inner (left));
            envelope.setBounds (inner (row));
        }

        area.removeFromTop (margin);

        // Left hand, and the two knobs that set the instrument up.
        {
            auto row = area;
            auto setup = row.removeFromRight (150);
            row.removeFromRight (margin);
            panels.push_back ({ "Left Hand", row });
            panels.push_back ({ "Setup", setup });

            auto in = inner (row);
            auto controls = in.removeFromLeft (200);
            in.removeFromLeft (8);
            legato.setBounds (in);

            auto buttons = controls.removeFromTop (26);
            const int w = buttons.getWidth() / Plugin::numTechniques;
            for (auto& b : techniqueButtons)
                b.setBounds (buttons.removeFromLeft (w));

            controls.removeFromTop (8);
            auto switches = controls.removeFromTop (22);
            monoButton.setBounds (switches.removeFromLeft (switches.getWidth() / 2));
            fretlessButton.setBounds (switches);
            controls.removeFromTop (6);
            outFretsKnob->setBounds (controls.withWidth (70).withSizeKeepingCentre (70, controls.getHeight()));

            auto k = inner (setup).withSizeKeepingCentre (140, 64);
            tuneKnob->setBounds (k.removeFromLeft (70));
            levelKnob->setBounds (k);
        }
    }

private:
    static constexpr int margin = 6;

    Plugin* getPlugin() const  { return dynamic_cast<Plugin*> (plugin.get()); }

    //==========================================================================
    // Dragging one of the plugin's parameters from a drawing: a gesture per
    // drag, so automation recording sees one move.
    struct ParamDrag
    {
        te::AutomatableParameter::Ptr parameter;
        float startValue = 0.0f;

        void begin (te::AutomatableParameter::Ptr p)
        {
            parameter = std::move (p);
            startValue = parameter->getCurrentValue();
            parameter->parameterChangeGestureBegin();
        }

        void set (float value)
        {
            if (parameter != nullptr)
            {
                const auto range = parameter->getValueRange();
                parameter->setParameter (juce::jlimit (range.getStart(), range.getEnd(), value), juce::sendNotification);
            }
        }

        void end()
        {
            if (parameter != nullptr)
                parameter->parameterChangeGestureEnd();

            parameter = nullptr;
        }
    };

    static void resetParameter (te::AutomatableParameter& p)
    {
        if (const auto d = p.getDefaultValue())
        {
            p.parameterChangeGestureBegin();
            p.setParameter (*d, juce::sendNotification);
            p.parameterChangeGestureEnd();
        }
    }

    //==========================================================================
    void timerCallback() override
    {
        if (getPlugin() == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        refresh();
    }

    void refresh()
    {
        auto* p = getPlugin();
        auto* kit = Plugin::getKit();

        if (p == nullptr || kit == nullptr)
            return;

        for (auto* k : { attackKnob.get(), evenKnob.get(), tuneKnob.get(), levelKnob.get(), outFretsKnob.get() })
            k->refresh();

        const auto tech = (int) p->getTechnique();
        if (! techniqueButtons[(size_t) tech].getToggleState())
            techniqueButtons[(size_t) tech].setToggleState (true, juce::dontSendNotification);

        monoButton.setToggleState (p->isMono(), juce::dontSendNotification);
        fretlessButton.setToggleState (p->isFretless(), juce::dontSendNotification);

        if (p->getLastNote() >= 0)
        {
            shownNote = p->getLastNote();
            shownVelocity = p->getLastVelocity();
        }

        // Recompute the drawings only when something they show has moved.
        const auto options = p->makeOptions();
        const std::array<double, 12> key { (double) shownNote, shownVelocity, p->getTuneSemitones(), options.pick, options.mute,
                                           options.attack, options.bright, options.body, options.buzz, options.decay,
                                           options.gain, options.attackEq };

        if (key != lastKey)
        {
            lastKey = key;

            bass::makeSpec (*kit, shownNote, shownVelocity, p->getTuneSemitones(), spec);
            current.start (*kit, spec, kit->fs, options);
            recorded.start (*kit, spec, kit->fs, bass::Options {});

            noteLabel.setText ("showing " + juce::MidiMessage::getMidiNoteName (shownNote, true, true, 3)
                                 + juce::String::formatted (" (%.1f Hz)", spec.f0),
                               juce::dontSendNotification);
        }

        string.repaint();
        spectrum.repaint();
        envelope.repaint();
        legato.repaint();
    }

    //==========================================================================
    // The string, bridge on the left, out to the 12th fret.
    class StringGraph : public graphs::HandleGraph
    {
    public:
        explicit StringGraph (ElectricBassEditor& e) : editor (e)
        {
            setTooltip ("Drag the dot along the string to pluck somewhere else, the shaded edge to palm-mute. "
                        "Double-click either to put it back.");
        }

    protected:
        static constexpr float span = 0.5f;          // bridge to 12th fret
        static constexpr float muteWidth = 0.12f;    // how far the hand reaches at full mute

        float xFor (float position) const
        {
            const auto plot = getPlotArea().reduced (14.0f, 0.0f);
            return plot.getX() + plot.getWidth() * position / span;
        }

        float positionFor (float x) const
        {
            const auto plot = getPlotArea().reduced (14.0f, 0.0f);
            return juce::jlimit (0.0f, span, (x - plot.getX()) / plot.getWidth() * span);
        }

        float stringY() const  { return getPlotArea().getY() + getPlotArea().getHeight() * 0.68f; }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return;

            const auto y = stringY();
            const auto pick = p->pick->getCurrentValue();
            const auto mute = p->mute->getCurrentValue();
            const bool asRecorded = pick < 0.03f;

            // The fretboard, from the last (20th) fret to the 12th, and the
            // two pickups.
            const auto boardStart = std::pow (2.0f, -20.0f / 12.0f);
            g.setColour (juce::Colour (0xff2c2620));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (xFor (boardStart), y - 16.0f, plot.getRight(), y + 16.0f));
            g.setColour (juce::Colour (0xff5a5048));

            for (int fret = 12; fret <= 20; ++fret)
                g.drawVerticalLine ((int) xFor (std::pow (2.0f, (float) -fret / 12.0f)), y - 16.0f, y + 16.0f);

            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (graphs::colours::faint));
            g.drawText ("12th fret", juce::Rectangle<float> (70.0f, 14.0f).withPosition (xFor (0.5f) - 74.0f, y + 18.0f),
                        juce::Justification::centredRight);

            for (auto [at, name] : { std::pair { 0.10f, "bridge PU" }, std::pair { 0.19f, "neck PU" } })
            {
                const auto r = juce::Rectangle<float> (16.0f, 26.0f).withCentre ({ xFor (at), y });
                g.setColour (juce::Colour (0xff3b3b44));
                g.fillRoundedRectangle (r, 3.0f);
                g.setColour (juce::Colour (graphs::colours::faint));
                g.drawText (name, juce::Rectangle<float> (70.0f, 14.0f).withCentre ({ r.getCentreX(), y + 22.0f }),
                            juce::Justification::centred);
            }

            // The bridge.
            g.setColour (juce::Colour (0xff8a8a96));
            g.fillRect (juce::Rectangle<float> (6.0f, 30.0f).withCentre ({ xFor (0.0f) - 3.0f, y }));

            // The palm, over the bridge end.
            if (mute > 0.0f)
            {
                const auto r = juce::Rectangle<float>::leftTopRightBottom (xFor (0.0f) - 8.0f, y - 22.0f,
                                                                           xFor (0.012f + mute * muteWidth), y + 22.0f);
                g.setColour (juce::Colour (graphs::colours::teal).withAlpha (0.12f + 0.18f * mute));
                g.fillRoundedRectangle (r, 8.0f);
            }

            // The string, pulled at the plucking point.
            {
                juce::Path s;
                // The rest of the string runs on to the nut, off to the right.
                const auto apex = asRecorded ? 0.0f : 9.0f;
                const auto at = asRecorded ? 0.1f : pick;
                s.startNewSubPath (xFor (0.0f), y);
                s.lineTo (xFor (at), y - apex);
                s.lineTo (plot.getRight(), y - apex * (1.0f - span) / (1.0f - at));
                g.setColour (juce::Colour (0xffd8d0c0));
                g.strokePath (s, juce::PathStrokeType (2.0f));
            }

            // The harmonics the plucking point leaves: a node at the pick
            // takes that harmonic out.
            const int count = 16;
            auto comb = plot.reduced (14.0f, 0.0f).withTrimmedTop (6.0f).withHeight (plot.getHeight() * 0.36f);
            const auto barWidth = comb.getWidth() / (float) count;

            g.setColour (juce::Colour (graphs::colours::faint));
            g.drawText ("harmonics 1-16", comb.withHeight (14.0f).translated (0.0f, -2.0f), juce::Justification::topRight);

            for (int k = 1; k <= count; ++k)
            {
                const auto weight = asRecorded ? 1.0f : juce::jmax (0.03f, std::abs (std::sin (juce::MathConstants<float>::pi * (float) k * pick)));
                const auto mutedDb = mute * 4.0f * juce::jmax (0.0f, std::log2 ((float) k * 41.2f / 150.0f));
                const auto h = comb.getHeight() * weight * std::pow (10.0f, -mutedDb / 40.0f);
                const auto bar = juce::Rectangle<float> (barWidth * 0.55f, h)
                                     .withPosition (comb.getX() + barWidth * ((float) k - 1.0f + 0.22f), comb.getBottom() - h);
                g.setColour (juce::Colour (graphs::colours::accent).withAlpha (asRecorded ? 0.35f : 0.85f));
                g.fillRect (bar);
            }
        }

        std::vector<Handle> getHandles() override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return {};

            const auto pick = p->pick->getCurrentValue();
            const auto mute = p->mute->getCurrentValue();
            const bool asRecorded = pick < 0.03f;
            const auto y = stringY();

            return { { { xFor (asRecorded ? 0.1f : pick), y - (asRecorded ? 0.0f : 9.0f) }, juce::Colour (graphs::colours::accent),
                       "Pluck", p->pick->getCurrentValueAsStringWithLabel(), asRecorded },
                     { { xFor (0.012f + mute * muteWidth), y - 22.0f }, juce::Colour (graphs::colours::teal),
                       "Palm Mute", p->mute->getCurrentValueAsStringWithLabel(), mute <= 0.0f } };
        }

        void beginDrag (int handle) override
        {
            if (auto* p = editor.getPlugin())
                drag.begin (handle == 0 ? p->pick : p->mute);
        }

        void dragHandle (int handle, juce::Point<float> position, juce::Point<float>) override
        {
            const auto at = positionFor (position.x);

            if (handle == 0)
                drag.set (at < 0.03f ? 0.0f : at);
            else
                drag.set ((at - 0.012f) / muteWidth);

            editor.refresh();
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int handle) override
        {
            if (auto* p = editor.getPlugin())
                resetParameter (handle == 0 ? *p->pick : *p->mute);
        }

    private:
        ElectricBassEditor& editor;
        ParamDrag drag;
    };

    //==========================================================================
    // The harmonics 150ms in, on a log frequency axis.
    class SpectrumGraph : public graphs::HandleGraph
    {
    public:
        explicit SpectrumGraph (ElectricBassEditor& e) : editor (e)
        {
            setTooltip ("Drag the handles up or down: Tone tilts the spectrum, Body lifts the lowest harmonics, "
                        "Buzz the fret rattle. Grey is the note as recorded.");
        }

    protected:
        static constexpr double at = 0.15, lowHz = 30.0, highHz = 8000.0;
        static constexpr float topDb = 6.0f, bottomDb = -84.0f, handleBaseDb = -24.0f, toneHz = 5000.0f;

        juce::Rectangle<float> getPlotArea() const override  { return getLocalBounds().toFloat().withTrimmedBottom (16.0f); }

        float xFor (double hz) const
        {
            const auto plot = getPlotArea();
            return plot.getX() + plot.getWidth() * (float) (std::log (juce::jlimit (lowHz, highHz, hz) / lowHz) / std::log (highHz / lowHz));
        }

        float yFor (double db) const
        {
            const auto plot = getPlotArea();
            return juce::jmap ((float) juce::jlimit ((double) bottomDb, (double) topDb, db), topDb, bottomDb, plot.getY() + 4.0f, plot.getBottom());
        }

        float dbPerPixel() const  { return (topDb - bottomDb) / (getPlotArea().getHeight() - 4.0f); }

        // Everything against the recorded note's loudest harmonic.
        double reference() const
        {
            double peak = -300.0;
            const auto& v = editor.recorded;

            for (int c = 0; c < v.getNumComponents(); ++c)
                if (v.isComponentAudible (c))
                    peak = juce::jmax (peak, v.getComponentLevelDb (c, at));

            return peak;
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0 })
                graphs::drawGuide (g, { xFor (hz), plot.getY(), xFor (hz), plot.getBottom() });

            for (float db : { 0.0f, -30.0f, -60.0f })
                graphs::drawGuide (g, { plot.getX(), yFor (db), plot.getRight(), yFor (db) }, db == 0.0f ? "0 dB" : juce::String ((int) db));

            const auto ref = reference();

            auto bars = [&] (const bass::Voice& v, bool ghost)
            {
                for (int c = 0; c < v.getNumComponents(); ++c)
                {
                    if (! v.isComponentAudible (c))
                        continue;

                    const auto f = v.getComponentFrequency (c);
                    if (f < lowHz || f > highHz)
                        continue;

                    const auto db = v.getComponentLevelDb (c, at) - ref;
                    if (db < bottomDb)
                        continue;

                    const bool buzz = c >= v.getNumStringPartials();
                    const auto x = xFor (f);
                    const auto top = yFor (db);

                    if (ghost)
                    {
                        g.setColour (juce::Colour (graphs::colours::faint).withAlpha (0.8f));
                        g.drawHorizontalLine ((int) top, x - 2.5f, x + 2.5f);
                    }
                    else
                    {
                        g.setColour (juce::Colour (buzz ? graphs::colours::teal : graphs::colours::violet).withAlpha (buzz ? 0.55f : 0.9f));
                        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x - (buzz ? 0.5f : 1.0f), top, x + (buzz ? 0.5f : 1.0f), plot.getBottom()));
                    }
                }
            };

            bars (editor.current, false);
            bars (editor.recorded, true);

            // The tilt the Tone knob puts on everything, pivoting at 200Hz.
            if (auto* p = editor.getPlugin())
            {
                const auto tilt = p->tone->getCurrentValue();
                juce::Path line;
                line.startNewSubPath (xFor (lowHz), yFor (handleBaseDb + tilt * std::log2 (lowHz / 200.0)));
                line.lineTo (xFor (highHz), yFor (handleBaseDb + tilt * std::log2 (highHz / 200.0)));
                g.setColour (juce::Colour (graphs::colours::rose).withAlpha (0.6f));
                g.strokePath (line, juce::PathStrokeType (1.5f));
            }
        }

        void paintOverlay (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (graphs::colours::faint));

            for (auto [hz, text] : { std::pair { 50.0, "50" }, std::pair { 200.0, "200" }, std::pair { 1000.0, "1k" }, std::pair { 5000.0, "5k Hz" } })
                g.drawText (text, juce::Rectangle<float> (50.0f, 14.0f).withCentre ({ xFor (hz), plot.getBottom() + 9.0f }),
                            juce::Justification::centred);

            auto legend = plot.reduced (8.0f, 6.0f).removeFromTop (14.0f);
            for (auto [colour, name] : { std::pair { graphs::colours::teal, "buzz" }, std::pair { graphs::colours::violet, "string" },
                                         std::pair { graphs::colours::faint, "as recorded" } })
            {
                auto item = legend.removeFromRight ((float) std::strlen (name) * 7.0f + 16.0f);
                g.setColour (juce::Colour (colour));
                g.fillRect (item.removeFromLeft (8.0f).withSizeKeepingCentre (8.0f, 3.0f));
                g.setColour (juce::Colour (graphs::colours::dimText));
                g.drawText (name, item.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
            }
        }

        double buzzHz() const
        {
            // Where the buzz lives for this note: the middle of its comb.
            const auto& v = editor.recorded;
            const int n = v.getNumComponents() - v.getNumStringPartials();
            return n > 0 ? juce::jlimit (300.0, 1800.0, v.getComponentFrequency (v.getNumStringPartials() + n / 4)) : 1200.0;
        }

        std::vector<Handle> getHandles() override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return {};

            const auto tilt = p->tone->getCurrentValue();
            const auto body = p->body->getCurrentValue();
            const auto buzz = p->buzz->getCurrentValue();
            const auto f0 = editor.recorded.getNumComponents() > 0 ? editor.recorded.getComponentFrequency (0) : 41.2;

            return { { { xFor (toneHz), yFor (handleBaseDb + tilt * std::log2 (toneHz / 200.0)) }, juce::Colour (graphs::colours::rose),
                       "Tone", p->tone->getCurrentValueAsStringWithLabel(), std::abs (tilt) < 1.0e-4f },
                     { { xFor (f0 * 1.6), yFor (handleBaseDb + body) }, juce::Colour (graphs::colours::violet),
                       "Body", p->body->getCurrentValueAsStringWithLabel(), std::abs (body) < 1.0e-4f },
                     { { xFor (buzzHz()), yFor (buzz <= -60.0f ? bottomDb + 4.0f : handleBaseDb + buzz) }, juce::Colour (graphs::colours::teal),
                       "Fret Buzz", p->buzz->getCurrentValueAsStringWithLabel(), std::abs (buzz) < 1.0e-4f || buzz <= -60.0f } };
        }

        void beginDrag (int handle) override
        {
            if (auto* p = editor.getPlugin())
                drag.begin (handle == 0 ? p->tone : handle == 1 ? p->body : p->buzz);
        }

        void dragHandle (int handle, juce::Point<float>, juce::Point<float> delta) override
        {
            const auto db = -delta.y * dbPerPixel();

            if (handle == 0)
                drag.set (std::round ((drag.startValue + db / (float) std::log2 (toneHz / 200.0)) * 10.0f) / 10.0f);
            else if (handle == 1)
                drag.set (std::round ((drag.startValue + db) * 2.0f) / 2.0f);
            else
            {
                const auto from = drag.startValue <= -60.0f ? -40.0f : drag.startValue;
                const auto v = std::round ((from + db) * 2.0f) / 2.0f;
                drag.set (v <= -60.0f ? -61.0f : v);
            }

            editor.refresh();
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int handle) override
        {
            if (auto* p = editor.getPlugin())
                resetParameter (handle == 0 ? *p->tone : handle == 1 ? *p->body : *p->buzz);
        }

    private:
        ElectricBassEditor& editor;
        ParamDrag drag;
    };

    //==========================================================================
    // The level over time, on a square-root time axis.
    class EnvelopeGraph : public graphs::HandleGraph
    {
    public:
        explicit EnvelopeGraph (ElectricBassEditor& e) : editor (e)
        {
            setTooltip ("Drag the tail sideways to make the note ring longer or shorter. Grey is as recorded.");
        }

    protected:
        static constexpr double axisSeconds = 9.0;
        static constexpr float topDb = 6.0f, bottomDb = -66.0f;

        juce::Rectangle<float> getPlotArea() const override  { return getLocalBounds().toFloat().withTrimmedBottom (16.0f); }

        float xFor (double t) const
        {
            const auto plot = getPlotArea();
            return plot.getX() + plot.getWidth() * (float) std::sqrt (juce::jlimit (0.0, 1.0, t / axisSeconds));
        }

        double secondsFor (float x) const
        {
            const auto plot = getPlotArea();
            const auto u = juce::jlimit (0.0, 1.0, (double) ((x - plot.getX()) / plot.getWidth()));
            return u * u * axisSeconds;
        }

        float yFor (double db) const
        {
            const auto plot = getPlotArea();
            return juce::jmap ((float) juce::jlimit ((double) bottomDb, (double) topDb, db), topDb, bottomDb, plot.getY() + 4.0f, plot.getBottom());
        }

        double reference() const
        {
            return 20.0 * std::log10 (juce::jmax (1.0e-9, editor.recorded.levelAt (0.02)));
        }

        static double lengthOf (const bass::Voice& v)
        {
            return v.getLength() / v.getSampleRate();
        }

        juce::Path curve (const bass::Voice& v) const
        {
            juce::Path path;
            const auto ref = reference();
            const auto length = lengthOf (v);
            const auto plot = getPlotArea();
            bool started = false;

            for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
            {
                const auto t = secondsFor (x);

                if (t > length)
                    break;

                const auto y = yFor (20.0 * std::log10 (juce::jmax (1.0e-9, v.levelAt (t))) - ref);

                if (! started)
                {
                    path.startNewSubPath (x, y);
                    started = true;
                }
                else
                    path.lineTo (x, y);
            }

            return path;
        }

        // Where the note has fallen 40dB under its start.
        double tailSeconds (const bass::Voice& v) const
        {
            const auto start = v.levelAt (0.02);
            const auto length = lengthOf (v);

            for (double t = 0.05; t < length; t += 0.02)
                if (v.levelAt (t) < start * 0.01)
                    return t;

            return length;
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (float db : { 0.0f, -20.0f, -40.0f })
                graphs::drawGuide (g, { plot.getX(), yFor (db), plot.getRight(), yFor (db) }, db == 0.0f ? "0 dB" : juce::String ((int) db));

            const float dashes[] { 4.0f, 3.0f };
            juce::Path dashed;
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, curve (editor.recorded), dashes, 2);
            g.setColour (juce::Colour (graphs::colours::faint));
            g.fillPath (dashed);

            auto mine = curve (editor.current);
            juce::Path area (mine);
            area.lineTo (area.getCurrentPosition().x, plot.getBottom());
            area.lineTo (plot.getX(), plot.getBottom());
            area.closeSubPath();
            g.setColour (juce::Colour (graphs::colours::accent).withAlpha (0.15f));
            g.fillPath (area);
            g.setColour (juce::Colour (graphs::colours::accent));
            g.strokePath (mine, juce::PathStrokeType (1.6f));
        }

        void paintOverlay (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (graphs::colours::faint));

            for (double t : { 0.1, 0.5, 2.0, 4.0, 8.0 })
                g.drawText (t < 1.0 ? juce::String (juce::roundToInt (t * 1000.0)) + " ms" : juce::String (t, 0) + " s",
                            juce::Rectangle<float> (50.0f, 14.0f).withCentre ({ xFor (t), plot.getBottom() + 9.0f }),
                            juce::Justification::centred);
        }

        std::vector<Handle> getHandles() override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return {};

            const auto t = tailSeconds (editor.current);
            const auto ref = reference();
            const auto value = p->sustain->getCurrentValue();
            return { { { xFor (t), yFor (20.0 * std::log10 (juce::jmax (1.0e-9, editor.current.levelAt (t))) - ref) },
                       juce::Colour (graphs::colours::accent), "Sustain", p->sustain->getCurrentValueAsStringWithLabel(),
                       std::abs (value - 1.0f) < 1.0e-3f } };
        }

        void beginDrag (int) override
        {
            if (auto* p = editor.getPlugin())
            {
                drag.begin (p->sustain);
                startSeconds = tailSeconds (editor.current);
            }
        }

        void dragHandle (int, juce::Point<float> position, juce::Point<float>) override
        {
            // The tail follows the mouse: the time after the attack scales by
            // however far it was pulled.
            const auto attack = 0.09;
            const auto ratio = (secondsFor (position.x) - attack) / juce::jmax (0.01, startSeconds - attack);
            drag.set ((float) (drag.startValue * juce::jmax (0.05, ratio)));
            editor.refresh();
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int) override
        {
            if (auto* p = editor.getPlugin())
                resetParameter (*p->sustain);
        }

    private:
        ElectricBassEditor& editor;
        ParamDrag drag;
        double startSeconds = 1.0;
    };

    //==========================================================================
    // What happens to the pitch when a note is played over a held one: a step
    // (pluck, hammer) or a glide (slide). The handle on the glide's end is
    // Slide Time; on the hammer's step, the fret tap's level.
    class LegatoGraph : public graphs::HandleGraph
    {
    public:
        explicit LegatoGraph (ElectricBassEditor& e) : editor (e)
        {
            setTooltip ("What a note started over a held one does. In the piano roll, notes can be marked "
                        "one by one instead (Into the note, Out)");
        }

    protected:
        static constexpr double axisSeconds = 0.9, changeAt = 0.2;

        float xFor (double t) const
        {
            const auto plot = getPlotArea().reduced (10.0f, 0.0f);
            return plot.getX() + plot.getWidth() * (float) (t / axisSeconds);
        }

        double secondsFor (float x) const
        {
            const auto plot = getPlotArea().reduced (10.0f, 0.0f);
            return (x - plot.getX()) / plot.getWidth() * axisSeconds;
        }

        float lowY() const   { return getPlotArea().getBottom() - 22.0f; }
        float highY() const  { return getPlotArea().getY() + 26.0f; }

        // The slide's level, drawn as a line falling from the pitch line's
        // right end: how far a slide this long would fall untouched (the
        // dashed line) and how far it falls with Slide Hold (the solid one).
        static constexpr float slideDropDb = 12.0f;
        float levelY (float dropDb) const   { return lowY() - 26.0f + dropDb * 2.2f; }

        float tapTop (float tapDb) const
        {
            const auto h = juce::jmap (tapDb, -40.0f, 0.0f, 0.0f, (lowY() - highY()) * 0.9f);
            return highY() - 2.0f - h * 0.5f;
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return;

            const auto tech = p->getTechnique();
            const auto glide = (double) p->slideTime->getCurrentValue();

            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (graphs::colours::faint));
            auto text = plot.reduced (8.0f, 4.0f);
            g.drawText (tech == Technique::pluck ? "plucked again" : tech == Technique::hammer ? "hammered on" : "slid to",
                        text, juce::Justification::topRight);
            g.drawText ("held note", juce::Rectangle<float> (90.0f, 14.0f).withPosition (xFor (0.0), lowY() + 4.0f), juce::Justification::left);
            g.drawText ("next note, over it", juce::Rectangle<float> (140.0f, 14.0f).withPosition (xFor (changeAt) + 4.0f, highY() - 18.0f),
                        juce::Justification::left);

            juce::Path pitch;
            pitch.startNewSubPath (xFor (0.0), lowY());
            pitch.lineTo (xFor (changeAt), lowY());

            if (tech == Technique::slide)
            {
                // Five frets up, as the decoder will move it: in steps on a
                // fretted neck, a glide on a fretless one.
                constexpr double frets = 5.0;
                const bool fretted = ! p->isFretless();

                for (int i = 0; i <= 200; ++i)
                {
                    const auto u = i / 200.0;
                    const auto s = bass::Bend::semitonesAt (u, frets, fretted) / frets;
                    pitch.lineTo (xFor (changeAt + u * glide), juce::jmap ((float) s, lowY(), highY()));
                }

                g.setColour (juce::Colour (graphs::colours::grid));
                for (int f = 1; f < (int) frets; ++f)
                {
                    const auto y = juce::jmap ((float) f / (float) frets, lowY(), highY());
                    g.drawHorizontalLine ((int) y, xFor (changeAt), xFor (changeAt + glide));
                }
            }

            pitch.lineTo (xFor (changeAt + (tech == Technique::slide ? glide : 0.0)), highY());
            pitch.lineTo (xFor (axisSeconds), highY());

            g.setColour (juce::Colour (graphs::colours::accent));
            g.strokePath (pitch, juce::PathStrokeType (2.0f));

            if (tech == Technique::slide)
            {
                const auto hold = p->slideHold->getCurrentValue();
                const auto x0 = xFor (changeAt), x1 = xFor (changeAt + glide);

                const float dashes[] { 4.0f, 3.0f };
                g.setColour (juce::Colour (graphs::colours::faint));
                g.drawDashedLine ({ x0, levelY (0.0f), x1, levelY (slideDropDb) }, dashes, 2, 1.2f);

                g.setColour (juce::Colour (graphs::colours::teal));
                g.drawLine (x0, levelY (0.0f), x1, levelY (slideDropDb * (1.0f - hold)), 2.0f);

                g.setFont (uiFont (fonts::small));
                g.drawText ("level", juce::Rectangle<float> (40.0f, 14.0f).withPosition (x0 - 44.0f, levelY (0.0f) - 7.0f),
                            juce::Justification::centredRight);
            }

            // A pluck: a fresh attack at the change. A hammer: the tap, as
            // tall as it is loud.
            if (tech == Technique::pluck)
            {
                g.setColour (juce::Colour (graphs::colours::text));
                g.drawLine (xFor (changeAt), highY() - 14.0f, xFor (changeAt), highY() + 6.0f, 2.0f);
            }
            else if (tech == Technique::hammer)
            {
                const auto tap = p->tap->getCurrentValue();

                if (tap > -40.0f)
                {
                    g.setColour (juce::Colour (graphs::colours::teal).withAlpha (0.8f));
                    g.drawLine (xFor (changeAt), highY() - 2.0f, xFor (changeAt), tapTop (tap), 2.0f);
                }
            }
        }

        std::vector<Handle> getHandles() override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return {};

            if (p->getTechnique() == Technique::slide)
            {
                const auto end = xFor (changeAt + p->slideTime->getCurrentValue());
                return { { { end, highY() }, juce::Colour (graphs::colours::accent),
                           "Slide Time", p->slideTime->getCurrentValueAsStringWithLabel() },
                         { { end, levelY (slideDropDb * (1.0f - p->slideHold->getCurrentValue())) }, juce::Colour (graphs::colours::teal),
                           "Slide Hold", p->slideHold->getCurrentValueAsStringWithLabel() } };
            }

            if (p->getTechnique() == Technique::hammer)
            {
                const auto tap = p->tap->getCurrentValue();
                return { { { xFor (changeAt), tapTop (tap) }, juce::Colour (graphs::colours::teal),
                           "Fret Tap", p->tap->getCurrentValueAsStringWithLabel(), tap <= -40.0f } };
            }

            return {};
        }

        void beginDrag (int handle) override
        {
            if (auto* p = editor.getPlugin())
                drag.begin (p->getTechnique() != Technique::slide ? p->tap : handle == 0 ? p->slideTime : p->slideHold);
        }

        void dragHandle (int handle, juce::Point<float> position, juce::Point<float> delta) override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return;

            if (p->getTechnique() == Technique::slide && handle == 1)
            {
                // Up holds the level, down lets it fall as it would.
                const auto drop = (position.y - levelY (0.0f)) / (levelY (slideDropDb) - levelY (0.0f));
                drag.set (std::round ((1.0f - juce::jlimit (0.0f, 1.0f, drop)) * 100.0f) / 100.0f);
            }
            else if (p->getTechnique() == Technique::slide)
                drag.set ((float) (std::round ((secondsFor (position.x) - changeAt) * 200.0) / 200.0));
            else
            {
                const auto perPixel = 40.0f / ((lowY() - highY()) * 0.45f);
                drag.set (std::round (drag.startValue - delta.y * perPixel));
            }

            editor.refresh();
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int handle) override
        {
            if (auto* p = editor.getPlugin())
                resetParameter (p->getTechnique() != Technique::slide ? *p->tap : handle == 0 ? *p->slideTime : *p->slideHold);
        }

    private:
        ElectricBassEditor& editor;
        ParamDrag drag;
    };

    //==========================================================================
    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    StringGraph string;
    SpectrumGraph spectrum;
    EnvelopeGraph envelope;
    LegatoGraph legato;
    BassAmpPanel<Plugin> amp;
    std::unique_ptr<ParameterKnob> attackKnob, evenKnob, tuneKnob, levelKnob, outFretsKnob;
    std::array<juce::TextButton, Plugin::numTechniques> techniqueButtons;
    juce::ToggleButton monoButton { "Mono" }, fretlessButton { "Fretless" };
    juce::TextButton playButton { "Play" };
    juce::Label noteLabel;
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> panels;

    // What the drawings are of: a note, its voice as the knobs stand, and as
    // recorded. Message thread only.
    int shownNote = 28;
    float shownVelocity = 0.75f;
    bass::Spec spec;
    bass::Voice current, recorded;
    std::array<double, 12> lastKey {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ElectricBassEditor)
};

} // namespace carve::app
