#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "InstrumentGraphs.h"
#include "KnobPanel.h"
#include "plugins/InstrumentFx.h"

namespace carve::app
{

namespace fx = plugins::fx;

// The output stages of the recorded instruments, as panels: the drum kit's
// bus compressor and the bass's amp. Each is a titled box with an On switch
// in its title and the stage's knobs drawn the way the rest of those editors
// draw theirs -- the ones that shape a curve are handles on the curve.
//
// Both hold their plugin by SafeSelectable and do nothing once it is gone,
// as every editor here does: EditSync can delete a plugin under an open
// window.

namespace fxpanels
{
    // Dragging an automatable parameter from a drawing: one gesture a drag.
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
            if (parameter == nullptr)
                return;

            const auto range = parameter->getValueRange();
            parameter->setParameter (juce::jlimit (range.getStart(), range.getEnd(), value), juce::sendNotification);
        }

        void end()
        {
            if (parameter != nullptr)
                parameter->parameterChangeGestureEnd();

            parameter = nullptr;
        }
    };

    inline void reset (te::AutomatableParameter& p)
    {
        if (const auto d = p.getDefaultValue())
        {
            p.parameterChangeGestureBegin();
            p.setParameter (*d, juce::sendNotification);
            p.parameterChangeGestureEnd();
        }
    }

    // The On switch in a panel's title band.
    inline void styleSwitch (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (graphs::colours::accent));
        b.setColour (juce::TextButton::textColourOnId, juce::Colour (graphs::colours::background));
    }
} // namespace fxpanels

//==============================================================================
// The drum kit's bus compressor. Left, the transfer curve: the knee's handle
// is the threshold (drag along the diagonal), the handle at 0 dB in is the
// ratio (drag up or down). Middle, the last few seconds scrolling past: the
// kit's level in grey, the gain taken off it hanging from the top in orange,
// the threshold a line across both that can be dragged too. Right, the four
// knobs that are times and amounts rather than shapes.
template <typename Plugin>
class CompressorPanel : public juce::Component,
                        private juce::Timer
{
public:
    static constexpr int preferredHeight = 168;

    // As an effect's graph (see createEffectGraph) the window already has a
    // slider per parameter and a reduction meter, so the panel is only the
    // two drawings: no title, no switch, no knobs.
    explicit CompressorPanel (Plugin& p, bool graphsOnly = false)
        : plugin (&p), transfer (*this), history (*this), drawingsOnly (graphsOnly)
    {
        addAndMakeVisible (transfer);
        addAndMakeVisible (history);
        startTimerHz (30);

        if (drawingsOnly)
            return;

        fxpanels::styleSwitch (onButton);
        onButton.setTooltip ("Compress the whole kit");
        onButton.onClick = [this]
        {
            if (auto* d = get())
                d->setCompressorOn (onButton.getToggleState());
        };
        addAndMakeVisible (onButton);

        auto isAlive = [this] { return plugin != nullptr; };

        for (auto [param, name] : { std::pair { p.compAttack, "Attack" }, std::pair { p.compRelease, "Release" },
                                    std::pair { p.compMakeup, "Makeup" }, std::pair { p.compMix, "Mix" } })
        {
            auto k = std::make_unique<ParameterKnob> (*param, name, KnobSize::medium, isAlive);
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
        }
    }

    void paint (juce::Graphics& g) override
    {
        if (drawingsOnly)
            return;

        graphs::drawPanel (g, getLocalBounds(), "Bus Compressor");

        if (auto* d = get(); d != nullptr && ! d->isCompressorOn())
        {
            g.setColour (juce::Colour (graphs::colours::faint));
            g.setFont (uiFont (fonts::small));
            g.drawText ("off -- the kit as recorded", getLocalBounds().reduced (8, 0).removeFromTop (20).withTrimmedLeft (190),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        if (drawingsOnly)
        {
            auto area = getLocalBounds().reduced (6, 4);
            transfer.setBounds (area.removeFromLeft (area.getHeight() + 10));
            area.removeFromLeft (6);
            history.setBounds (area);
            return;
        }

        auto area = getLocalBounds().reduced (8, 0);
        auto title = area.removeFromTop (graphs::panelTitleHeight);
        onButton.setBounds (title.withX (title.getX() + 130).withWidth (46).reduced (0, 2));
        area.removeFromBottom (8);

        transfer.setBounds (area.removeFromLeft (area.getHeight() + 10));
        area.removeFromLeft (8);

        auto knobArea = area.removeFromRight (4 * 66);
        area.removeFromRight (8);
        history.setBounds (area);

        auto row = knobArea.withSizeKeepingCentre (knobArea.getWidth(), 64);
        for (auto& k : knobs)
            k->setBounds (row.removeFromLeft (66));
    }

private:
    Plugin* get() const  { return dynamic_cast<Plugin*> (plugin.get()); }

    void timerCallback() override
    {
        auto* d = get();

        if (d == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        if (onButton.getToggleState() != d->isCompressorOn())
        {
            onButton.setToggleState (d->isCompressorOn(), juce::dontSendNotification);
            repaint();
        }

        for (auto& k : knobs)
            k->refresh();

        transfer.repaint();
        history.repaint();
    }

    //==========================================================================
    class Transfer : public graphs::HandleGraph
    {
    public:
        explicit Transfer (CompressorPanel& o) : owner (o)
        {
            setTooltip ("Drag the knee along the diagonal for the threshold, the right-hand end up or down for the ratio");
        }

    protected:
        static constexpr float lowDb = -48.0f;

        juce::Point<float> toScreen (float inDb, float outDb) const
        {
            const auto plot = getPlotArea().reduced (6.0f);
            return { juce::jmap (inDb, lowDb, 0.0f, plot.getX(), plot.getRight()),
                     juce::jmap (outDb, lowDb, 0.0f, plot.getBottom(), plot.getY()) };
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (float db : { -12.0f, -24.0f, -36.0f })
            {
                graphs::drawGuide (g, { toScreen (db, lowDb).x, plot.getY(), toScreen (db, lowDb).x, plot.getBottom() });
                graphs::drawGuide (g, { plot.getX(), toScreen (lowDb, db).y, plot.getRight(), toScreen (lowDb, db).y });
            }

            g.setColour (juce::Colour (graphs::colours::grid));
            g.drawLine ({ toScreen (lowDb, lowDb), toScreen (0.0f, 0.0f) }, 1.0f);

            auto* d = owner.get();
            if (d == nullptr)
                return;

            const auto s = d->getCompressorSettings();
            juce::Path curve;

            for (float in = lowDb; in <= 0.0f; in += 0.5f)
            {
                const auto p = toScreen (in, fx::BusCompressor::outputDb (in, s.thresholdDb, s.ratio));
                if (in <= lowDb) curve.startNewSubPath (p); else curve.lineTo (p);
            }

            g.setColour (juce::Colour (graphs::colours::accent).withAlpha (d->isCompressorOn() ? 1.0f : 0.4f));
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            // Where the kit is right now, on the curve.
            const auto now = owner.get()->getCompressorHistory().get (0).inputDb;
            if (d->isCompressorOn() && now > lowDb)
            {
                g.setColour (juce::Colour (graphs::colours::text));
                g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (toScreen (now, fx::BusCompressor::outputDb (now, s.thresholdDb, s.ratio))));
            }
        }

        std::vector<Handle> getHandles() override
        {
            auto* d = owner.get();
            if (d == nullptr)
                return {};

            const auto s = d->getCompressorSettings();
            return { { toScreen (s.thresholdDb, s.thresholdDb), juce::Colour (graphs::colours::accent), "Threshold",
                       d->compThreshold->getCurrentValueAsStringWithLabel() },
                     { toScreen (0.0f, fx::BusCompressor::outputDb (0.0f, s.thresholdDb, s.ratio)), juce::Colour (graphs::colours::rose),
                       "Ratio", d->compRatio->getCurrentValueAsStringWithLabel() } };
        }

        void beginDrag (int h) override
        {
            if (auto* d = owner.get())
                drag.begin (h == 0 ? d->compThreshold : d->compRatio);
        }

        void dragHandle (int h, juce::Point<float> position, juce::Point<float>) override
        {
            auto* d = owner.get();
            if (d == nullptr)
                return;

            const auto plot = getPlotArea().reduced (6.0f);

            if (h == 0)
            {
                // Along the diagonal: whichever of across and up is further.
                const auto x = juce::jmap (position.x, plot.getX(), plot.getRight(), lowDb, 0.0f);
                const auto y = juce::jmap (position.y, plot.getBottom(), plot.getY(), lowDb, 0.0f);
                drag.set (std::round ((x + y) * 0.5f));
            }
            else
            {
                // The output level at 0 dB in, and the ratio that gives it.
                const auto threshold = d->compThreshold->getCurrentValue();
                const auto out = juce::jmap (position.y, plot.getBottom(), plot.getY(), lowDb, 0.0f);
                const auto over = -threshold, rise = juce::jmax (0.05f, out - threshold);
                drag.set (std::round (juce::jlimit (1.0f, 20.0f, over / rise) * 10.0f) / 10.0f);
            }
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int h) override
        {
            if (auto* d = owner.get())
                fxpanels::reset (h == 0 ? *d->compThreshold : *d->compRatio);
        }

    private:
        CompressorPanel& owner;
        fxpanels::ParamDrag drag;
    };

    //==========================================================================
    class History : public graphs::HandleGraph
    {
    public:
        explicit History (CompressorPanel& o) : owner (o)
        {
            setTooltip ("The kit's level (grey) and the gain the compressor takes off it (orange). Drag the line for the threshold.");
        }

    protected:
        static constexpr float lowDb = -48.0f, grRange = 24.0f;

        float yForLevel (float db) const
        {
            const auto plot = getPlotArea();
            return juce::jmap (juce::jlimit (lowDb, 0.0f, db), lowDb, 0.0f, plot.getBottom(), plot.getY() + 2.0f);
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (float db : { -12.0f, -24.0f, -36.0f })
                graphs::drawGuide (g, { plot.getX(), yForLevel (db), plot.getRight(), yForLevel (db) }, juce::String ((int) db));

            auto* d = owner.get();
            if (d == nullptr)
                return;

            const auto& h = d->getCompressorHistory();
            const int columns = juce::jmin (fx::LevelHistory::size, (int) plot.getWidth());
            const auto step = plot.getWidth() / (float) columns;

            juce::Path level, reduction;
            level.startNewSubPath (plot.getRight(), plot.getBottom());
            reduction.startNewSubPath (plot.getRight(), plot.getY());

            for (int i = 0; i < columns; ++i)
            {
                const auto e = h.get (i);
                const auto x = plot.getRight() - (float) i * step;
                level.lineTo (x, yForLevel (e.inputDb));
                reduction.lineTo (x, plot.getY() + juce::jlimit (0.0f, 1.0f, e.reductionDb / grRange) * plot.getHeight());
            }

            level.lineTo (plot.getRight() - (float) columns * step, plot.getBottom());
            level.closeSubPath();
            reduction.lineTo (plot.getRight() - (float) columns * step, plot.getY());
            reduction.closeSubPath();

            g.setColour (juce::Colour (graphs::colours::faint).withAlpha (0.5f));
            g.fillPath (level);

            if (d->isCompressorOn())
            {
                g.setColour (juce::Colour (graphs::colours::accent).withAlpha (0.55f));
                g.fillPath (reduction);
            }

            const auto ty = yForLevel (d->compThreshold->getCurrentValue());
            g.setColour (juce::Colour (graphs::colours::accent).withAlpha (d->isCompressorOn() ? 0.9f : 0.35f));
            g.drawHorizontalLine ((int) ty, plot.getX(), plot.getRight());

            g.setColour (juce::Colour (graphs::colours::dimText));
            g.setFont (uiFont (fonts::small));
            g.drawText ("GR " + juce::String (-d->getGainReductionDb(), 1) + " dB",
                        plot.reduced (8.0f, 4.0f), juce::Justification::bottomRight);
        }

        std::vector<Handle> getHandles() override
        {
            auto* d = owner.get();
            if (d == nullptr)
                return {};

            const auto plot = getPlotArea();
            return { { { plot.getRight() - 14.0f, yForLevel (d->compThreshold->getCurrentValue()) }, juce::Colour (graphs::colours::accent),
                       "Threshold", d->compThreshold->getCurrentValueAsStringWithLabel() } };
        }

        void beginDrag (int) override
        {
            if (auto* d = owner.get())
                drag.begin (d->compThreshold);
        }

        void dragHandle (int, juce::Point<float> position, juce::Point<float>) override
        {
            const auto plot = getPlotArea();
            drag.set (std::round (juce::jmap (position.y, plot.getBottom(), plot.getY() + 2.0f, lowDb, 0.0f)));
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int) override
        {
            if (auto* d = owner.get())
                fxpanels::reset (*d->compThreshold);
        }

    private:
        CompressorPanel& owner;
        fxpanels::ParamDrag drag;
    };

    te::SafeSelectable<te::Plugin> plugin;
    juce::TextButton onButton { "On" };
    Transfer transfer;
    History history;
    bool drawingsOnly = false;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
};

//==============================================================================
// The bass's amp. Left, the drive: the curve the signal goes through, clean
// diagonal behind it, with the blend back to clean already folded in -- its
// handle is the drive. Middle, the tone stack's response: Bass and Treble
// handles go up and down, Mid goes anywhere (frequency across, gain up).
// Right, Blend and Level.
template <typename Plugin>
class BassAmpPanel : public juce::Component,
                     private juce::Timer
{
public:
    static constexpr int preferredHeight = 168;

    explicit BassAmpPanel (Plugin& p) : plugin (&p), drive (*this), eq (*this)
    {
        fxpanels::styleSwitch (onButton);
        onButton.setTooltip ("Play the bass through the amp");
        onButton.onClick = [this]
        {
            if (auto* b = get())
                b->setAmpOn (onButton.getToggleState());
        };
        addAndMakeVisible (onButton);

        auto isAlive = [this] { return plugin != nullptr; };

        for (auto [param, name] : { std::pair { p.ampBlend, "Blend" }, std::pair { p.ampLevel, "Level" } })
        {
            auto k = std::make_unique<ParameterKnob> (*param, name, KnobSize::medium, isAlive);
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
        }

        addAndMakeVisible (drive);
        addAndMakeVisible (eq);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        graphs::drawPanel (g, getLocalBounds(), "Amp");

        if (auto* b = get(); b != nullptr && ! b->isAmpOn())
        {
            g.setColour (juce::Colour (graphs::colours::faint));
            g.setFont (uiFont (fonts::small));
            g.drawText ("off -- straight from the DI", getLocalBounds().reduced (8, 0).removeFromTop (20).withTrimmedLeft (110),
                        juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8, 0);
        auto title = area.removeFromTop (graphs::panelTitleHeight);
        onButton.setBounds (title.withX (title.getX() + 50).withWidth (46).reduced (0, 2));
        area.removeFromBottom (8);

        drive.setBounds (area.removeFromLeft (area.getHeight() + 30));
        area.removeFromLeft (8);

        auto knobArea = area.removeFromRight (2 * 70);
        area.removeFromRight (8);
        eq.setBounds (area);

        auto row = knobArea.withSizeKeepingCentre (knobArea.getWidth(), 64);
        for (auto& k : knobs)
            k->setBounds (row.removeFromLeft (70));
    }

private:
    Plugin* get() const  { return dynamic_cast<Plugin*> (plugin.get()); }

    void timerCallback() override
    {
        auto* b = get();

        if (b == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        if (onButton.getToggleState() != b->isAmpOn())
        {
            onButton.setToggleState (b->isAmpOn(), juce::dontSendNotification);
            repaint();
        }

        for (auto& k : knobs)
            k->refresh();

        drive.repaint();
        eq.repaint();
    }

    float activeAlpha() const
    {
        auto* b = get();
        return b != nullptr && b->isAmpOn() ? 1.0f : 0.4f;
    }

    //==========================================================================
    class Drive : public graphs::HandleGraph
    {
    public:
        explicit Drive (BassAmpPanel& o) : owner (o)
        {
            setTooltip ("The curve the bass goes through. Drag the handle up for more drive.");
        }

    protected:
        static constexpr float range = 0.8f;   // the input span drawn, either side of zero

        juce::Point<float> toScreen (float in, float out) const
        {
            const auto plot = getPlotArea().reduced (6.0f);
            return { juce::jmap (in, -range, range, plot.getX(), plot.getRight()),
                     juce::jmap (juce::jlimit (-range * 1.2f, range * 1.2f, out), -range, range, plot.getBottom(), plot.getY()) };
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            graphs::drawGuide (g, { toScreen (0.0f, 0.0f).x, plot.getY(), toScreen (0.0f, 0.0f).x, plot.getBottom() });
            graphs::drawGuide (g, { plot.getX(), toScreen (0.0f, 0.0f).y, plot.getRight(), toScreen (0.0f, 0.0f).y });

            g.setColour (juce::Colour (graphs::colours::grid));
            g.drawLine ({ toScreen (-range, -range), toScreen (range, range) }, 1.0f);

            auto* b = owner.get();
            if (b == nullptr)
                return;

            const auto s = b->getAmpSettings();
            juce::Path curve;

            for (float x = -range; x <= range; x += range / 60.0f)
            {
                const auto y = x + s.blend * (fx::BassAmp::shape (x, s.drive) - x);
                const auto p = toScreen (x, y);
                if (x <= -range) curve.startNewSubPath (p); else curve.lineTo (p);
            }

            g.setColour (juce::Colour (graphs::colours::rose).withAlpha (owner.activeAlpha()));
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            g.setColour (juce::Colour (graphs::colours::faint));
            g.setFont (uiFont (fonts::small));
            g.drawText ("DRIVE", plot.reduced (6.0f, 4.0f), juce::Justification::topLeft);
        }

        // On the curve, three quarters of the way out: high drive bends it
        // down there, so the handle sinks as the drive goes up -- dragging up
        // is still "more", the value being read off the drag, not the curve.
        std::vector<Handle> getHandles() override
        {
            auto* b = owner.get();
            if (b == nullptr)
                return {};

            const auto s = b->getAmpSettings();
            const auto plot = getPlotArea().reduced (6.0f);
            return { { { plot.getRight() - 14.0f, juce::jmap (s.drive, 0.0f, 1.0f, plot.getBottom() - 6.0f, plot.getY() + 6.0f) },
                       juce::Colour (graphs::colours::rose), "Drive", b->ampDrive->getCurrentValueAsStringWithLabel(), s.drive <= 0.0f } };
        }

        int handleForEmptyClick (juce::Point<float>) override  { return 0; }

        void beginDrag (int) override
        {
            if (auto* b = owner.get())
                drag.begin (b->ampDrive);
        }

        void dragHandle (int, juce::Point<float> position, juce::Point<float>) override
        {
            const auto plot = getPlotArea().reduced (6.0f);
            drag.set (std::round (juce::jmap (position.y, plot.getBottom() - 6.0f, plot.getY() + 6.0f, 0.0f, 1.0f) * 100.0f) / 100.0f);
        }

        void endDrag (int) override  { drag.end(); }

        void resetHandle (int) override
        {
            if (auto* b = owner.get())
                fxpanels::reset (*b->ampDrive);
        }

    private:
        BassAmpPanel& owner;
        fxpanels::ParamDrag drag;
    };

    //==========================================================================
    class Eq : public graphs::HandleGraph
    {
    public:
        explicit Eq (BassAmpPanel& o) : owner (o)
        {
            setTooltip ("Bass and Treble up or down; Mid anywhere -- across for its frequency, up or down for its gain");
        }

    protected:
        static constexpr double lowHz = 30.0, highHz = 10000.0, fs = 48000.0;
        static constexpr float rangeDb = 18.0f;

        float xFor (double hz) const
        {
            const auto plot = getPlotArea().withTrimmedBottom (14.0f);
            return plot.getX() + plot.getWidth() * (float) (std::log (hz / lowHz) / std::log (highHz / lowHz));
        }

        double hzFor (float x) const
        {
            const auto plot = getPlotArea().withTrimmedBottom (14.0f);
            return lowHz * std::pow (highHz / lowHz, juce::jlimit (0.0, 1.0, (double) ((x - plot.getX()) / plot.getWidth())));
        }

        float yFor (double db) const
        {
            const auto plot = getPlotArea().withTrimmedBottom (14.0f).reduced (0.0f, 6.0f);
            return juce::jmap ((float) juce::jlimit (-(double) rangeDb, (double) rangeDb, db), -rangeDb, rangeDb, plot.getBottom(), plot.getY());
        }

        float dbPerPixel() const
        {
            return 2.0f * rangeDb / (getPlotArea().getHeight() - 26.0f);
        }

        void paintPlot (juce::Graphics& g, juce::Rectangle<float> plot) override
        {
            for (double hz : { 100.0, 1000.0 })
                graphs::drawGuide (g, { xFor (hz), plot.getY(), xFor (hz), plot.getBottom() });

            graphs::drawGuide (g, { plot.getX(), yFor (0.0), plot.getRight(), yFor (0.0) }, "0 dB");

            g.setColour (juce::Colour (graphs::colours::faint));
            g.setFont (uiFont (fonts::small));
            for (auto [hz, text] : { std::pair { 100.0, "100" }, std::pair { 1000.0, "1k" }, std::pair { 5000.0, "5k Hz" } })
                g.drawText (text, juce::Rectangle<float> (50.0f, 14.0f).withCentre ({ xFor (hz), plot.getBottom() - 7.0f }),
                            juce::Justification::centred);
            g.drawText ("EQ", plot.reduced (6.0f, 4.0f), juce::Justification::topLeft);

            auto* b = owner.get();
            if (b == nullptr)
                return;

            const auto s = b->getAmpSettings();
            juce::Path curve;
            bool started = false;

            for (float x = plot.getX(); x <= plot.getRight(); x += 2.0f)
            {
                const auto y = yFor (fx::BassAmp::responseDb (s, hzFor (x), fs));
                if (! started) { curve.startNewSubPath (x, y); started = true; } else curve.lineTo (x, y);
            }

            juce::Path fill (curve);
            fill.lineTo (plot.getRight(), yFor (0.0));
            fill.lineTo (plot.getX(), yFor (0.0));
            fill.closeSubPath();
            g.setColour (juce::Colour (graphs::colours::violet).withAlpha (0.18f * owner.activeAlpha()));
            g.fillPath (fill);
            g.setColour (juce::Colour (graphs::colours::violet).withAlpha (owner.activeAlpha()));
            g.strokePath (curve, juce::PathStrokeType (2.0f));
        }

        std::vector<Handle> getHandles() override
        {
            auto* b = owner.get();
            if (b == nullptr)
                return {};

            const auto s = b->getAmpSettings();
            const auto colour = juce::Colour (graphs::colours::violet);
            return { { { xFor (fx::BassAmp::bassHz), yFor (s.bassDb) }, colour, "Bass", b->ampBass->getCurrentValueAsStringWithLabel(), std::abs (s.bassDb) < 0.01f },
                     { { xFor (s.midHz), yFor (s.midDb) }, colour, "Mid",
                       b->ampMid->getCurrentValueAsStringWithLabel() + " @ " + b->ampMidFreq->getCurrentValueAsStringWithLabel(), std::abs (s.midDb) < 0.01f },
                     { { xFor (fx::BassAmp::trebleHz), yFor (s.trebleDb) }, colour, "Treble", b->ampTreble->getCurrentValueAsStringWithLabel(), std::abs (s.trebleDb) < 0.01f } };
        }

        void beginDrag (int h) override
        {
            auto* b = owner.get();
            if (b == nullptr)
                return;

            drag.begin (h == 0 ? b->ampBass : h == 1 ? b->ampMid : b->ampTreble);

            if (h == 1)
                freqDrag.begin (b->ampMidFreq);
        }

        void dragHandle (int h, juce::Point<float> position, juce::Point<float> delta) override
        {
            drag.set (std::round ((drag.startValue - delta.y * dbPerPixel()) * 2.0f) / 2.0f);

            if (h == 1)
                freqDrag.set ((float) std::round (hzFor (position.x)));
        }

        void endDrag (int) override
        {
            drag.end();
            freqDrag.end();
        }

        void resetHandle (int h) override
        {
            auto* b = owner.get();
            if (b == nullptr)
                return;

            fxpanels::reset (h == 0 ? *b->ampBass : h == 1 ? *b->ampMid : *b->ampTreble);

            if (h == 1)
                fxpanels::reset (*b->ampMidFreq);
        }

    private:
        BassAmpPanel& owner;
        fxpanels::ParamDrag drag, freqDrag;
    };

    te::SafeSelectable<te::Plugin> plugin;
    juce::TextButton onButton { "On" };
    Drive drive;
    Eq eq;
    std::vector<std::unique_ptr<ParameterKnob>> knobs;
};

} // namespace carve::app
