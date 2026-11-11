#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "Fonts.h"
#include "InstrumentGraphs.h"
#include "InstrumentFxPanels.h"
#include "PresetManager.h"
#include "plugins/AcousticDrumsPlugin.h"

#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "plugins/CtdDecoder.h"
#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace carve::app
{

// The Acoustic Drums panel. The kit is on the left, a pad per drum, grouped
// as a drummer would sit at it; clicking a pad plays it and puts it on the
// right, where its seven knobs are three drawings:
//
//   Sound   the hit's envelope split into its three parts -- the recorded
//           attack, the drum's ringing body, the noise (wires, wash) -- each a
//           layer of its own colour with a handle on its peak: up and down is
//           that part's level. A fourth handle on the tail is the decay.
//   Pitch & Tone   one pad, tune across, brightness up.
//   Level   a fader.
//
// The envelope is the real thing: the decoder renders the hit part by part
// on a worker thread whenever a knob moves, so the drawing is what will be
// heard, not a sketch of it.
class AcousticDrumsEditor : public juce::Component,
                            private juce::Timer
{
public:
    using Plugin = plugins::AcousticDrumsPlugin;
    using Param = Plugin::Param;

    std::function<void (int note, int velocity)> onPreviewNote;

    static constexpr int defaultWidth = 920, defaultHeight = 424 + 6 + CompressorPanel<plugins::AcousticDrumsPlugin>::preferredHeight;

    explicit AcousticDrumsEditor (Plugin& drums)
        : plugin (&drums), presetBar (drums), sound (*this), pitchTone (*this), compressor (drums)
    {
        addAndMakeVisible (presetBar);
        addAndMakeVisible (compressor);

        for (int i = 0; i < Plugin::numPads; ++i)
        {
            auto pad = std::make_unique<PadButton> (*this, i);
            addAndMakeVisible (*pad);
            pads.push_back (std::move (pad));
        }

        drumName.setFont (uiFont (fonts::title, juce::Font::bold));
        drumName.setColour (juce::Label::textColourId, juce::Colour (graphs::colours::text));
        addAndMakeVisible (drumName);

        drumNote.setFont (uiFont (fonts::small));
        drumNote.setColour (juce::Label::textColourId, juce::Colour (graphs::colours::dimText));
        addAndMakeVisible (drumNote);

        resetButton.setTooltip ("Put this drum's knobs back as recorded");
        resetButton.onClick = [this]
        {
            if (auto* p = getPlugin())
                p->resetPad (selected);
        };
        addAndMakeVisible (resetButton);

        playButton.setTooltip ("Play this drum");
        playButton.onClick = [this] { play (selected, 100); };
        addAndMakeVisible (playButton);

        addAndMakeVisible (sound);
        addAndMakeVisible (pitchTone);

        level.setSliderStyle (juce::Slider::LinearVertical);
        level.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
        level.setRange (Plugin::getParamSpec (Param::level).minimum, Plugin::getParamSpec (Param::level).maximum, 0.1);
        level.setDoubleClickReturnValue (true, 0.0);
        level.textFromValueFunction = [] (double v) { return Plugin::formatValue (Param::level, (float) v); };
        level.valueFromTextFunction = [] (const juce::String& s) { return (double) s.getFloatValue(); };
        level.updateText();
        level.setColour (juce::Slider::trackColourId, juce::Colour (graphs::colours::accent));
        level.setColour (juce::Slider::backgroundColourId, juce::Colour (graphs::colours::plot));
        level.setColour (juce::Slider::thumbColourId, juce::Colour (graphs::colours::text));
        level.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        level.onValueChange = [this]
        {
            if (! refreshing)
                if (auto* p = getPlugin())
                    p->setValue (selected, Param::level, (float) level.getValue());
        };
        addAndMakeVisible (level);

        kitHint.setFont (uiFont (fonts::small));
        kitHint.setColour (juce::Label::textColourId, juce::Colour (graphs::colours::faint));
        kitHint.setJustificationType (juce::Justification::topLeft);
        kitHint.setText ("Click a drum to hear it and edit it. The number is its MIDI note; "
                         "the bar under it, its level. A dot marks a drum that has been changed.", juce::dontSendNotification);
        addAndMakeVisible (kitHint);

        legend.setFont (uiFont (fonts::small));
        legend.setColour (juce::Label::textColourId, juce::Colour (graphs::colours::faint));
        legend.setJustificationType (juce::Justification::topLeft);
        legend.setText ("Drag a layer's dot up or down for its level, the tail's sideways for the decay. "
                        "Double-click a dot to put it back.", juce::dontSendNotification);
        addAndMakeVisible (legend);

        worker.startThread();
        setSize (defaultWidth, defaultHeight);
        select (0);
        startTimerHz (30);
    }

    ~AcousticDrumsEditor() override
    {
        worker.stopThread (2000);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (graphs::colours::background));
        graphs::drawPanel (g, kitArea, "Kit");
        graphs::drawPanel (g, drumArea, {});

        g.setFont (uiFont (fonts::small));
        g.setColour (juce::Colour (graphs::colours::faint));

        for (const auto& [text, box] : groupLabels)
            g.drawText (text, box, juce::Justification::bottomLeft);

        for (const auto& [text, box] : panelLabels)
        {
            g.setColour (juce::Colour (graphs::colours::dimText));
            g.setFont (uiFont (fonts::small, juce::Font::bold));
            g.drawText (text.toUpperCase(), box, juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));
        area.reduce (margin, margin);

        // The bus compressor across the bottom: it is the whole kit's.
        compressor.setBounds (area.removeFromBottom (CompressorPanel<Plugin>::preferredHeight));
        area.removeFromBottom (margin);

        kitArea = area.removeFromLeft (kitWidth);
        area.removeFromLeft (margin);
        drumArea = area;

        layoutKit();
        layoutDrum();
    }

private:
    static constexpr int margin = 6, kitWidth = 7 * 50 + 6 * 4 + 16, padHeight = 42, padGap = 4, groupLabelHeight = 16;

    Plugin* getPlugin() const  { return dynamic_cast<Plugin*> (plugin.get()); }

    void play (int pad, int velocity)
    {
        if (onPreviewNote)
            onPreviewNote (Plugin::getPads()[(size_t) pad].notes[0], velocity);
    }

    void select (int pad)
    {
        selected = pad;
        const auto& info = Plugin::getPads()[(size_t) pad];
        drumName.setText (info.name, juce::dontSendNotification);

        juce::String notes;
        for (auto n : info.notes)
            if (n > 0)
                notes << (notes.isEmpty() ? "" : ", ") << juce::MidiMessage::getMidiNoteName (n, true, true, 3) << " (" << n << ")";

        drumNote.setText ("Note " + notes, juce::dontSendNotification);

        for (auto& p : pads)
            p->repaint();

        lastRequest = {};
        refresh();
    }

    //==========================================================================
    // Laying out the kit: a row of pads per group, toms and cymbals sharing
    // one, the percussion in two.
    void layoutKit()
    {
        groupLabels.clear();
        auto area = kitArea.reduced (8, 0);
        area.removeFromTop (graphs::panelTitleHeight);

        using Group = Plugin::Group;
        std::map<Group, std::vector<int>> byGroup;

        for (int i = 0; i < Plugin::numPads; ++i)
            byGroup[Plugin::getPads()[(size_t) i].group].push_back (i);

        auto placeRow = [this] (juce::Rectangle<int> row, const std::vector<int>& which)
        {
            for (auto i : which)
            {
                pads[(size_t) i]->setBounds (row.removeFromLeft (padWidth));
                row.removeFromLeft (padGap);
            }
        };

        auto groupRow = [&] (std::initializer_list<Group> groups)
        {
            auto labelRow = area.removeFromTop (groupLabelHeight);
            auto row = area.removeFromTop (padHeight);
            area.removeFromTop (6);

            for (auto group : groups)
            {
                const auto& members = byGroup[group];
                const int width = (int) members.size() * (padWidth + padGap);
                groupLabels.push_back ({ Plugin::getGroupName (group), labelRow.removeFromLeft (width).toFloat() });
                placeRow (row.removeFromLeft (width), members);
            }
        };

        groupRow ({ Group::kickSnare });
        groupRow ({ Group::hats });
        groupRow ({ Group::toms, Group::cymbals });

        const auto& perc = byGroup[Group::percussion];
        const auto half = perc.begin() + (std::ptrdiff_t) juce::jmin ((size_t) 7, perc.size());

        groupLabels.push_back ({ Plugin::getGroupName (Group::percussion), area.removeFromTop (groupLabelHeight).toFloat() });
        placeRow (area.removeFromTop (padHeight), std::vector<int> (perc.begin(), half));
        area.removeFromTop (padGap);
        placeRow (area.removeFromTop (padHeight), std::vector<int> (half, perc.end()));

        area.removeFromTop (10);
        kitHint.setBounds (area.removeFromTop (34));
    }

    void layoutDrum()
    {
        panelLabels.clear();
        auto area = drumArea.reduced (10, 8);

        auto header = area.removeFromTop (26);
        resetButton.setBounds (header.removeFromRight (64).reduced (0, 2));
        header.removeFromRight (6);
        playButton.setBounds (header.removeFromRight (56).reduced (0, 2));
        drumName.setBounds (header.removeFromLeft (180));
        drumNote.setBounds (header);

        area.removeFromTop (4);
        panelLabels.push_back ({ "Sound", area.removeFromTop (18).toFloat() });
        sound.setBounds (area.removeFromTop (190));
        area.removeFromTop (8);

        auto bottom = area;
        auto labels = bottom.removeFromTop (18);
        auto xyLabel = labels.removeFromLeft (220);
        labels.removeFromLeft (10);
        auto levelLabel = labels.removeFromLeft (70);
        panelLabels.push_back ({ "Pitch & Tone", xyLabel.toFloat() });
        panelLabels.push_back ({ "Level", levelLabel.toFloat() });

        pitchTone.setBounds (bottom.removeFromLeft (220));
        bottom.removeFromLeft (10);
        level.setBounds (bottom.removeFromLeft (70));
        bottom.removeFromLeft (14);
        legend.setBounds (bottom.withTrimmedTop (4));
    }

    //==========================================================================
    void timerCallback() override
    {
        auto* p = getPlugin();

        if (p == nullptr)
        {
            // EditSync deleted the plugin under us; the window is about to go.
            setEnabled (false);
            stopTimer();
            return;
        }

        const auto now = juce::Time::getMillisecondCounterHiRes();

        for (auto& pad : pads)
            pad->poll (*p, now);

        refresh();
    }

    // Pulls the knobs and the drawings into line with the plugin, and asks
    // for a new envelope when what it would show has changed.
    void refresh()
    {
        auto* p = getPlugin();

        if (p == nullptr)
            return;

        {
            const juce::ScopedValueSetter<bool> svs (refreshing, true);

            if (! level.isMouseButtonDown())
                level.setValue (p->getValue (selected, Param::level), juce::dontSendNotification);
        }

        std::array<float, Plugin::numParams> now {};
        for (int i = 0; i < Plugin::numParams; ++i)
            now[(size_t) i] = p->getValue (selected, (Param) i);

        if (lastRequest.pad != selected || lastRequest.values != now)
        {
            lastRequest = { selected, now };

            if (auto* inst = p->getInstrument (selected); inst != nullptr && ! inst->layers.empty())
            {
                // A firm hit -- three quarters up the velocity layers -- and
                // its first take: what the drum sounds like, steadily.
                const auto& layer = inst->layers[(size_t) juce::jmin ((int) inst->layers.size() - 1, (int) (inst->layers.size() * 3 / 4))];
                worker.request (selected, *inst, layer.front(), p->getRenderOptions (selected), defaultOptions (selected));
            }
        }

        if (auto result = worker.getResult(); result != nullptr && result != envelopes)
        {
            envelopes = result;
            sound.repaint();
        }

        pitchTone.repaint();
        sound.repaint();
    }

    // What the knobs at their defaults hand the decoder for this pad: the
    // scale the envelope is drawn against, so a part turned up is drawn up.
    ctd::RenderOptions defaultOptions (int pad) const
    {
        ctd::RenderOptions opts;

        if (auto* kit = Plugin::getKit())
            for (const auto& alias : kit->aliases)
                if (alias.id == Plugin::getPads()[(size_t) pad].id)
                    opts = ctd::withAlias (alias, opts);

        return opts;
    }

    //==========================================================================
    // The hit's three parts, each rendered alone, as RMS in short frames.
    struct Envelopes
    {
        int pad = -1;
        double frameSeconds = 0.004, attackSeconds = 0.0, axisSeconds = 1.0;
        std::array<std::vector<float>, 3> parts;   // attack, body, noise: dB against the default's peak
        std::vector<float> total;
        std::array<bool, 3> on {};
    };

    class Worker : public juce::Thread
    {
    public:
        Worker() : juce::Thread ("Carve Drum Envelope") {}

        void request (int pad, const ctd::Instrument& inst, const ctd::Hit& hit,
                      const ctd::RenderOptions& opts, const ctd::RenderOptions& defaults)
        {
            {
                const juce::ScopedLock sl (lock);
                pending = Job { pad, &inst, &hit, opts, defaults, true };
            }

            notify();
        }

        std::shared_ptr<const Envelopes> getResult() const
        {
            const juce::ScopedLock sl (lock);
            return result;
        }

        void run() override
        {
            auto* kit = Plugin::getKit();

            if (kit == nullptr)
                return;

            voice.reserve (*kit);

            while (! threadShouldExit())
            {
                Job job;

                {
                    const juce::ScopedLock sl (lock);
                    job = pending;
                    pending.waiting = false;
                }

                if (! job.waiting)
                {
                    wait (-1);
                    continue;
                }

                auto computed = compute (*kit, job);

                const juce::ScopedLock sl (lock);
                result = std::move (computed);
            }
        }

    private:
        struct Job
        {
            int pad = -1;
            const ctd::Instrument* inst = nullptr;
            const ctd::Hit* hit = nullptr;
            ctd::RenderOptions opts, defaults;
            bool waiting = false;
        };

        std::vector<float> renderPart (const ctd::Kit& kit, const Job& job, ctd::RenderOptions o, int part)
        {
            o.transient = part == 0 && o.transient;
            o.modes = part == 1 && o.modes;
            o.noise = part == 2 && o.noise;
            o.seed = 1;
            o.quality = ctd::Quality::Mid;
            voice.start (kit, *job.inst, *job.hit, o);
            std::vector<float> out ((size_t) voice.length());
            voice.render (out.data(), voice.length());
            return out;
        }

        std::shared_ptr<const Envelopes> compute (const ctd::Kit& kit, const Job& job)
        {
            // The default sound's peak, once per drum: the 0 dB of the drawing.
            if (peaks.count (job.pad) == 0)
            {
                std::vector<float> sum;

                for (int part = 0; part < 3; ++part)
                {
                    const auto y = renderPart (kit, job, job.defaults, part);
                    sum.resize (juce::jmax (sum.size(), y.size()), 0.0f);

                    for (size_t i = 0; i < y.size(); ++i)
                        sum[i] += y[i];
                }

                peaks[job.pad] = { rmsFrames (sum, kit.fs, 0.0).second, (double) sum.size() / kit.fs };
            }

            const auto [peak, defaultSeconds] = peaks[job.pad];

            auto env = std::make_shared<Envelopes>();
            env->pad = job.pad;
            env->attackSeconds = job.hit->ta;
            env->axisSeconds = juce::jmin (8.0, defaultSeconds * 2.2);
            env->on = { job.opts.transient, job.opts.modes, job.opts.noise };

            std::vector<float> sum;

            for (int part = 0; part < 3; ++part)
            {
                const auto y = renderPart (kit, job, job.opts, part);
                env->parts[(size_t) part] = rmsFrames (y, kit.fs, peak).first;
                sum.resize (juce::jmax (sum.size(), y.size()), 0.0f);

                for (size_t i = 0; i < y.size(); ++i)
                    sum[i] += y[i];
            }

            env->total = rmsFrames (sum, kit.fs, peak).first;
            return env;
        }

        // dB per 4ms frame against `reference`, and the loudest frame.
        static std::pair<std::vector<float>, float> rmsFrames (const std::vector<float>& y, double fs, float reference)
        {
            const int frame = juce::jmax (1, (int) std::round (0.004 * fs));
            std::vector<float> out;
            float loudest = -200.0f;

            for (size_t start = 0; start < y.size(); start += (size_t) frame)
            {
                double e = 0.0;
                const auto end = juce::jmin (y.size(), start + (size_t) frame);

                for (auto i = start; i < end; ++i)
                    e += (double) y[i] * y[i];

                out.push_back ((float) (e / (double) (end - start)));
            }

            // Noise is ragged frame to frame; three frames' average draws the
            // envelope rather than the randomness.
            std::vector<float> smoothed (out.size());

            for (size_t i = 0; i < out.size(); ++i)
            {
                const auto a = out[i > 0 ? i - 1 : i], b = out[i], c = out[i + 1 < out.size() ? i + 1 : i];
                const auto db = (float) (10.0 * std::log10 ((double) (a + 2.0f * b + c) / 4.0 + 1.0e-20));
                loudest = juce::jmax (loudest, db);
                smoothed[i] = db - reference;
            }

            return { smoothed, loudest };
        }

        juce::CriticalSection lock;
        Job pending;
        std::shared_ptr<const Envelopes> result;
        ctd::HitVoice voice;
        std::map<int, std::pair<float, double>> peaks;
    };

    //==========================================================================
    // One drum on the kit: its name and note, lit when it plays, outlined when
    // it is the one being edited, and with a thin bar along the bottom for its
    // level -- the kit's mixer at a glance.
    class PadButton : public juce::Button
    {
    public:
        PadButton (AcousticDrumsEditor& e, int padIndex)
            : juce::Button (Plugin::getPads()[(size_t) padIndex].name), editor (e), pad (padIndex)
        {
            const auto& info = Plugin::getPads()[(size_t) pad];
            setTooltip (juce::String (info.name) + " -- MIDI note " + juce::String (info.notes[0]));
            setWantsKeyboardFocus (false);
        }

        void clicked() override
        {
            editor.select (pad);
            editor.play (pad, 100);
        }

        void poll (const Plugin& p, double nowMs)
        {
            const auto hits = p.getHitCount (pad);

            if (hits != seenHits)
            {
                seenHits = hits;
                lastHitMs = nowMs;
            }

            const bool shouldLight = lastHitMs > 0.0 && nowMs - lastHitMs < 120.0;
            const auto levelNow = p.getValue (pad, Param::level);
            bool changed = false;

            for (int i = 0; i < Plugin::numParams; ++i)
                changed = changed || std::abs (p.getValue (pad, (Param) i) - Plugin::getParamSpec ((Param) i).defaultValue) > 1.0e-4f;

            if (shouldLight != lit || std::abs (levelNow - shownLevel) > 1.0e-4f || changed != edited)
            {
                lit = shouldLight;
                shownLevel = levelNow;
                edited = changed;
                repaint();
            }
        }

        void paintButton (juce::Graphics& g, bool highlighted, bool down) override
        {
            auto area = getLocalBounds().toFloat();
            const bool isSelected = editor.selected == pad;
            const bool bright = lit || down;

            g.setColour (bright ? juce::Colour (graphs::colours::accent)
                                : highlighted ? juce::Colour (0xff4a4a54) : juce::Colour (0xff3a3a42));
            g.fillRoundedRectangle (area, 4.0f);

            if (isSelected)
            {
                g.setColour (juce::Colour (graphs::colours::text));
                g.drawRoundedRectangle (area.reduced (0.75f), 4.0f, 1.5f);
            }

            const auto& info = Plugin::getPads()[(size_t) pad];
            const auto textColour = bright ? juce::Colour (graphs::colours::background) : juce::Colour (graphs::colours::text);
            auto box = getLocalBounds().reduced (2, 3);
            box.removeFromBottom (4);

            g.setColour (textColour);
            g.setFont (uiFont (fonts::small, juce::Font::bold));
            g.drawText (info.shortName, box.removeFromTop (box.getHeight() / 2 + 1), juce::Justification::centredBottom);
            g.setColour (textColour.withAlpha (0.65f));
            g.setFont (uiFont (fonts::small));
            g.drawText (juce::String (info.notes[0]), box, juce::Justification::centredTop);

            // The level: 0 dB at four fifths of the width.
            const auto& spec = Plugin::getParamSpec (Param::level);
            const auto fraction = juce::jmap (shownLevel, spec.minimum, spec.maximum, 0.0f, 1.0f);
            auto bar = area.reduced (5.0f, 0.0f).removeFromBottom (5.0f).withTrimmedBottom (2.0f);
            g.setColour (juce::Colour (0x40000000));
            g.fillRect (bar);
            g.setColour (bright ? juce::Colour (graphs::colours::background) : juce::Colour (graphs::colours::accent).withAlpha (0.8f));
            g.fillRect (bar.withWidth (bar.getWidth() * fraction));

            if (edited)
            {
                g.setColour (juce::Colour (graphs::colours::teal));
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withPosition (area.getRight() - 9.0f, area.getY() + 4.0f));
            }
        }

    private:
        AcousticDrumsEditor& editor;
        int pad;
        std::uint32_t seenHits = 0;
        double lastHitMs = 0.0;
        bool lit = false, edited = false;
        float shownLevel = 0.0f;
    };

    //==========================================================================
    // The envelope. Time runs on a square-root axis, so the attack gets room
    // and the tail still fits; level is dB against the drum's own peak as
    // recorded.
    class SoundGraph : public graphs::HandleGraph
    {
    public:
        explicit SoundGraph (AcousticDrumsEditor& e) : editor (e) {}

    protected:
        static constexpr float topDb = 12.0f, bottomDb = -60.0f;

        juce::Rectangle<float> getPlotArea() const override
        {
            return getLocalBounds().toFloat().withTrimmedBottom (16.0f);
        }

        float xFor (double seconds, juce::Rectangle<float> plot) const
        {
            const auto axis = editor.envelopes != nullptr ? editor.envelopes->axisSeconds : 1.0;
            return plot.getX() + plot.getWidth() * (float) std::sqrt (juce::jlimit (0.0, 1.0, seconds / axis));
        }

        double secondsFor (float x, juce::Rectangle<float> plot) const
        {
            const auto axis = editor.envelopes != nullptr ? editor.envelopes->axisSeconds : 1.0;
            const auto u = juce::jlimit (0.0, 1.0, (double) ((x - plot.getX()) / plot.getWidth()));
            return u * u * axis;
        }

        float yFor (float db, juce::Rectangle<float> plot) const
        {
            return juce::jmap (juce::jlimit (bottomDb, topDb, db), topDb, bottomDb, plot.getY() + 4.0f, plot.getBottom());
        }

        float dbPerPixel (juce::Rectangle<float> plot) const
        {
            return (topDb - bottomDb) / (plot.getHeight() - 4.0f);
        }

        static juce::Colour partColour (int part)
        {
            return juce::Colour (part == 0 ? graphs::colours::accent : part == 1 ? graphs::colours::violet : graphs::colours::teal);
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (float db : { 0.0f, -20.0f, -40.0f })
                graphs::drawGuide (g, { plot.getX(), yFor (db, plot), plot.getRight(), yFor (db, plot) },
                                   db == 0.0f ? "0 dB" : juce::String ((int) db));

            const auto env = editor.envelopes;

            if (env == nullptr || env->pad != editor.selected)
                return;

            // The attack's window, where the recording is kept as it was.
            const auto ax = xFor (env->attackSeconds, plot);
            g.setColour (juce::Colour (graphs::colours::accent).withAlpha (0.05f));
            g.fillRect (plot.withRight (ax));

            auto pathFor = [&] (const std::vector<float>& frames, bool closed)
            {
                juce::Path path;
                const auto bottom = plot.getBottom() + 2.0f;
                bool started = false;

                for (size_t i = 0; i < frames.size(); ++i)
                {
                    const auto x = xFor (((double) i + 0.5) * env->frameSeconds, plot);
                    const auto y = yFor (frames[i], plot);

                    if (! started)
                    {
                        path.startNewSubPath (x, closed ? bottom : y);
                        if (closed)
                            path.lineTo (x, y);
                        started = true;
                    }
                    else
                        path.lineTo (x, y);
                }

                if (closed && started)
                {
                    path.lineTo (path.getCurrentPosition().x, bottom);
                    path.closeSubPath();
                }

                return path;
            };

            // Back to front: noise, body, attack -- the attack is short and
            // would vanish under the others.
            for (int part : { 2, 1, 0 })
            {
                if (! env->on[(size_t) part])
                    continue;

                const auto& frames = env->parts[(size_t) part];
                g.setColour (partColour (part).withAlpha (0.22f));
                g.fillPath (pathFor (frames, true));
                g.setColour (partColour (part).withAlpha (0.9f));
                g.strokePath (pathFor (frames, false), juce::PathStrokeType (1.4f));
            }

            g.setColour (juce::Colour (graphs::colours::text).withAlpha (0.75f));
            g.strokePath (pathFor (env->total, false), juce::PathStrokeType (1.0f));
        }

        void paintOverlay (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            g.setColour (juce::Colour (graphs::colours::faint));
            g.setFont (uiFont (fonts::small));

            const auto env = editor.envelopes;
            const auto axis = env != nullptr ? env->axisSeconds : 1.0;

            float lastX = -1000.0f;

            for (double t : { 0.01, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0 })
            {
                if (t > axis * 0.95)
                    break;

                const auto x = xFor (t, plot);

                if (x - lastX < 56.0f)
                    continue;

                lastX = x;
                g.drawVerticalLine ((int) x, plot.getBottom(), plot.getBottom() + 3.0f);
                g.drawText (t < 1.0 ? juce::String (juce::roundToInt (t * 1000.0)) + " ms" : juce::String (t, 0) + " s",
                            juce::Rectangle<float> (60.0f, 14.0f).withPosition (x - 30.0f, plot.getBottom() + 2.0f),
                            juce::Justification::centred);
            }

            // The legend, top right.
            auto legendBox = plot.reduced (8.0f, 6.0f).removeFromTop (14.0f);
            for (int part : { 2, 1, 0 })
            {
                static const char* names[] { "Attack", "Body", "Noise" };
                const auto w = 58.0f;
                auto item = legendBox.removeFromRight (w);
                g.setColour (partColour (part));
                g.fillEllipse (item.removeFromLeft (8.0f).withSizeKeepingCentre (7.0f, 7.0f));
                g.setColour (juce::Colour (graphs::colours::dimText));
                g.drawText (names[part], item.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
            }
        }

        // 0..2 the parts' levels, 3 the decay.
        std::vector<Handle> getHandles() override
        {
            std::vector<Handle> out;
            auto* p = editor.getPlugin();
            const auto env = editor.envelopes;

            if (p == nullptr || env == nullptr || env->pad != editor.selected)
                return out;

            const auto plot = getPlotArea();
            static const Param params[] { Param::attack, Param::body, Param::noise };

            for (int part = 0; part < 3; ++part)
            {
                const auto& frames = env->parts[(size_t) part];
                const auto value = p->getValue (editor.selected, params[part]);

                // At the part's peak; at the floor, hollow, when it is off.
                size_t best = 0;
                for (size_t i = 0; i < frames.size(); ++i)
                    if (frames[i] > frames[best])
                        best = i;

                if (! env->on[(size_t) part] || frames.empty())
                {
                    const auto x = part == 0 ? 0.004 : part == 1 ? env->attackSeconds * 1.5 : env->attackSeconds * 3.0;
                    out.push_back ({ { xFor (x, plot), plot.getBottom() - 6.0f }, partColour (part),
                                     Plugin::getParamSpec (params[part]).name, "off", true });
                    continue;
                }

                // Two parts peaking at once would put one dot on the other.
                auto x = xFor (((double) best + 0.5) * env->frameSeconds, plot);
                for (const auto& h : out)
                    if (std::abs (h.position.x - x) < 14.0f && std::abs (h.position.y - yFor (frames[best], plot)) < 14.0f)
                        x += 16.0f;

                out.push_back ({ { x, yFor (frames[best], plot) }, partColour (part),
                                 Plugin::getParamSpec (params[part]).name,
                                 Plugin::formatValue (params[part], value), std::abs (value) < 1.0e-4f });
            }

            // The decay, where the whole sound has fallen 50 dB under its peak.
            {
                const auto& total = env->total;
                float peak = -200.0f;
                for (auto v : total)
                    peak = juce::jmax (peak, v);

                size_t end = total.empty() ? 0 : total.size() - 1;
                for (size_t i = total.size(); i-- > 0;)
                    if (total[i] > peak - 50.0f)
                    {
                        end = i;
                        break;
                    }

                const auto value = p->getValue (editor.selected, Param::decay);
                decayHandleSeconds = ((double) end + 0.5) * env->frameSeconds;
                out.push_back ({ { xFor (decayHandleSeconds, plot), yFor (total.empty() ? bottomDb : total[end], plot) },
                                 juce::Colour (graphs::colours::text), "Decay",
                                 Plugin::formatValue (Param::decay, value), std::abs (value - 1.0f) < 1.0e-4f });
            }

            return out;
        }

        void beginDrag (int handle) override
        {
            if (auto* p = editor.getPlugin())
            {
                startValue = p->getValue (editor.selected, handleParam (handle));
                startSeconds = decayHandleSeconds;
            }
        }

        void dragHandle (int handle, juce::Point<float> position, juce::Point<float> delta) override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return;

            const auto plot = getPlotArea();
            const auto param = handleParam (handle);
            const auto& spec = Plugin::getParamSpec (param);

            if (handle < 3)
            {
                // Off is the bottom of the knob's travel; dragging an off part
                // up brings it in from there.
                const auto from = startValue <= spec.minimum + 0.01f ? -24.0f : startValue;
                auto v = from - delta.y * dbPerPixel (plot);
                v = juce::jlimit (spec.minimum, spec.maximum, std::round (v * 2.0f) / 2.0f);
                p->setValue (editor.selected, param, v);
            }
            else
            {
                // The tail follows the mouse: the time after the attack scales
                // by however far it was pulled.
                const auto attack = editor.envelopes != nullptr ? editor.envelopes->attackSeconds : 0.0;
                const auto t = secondsFor (position.x, plot);
                const auto ratio = (t - attack) / juce::jmax (0.005, startSeconds - attack);
                p->setValue (editor.selected, param, (float) juce::jlimit ((double) spec.minimum, (double) spec.maximum,
                                                                          startValue * juce::jmax (0.05, ratio)));
            }

            editor.refresh();
        }

        void resetHandle (int handle) override
        {
            if (auto* p = editor.getPlugin())
            {
                const auto param = handleParam (handle);
                p->setValue (editor.selected, param, Plugin::getParamSpec (param).defaultValue);
            }
        }

    private:
        static Param handleParam (int handle)
        {
            static const Param params[] { Param::attack, Param::body, Param::noise, Param::decay };
            return params[juce::jlimit (0, 3, handle)];
        }

        AcousticDrumsEditor& editor;
        float startValue = 0.0f;
        double startSeconds = 1.0, decayHandleSeconds = 1.0;
    };

    //==========================================================================
    // Tune across, brightness up: one dot for the two knobs that decide what
    // pitch and colour a drum is. Behind it, the tilt the Tone knob puts on
    // the spectrum, so "brighter" is a line leaning up to the right.
    class PitchToneGraph : public graphs::HandleGraph
    {
    public:
        explicit PitchToneGraph (AcousticDrumsEditor& e) : editor (e)
        {
            setTooltip ("Drag across to tune the drum, up to make it brighter. Double-click the dot to put it back.");
        }

    protected:
        juce::Rectangle<float> getPlotArea() const override
        {
            return getLocalBounds().toFloat();
        }

        juce::Point<float> toScreen (float tune, float tone) const
        {
            const auto plot = getPlotArea().reduced (8.0f);
            const auto& tuneSpec = Plugin::getParamSpec (Param::tune);
            const auto& toneSpec = Plugin::getParamSpec (Param::tone);
            return { juce::jmap (tune, tuneSpec.minimum, tuneSpec.maximum, plot.getX(), plot.getRight()),
                     juce::jmap (tone, toneSpec.minimum, toneSpec.maximum, plot.getBottom(), plot.getY()) };
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            auto* p = editor.getPlugin();
            const auto centre = toScreen (0.0f, 0.0f);

            // Octave lines across, the flat line up the middle.
            for (float st : { -12.0f, 12.0f })
                graphs::drawGuide (g, { toScreen (st, 0.0f).x, plot.getY(), toScreen (st, 0.0f).x, plot.getBottom() });

            graphs::drawGuide (g, { centre.x, plot.getY(), centre.x, plot.getBottom() });
            graphs::drawGuide (g, { plot.getX(), centre.y, plot.getRight(), centre.y });

            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (graphs::colours::faint));
            auto inner = plot.reduced (6.0f, 4.0f);
            g.drawText ("brighter", inner, juce::Justification::topLeft);
            g.drawText ("darker", inner, juce::Justification::bottomLeft);
            g.drawText ("lower", inner, juce::Justification::centredLeft);
            g.drawText ("higher", inner, juce::Justification::centredRight);

            if (p == nullptr)
                return;

            // The spectral tilt itself, through the dot.
            const auto tone = p->getValue (editor.selected, Param::tone);
            const auto dot = toScreen (p->getValue (editor.selected, Param::tune), tone);
            const auto slope = -tone / 6.0f * 0.35f;
            g.setColour (juce::Colour (graphs::colours::rose).withAlpha (0.55f));
            g.drawLine (plot.getX(), dot.y - slope * (dot.x - plot.getX()),
                        plot.getRight(), dot.y + slope * (plot.getRight() - dot.x), 1.5f);
        }

        std::vector<Handle> getHandles() override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return {};

            const auto tune = p->getValue (editor.selected, Param::tune);
            const auto tone = p->getValue (editor.selected, Param::tone);
            return { { toScreen (tune, tone), juce::Colour (graphs::colours::rose), "Tune / Tone",
                       Plugin::formatValue (Param::tune, tune) + ", " + Plugin::formatValue (Param::tone, tone),
                       std::abs (tune) < 1.0e-4f && std::abs (tone) < 1.0e-4f } };
        }

        int handleForEmptyClick (juce::Point<float>) override  { return 0; }

        void dragHandle (int, juce::Point<float> position, juce::Point<float>) override
        {
            auto* p = editor.getPlugin();

            if (p == nullptr)
                return;

            const auto plot = getPlotArea().reduced (8.0f);
            const auto& tuneSpec = Plugin::getParamSpec (Param::tune);
            const auto& toneSpec = Plugin::getParamSpec (Param::tone);

            // Tune snaps to semitones unless Shift is down; both snap to the
            // middle near it.
            auto tune = juce::jmap (position.x, plot.getX(), plot.getRight(), tuneSpec.minimum, tuneSpec.maximum);
            auto tone = juce::jmap (position.y, plot.getBottom(), plot.getY(), toneSpec.minimum, toneSpec.maximum);
            tune = juce::ModifierKeys::currentModifiers.isShiftDown() ? std::round (tune * 10.0f) / 10.0f : std::round (tune);
            tone = std::abs (tone) < 0.25f ? 0.0f : std::round (tone * 4.0f) / 4.0f;

            p->setValue (editor.selected, Param::tune, juce::jlimit (tuneSpec.minimum, tuneSpec.maximum, tune));
            p->setValue (editor.selected, Param::tone, juce::jlimit (toneSpec.minimum, toneSpec.maximum, tone));
            editor.refresh();
        }

        void resetHandle (int) override
        {
            if (auto* p = editor.getPlugin())
            {
                p->setValue (editor.selected, Param::tune, 0.0f);
                p->setValue (editor.selected, Param::tone, 0.0f);
            }
        }

    private:
        AcousticDrumsEditor& editor;
    };

    //==========================================================================
    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    std::vector<std::unique_ptr<PadButton>> pads;
    juce::Label drumName, drumNote, legend, kitHint;
    juce::TextButton resetButton { "Reset" }, playButton { "Play" };
    SoundGraph sound;
    PitchToneGraph pitchTone;
    CompressorPanel<Plugin> compressor;
    juce::Slider level;

    juce::Rectangle<int> kitArea, drumArea;
    std::vector<std::pair<juce::String, juce::Rectangle<float>>> groupLabels, panelLabels;
    static constexpr int padWidth = 50;

    int selected = 0;
    bool refreshing = false;

    struct Request
    {
        int pad = -1;
        std::array<float, Plugin::numParams> values {};
    };

    Request lastRequest;
    Worker worker;
    std::shared_ptr<const Envelopes> envelopes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AcousticDrumsEditor)
};

} // namespace carve::app
