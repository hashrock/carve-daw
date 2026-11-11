#pragma once

#include <array>
#include <functional>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Fonts.h"
#include "TouchUi.h"
#include "plugins/MasterAnalyzerPlugin.h"
#include "plugins/ReferencePlayerPlugin.h"

namespace carve::app
{

// The mastering analyzer: what comes out of the master, measured the ways a
// finished track is judged.
//
//   Loudness   momentary, short-term and integrated LUFS (BS.1770), the
//              loudness range, true peak and the peak-to-loudness ratio,
//              each against a delivery target -- a streaming service's, a
//              broadcaster's -- and the last minute of it as a graph.
//   Spectrum   1/6-octave smoothed, tilted 4.5 dB/oct by default (so a mix
//              that sounds balanced draws roughly flat), with peak hold and
//              the reference's long-term spectrum laid over it at the level
//              it is played at.
//   Stereo     a vectorscope, the correlation (below zero: something will
//              vanish in mono), the balance and the peaks.
//
// It reads MasterAnalyzerPlugin, which measures what is heard -- so on the
// reference (A/B), it measures that.
class AnalyzerComponent : public juce::Component,
                          private juce::Timer
{
public:
    AnalyzerComponent (plugins::MasterAnalyzerPlugin& analyzerToRead, plugins::ReferencePlayerPlugin* referenceToRead)
        : analyzer (&analyzerToRead), reference (referenceToRead)
    {
        for (const auto& t : targets())
            targetBox.addItem (t.name, targetBox.getNumItems() + 1);

        targetBox.setSelectedId (1, juce::dontSendNotification);
        targetBox.setTooltip ("What the readings are judged against");
        targetBox.onChange = [this] { repaint(); };
        addAndMakeVisible (targetBox);

        resetButton.setTooltip ("Start the integrated loudness, range and true peak over");
        resetButton.onClick = [this] { if (auto* a = get()) a->requestReset(); };
        addAndMakeVisible (resetButton);

        resetOnPlay.setToggleState (true, juce::dontSendNotification);
        resetOnPlay.setTooltip ("Start them over whenever playback starts");
        resetOnPlay.onClick = [this] { if (auto* a = get()) a->setResetOnPlay (resetOnPlay.getToggleState()); };
        addAndMakeVisible (resetOnPlay);

        auto setupBox = [this] (juce::ComboBox& box, std::initializer_list<const char*> items, int selected, const char* tip)
        {
            for (auto* item : items)
                box.addItem (item, box.getNumItems() + 1);

            box.setSelectedId (selected, juce::dontSendNotification);
            box.setTooltip (tip);
            box.onChange = [this] { columnsDirty = true; };
            addAndMakeVisible (box);
        };

        setupBox (smoothingBox, { "1/3 oct", "1/6 oct", "1/12 oct" }, 2, "How wide a band each point of the curve averages");
        setupBox (slopeBox, { "Flat", "3 dB/oct", "4.5 dB/oct" }, 3, "Tilt the curve up towards the top, so pink noise -- "
                                                                       "and a balanced mix, roughly -- draws flat");
        setupBox (speedBox, { "Fast", "Slow" }, 2, "How quickly the curve follows: fast to see a hit, slow for the balance");

        holdToggle.setTooltip ("Keep each band's highest level, falling slowly");
        holdToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (holdToggle);

        referenceToggle.setTooltip ("Lay the reference's long-term spectrum over the mix's, at the level it is played at");
        referenceToggle.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (referenceToggle);

        power.assign ((size_t) fftSize / 2 + 1, 0.0f);
        frame.assign ((size_t) fftSize * 2, 0.0f);
        left.assign ((size_t) fftSize, 0.0f);
        right.assign ((size_t) fftSize, 0.0f);

        setSize (980, 620);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (background));

        paintPanel (g, loudnessArea, "Loudness");
        paintPanel (g, historyArea, "History");
        paintPanel (g, spectrumArea, "Spectrum");
        paintPanel (g, stereoArea, "Stereo");

        paintLoudness (g);
        paintHistory (g);
        paintSpectrum (g);
        paintStereo (g);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        auto top = area.removeFromTop (196);
        area.removeFromTop (8);

        loudnessArea = top.removeFromLeft (360);
        top.removeFromLeft (8);
        historyArea = top;

        stereoArea = area.removeFromRight (220);
        area.removeFromRight (8);
        spectrumArea = area;

        // Controls in the panels' title bands.
        {
            auto band = loudnessArea.reduced (8, 0).removeFromTop (titleHeight).reduced (0, 3);
            resetButton.setBounds (band.removeFromRight (56));
            band.removeFromRight (6);
            targetBox.setBounds (band.removeFromRight (190));
        }
        {
            auto band = historyArea.reduced (8, 0).removeFromTop (titleHeight).reduced (0, 3);
            resetOnPlay.setBounds (band.removeFromRight (130));
        }
        {
            auto band = spectrumArea.reduced (8, 0).removeFromTop (titleHeight).reduced (0, 3);
            referenceToggle.setBounds (band.removeFromRight (100));
            holdToggle.setBounds (band.removeFromRight (64));
            band.removeFromRight (6);
            speedBox.setBounds (band.removeFromRight (74));
            band.removeFromRight (4);
            slopeBox.setBounds (band.removeFromRight (96));
            band.removeFromRight (4);
            smoothingBox.setBounds (band.removeFromRight (86));
        }

        columnsDirty = true;
    }

private:
    static constexpr juce::uint32 background = 0xff232327, panel = 0xff2a2a31, plot = 0xff1d1d22, grid = 0xff34343d,
                                  text = 0xfff0f0f4, dim = 0xffb8b8c0, faint = 0xff6c6c78, accent = 0xffe0a24f,
                                  good = 0xff6cc47a, warn = 0xffe0a24f, bad = 0xffe0604f, cool = 0xff6aa8e0, teal = 0xff5fc4b8;
    static constexpr int titleHeight = 24;
    static constexpr int fftOrder = 13, fftSize = 1 << fftOrder;
    static constexpr float lowHz = 20.0f, highHz = 20000.0f, topDb = -10.0f, bottomDb = -100.0f;

    struct Target
    {
        const char* name;
        double lufs, truePeak;
    };

    static const std::vector<Target>& targets()
    {
        static const std::vector<Target> list {
            { "Streaming  -14 LUFS / -1 dBTP", -14.0, -1.0 },
            { "Apple Music  -16 LUFS / -1 dBTP", -16.0, -1.0 },
            { "Loud master  -9 LUFS / -0.3 dBTP", -9.0, -0.3 },
            { "Broadcast EBU R128  -23 / -1", -23.0, -1.0 },
            { "Podcast  -16 LUFS / -1 dBTP", -16.0, -1.0 },
        };
        return list;
    }

    const Target& target() const  { return targets()[(size_t) juce::jlimit (0, (int) targets().size() - 1, targetBox.getSelectedId() - 1)]; }

    plugins::MasterAnalyzerPlugin* get() const  { return dynamic_cast<plugins::MasterAnalyzerPlugin*> (analyzer.get()); }

    void paintPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title) const
    {
        g.setColour (juce::Colour (panel));
        g.fillRoundedRectangle (area.toFloat(), 4.0f);
        g.setColour (juce::Colour (dim));
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText (title.toUpperCase(), area.reduced (8, 0).removeFromTop (titleHeight), juce::Justification::centredLeft);
    }

    static juce::String lufsText (double v, int decimals = 1)
    {
        return std::isfinite (v) ? juce::String (v, decimals) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
    }

    //==========================================================================
    void paintLoudness (juce::Graphics& g) const
    {
        const auto r = readings;
        const auto& t = target();
        auto area = loudnessArea.reduced (12, 0).withTrimmedTop (titleHeight + 4).withTrimmedBottom (10);

        // The integrated figure, large, coloured by where it sits against the
        // target: within a LU is on it.
        auto big = area.removeFromTop (64);
        const auto diff = r.integrated - t.lufs;
        const auto colour = ! std::isfinite (r.integrated) ? juce::Colour (faint)
                          : std::abs (diff) <= 1.0 ? juce::Colour (good)
                          : diff > 0.0 ? juce::Colour (warn) : juce::Colour (cool);

        g.setColour (colour);
        g.setFont (juce::FontOptions (46.0f, juce::Font::bold));
        g.drawText (lufsText (r.integrated), big.removeFromLeft (160), juce::Justification::centredLeft);

        g.setFont (uiFont (fonts::normal));
        g.setColour (juce::Colour (dim));
        auto side = big;
        g.drawText ("LUFS integrated", side.removeFromTop (30), juce::Justification::bottomLeft);
        g.setColour (colour);
        g.drawText (! std::isfinite (r.integrated) ? juce::String ("play to measure")
                    : std::abs (diff) <= 1.0 ? juce::String ("on target")
                    : juce::String (std::abs (diff), 1) + (diff > 0.0 ? " LU louder than target" : " LU quieter than target"),
                    side, juce::Justification::topLeft);

        area.removeFromTop (8);

        // The rest, two columns of label and value.
        struct Item { const char* label; juce::String value; juce::Colour colour; };
        const auto tpOver = std::isfinite (r.truePeakDb) && r.truePeakDb > t.truePeak;
        const auto plr = std::isfinite (r.truePeakDb) && std::isfinite (r.integrated) ? r.truePeakDb - r.integrated : NAN;

        const Item items[] {
            { "Momentary", lufsText (r.momentary) + " LUFS", juce::Colour (text) },
            { "Short-term", lufsText (r.shortTerm) + " LUFS", juce::Colour (text) },
            { "Range (LRA)", juce::String (r.range, 1) + " LU", juce::Colour (text) },
            { "True peak", lufsText (r.truePeakDb) + " dBTP", tpOver ? juce::Colour (bad) : juce::Colour (text) },
            { "PLR", std::isfinite (plr) ? juce::String (plr, 1) + " dB" : juce::String ("-"), juce::Colour (text) },
            { "Ceiling", juce::String (t.truePeak, 1) + " dBTP", juce::Colour (dim) },
        };

        const int rowH = 20, colW = area.getWidth() / 2;

        for (int i = 0; i < 6; ++i)
        {
            auto cell = juce::Rectangle<int> (area.getX() + (i % 2) * colW, area.getY() + (i / 2) * rowH, colW - 8, rowH);
            g.setFont (uiFont (fonts::small));
            g.setColour (juce::Colour (dim));
            g.drawText (items[i].label, cell, juce::Justification::centredLeft);
            g.setFont (uiFont (fonts::normal, juce::Font::bold));
            g.setColour (items[i].colour);
            g.drawText (items[i].value, cell, juce::Justification::centredRight);
        }
    }

    //==========================================================================
    void paintHistory (juce::Graphics& g) const
    {
        auto plotArea = historyArea.reduced (8, 0).withTrimmedTop (titleHeight).withTrimmedBottom (18).toFloat();
        g.setColour (juce::Colour (plot));
        g.fillRoundedRectangle (plotArea, 3.0f);

        constexpr float lo = -40.0f, hi = 0.0f;
        auto y = [&] (float v) { return juce::jmap (juce::jlimit (lo, hi, v), lo, hi, plotArea.getBottom() - 2.0f, plotArea.getY() + 2.0f); };

        g.setFont (uiFont (fonts::small));

        for (float db : { -10.0f, -20.0f, -30.0f })
        {
            g.setColour (juce::Colour (grid));
            g.drawHorizontalLine ((int) y (db), plotArea.getX(), plotArea.getRight());
            g.setColour (juce::Colour (faint));
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (30.0f, 12.0f).withPosition (plotArea.getX() + 3.0f, y (db) - 13.0f),
                        juce::Justification::left);
        }

        // The target, as a band a LU either side.
        const auto t = (float) target().lufs;
        g.setColour (juce::Colour (good).withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (plotArea.getX(), y (t + 1.0f), plotArea.getRight(), y (t - 1.0f)));
        g.setColour (juce::Colour (good).withAlpha (0.6f));
        g.drawHorizontalLine ((int) y (t), plotArea.getX(), plotArea.getRight());

        auto line = [&] (const std::vector<float>& values, juce::Colour colour, float thickness)
        {
            juce::Path path;
            bool started = false;
            const auto n = (int) values.size();

            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (values[(size_t) i]))
                {
                    started = false;
                    continue;
                }

                const auto x = plotArea.getX() + plotArea.getWidth() * (float) i / (float) (n - 1);

                if (! started)
                {
                    path.startNewSubPath (x, y (values[(size_t) i]));
                    started = true;
                }
                else
                    path.lineTo (x, y (values[(size_t) i]));
            }

            g.setColour (colour);
            g.strokePath (path, juce::PathStrokeType (thickness));
        };

        line (historyMomentary, juce::Colour (faint), 1.0f);
        line (historyShortTerm, juce::Colour (accent), 2.0f);

        g.setColour (juce::Colour (faint));
        g.drawText ("last 60 s  -  short-term (orange), momentary (grey), target band (green)",
                    historyArea.reduced (8, 0).removeFromBottom (18), juce::Justification::centredLeft);
    }

    //==========================================================================
    juce::Rectangle<float> spectrumPlot() const
    {
        return spectrumArea.reduced (8, 0).withTrimmedTop (titleHeight).withTrimmedBottom (20).toFloat();
    }

    float xForHz (float hz, juce::Rectangle<float> p) const
    {
        return p.getX() + p.getWidth() * std::log (hz / lowHz) / std::log (highHz / lowHz);
    }

    float yForDb (float db, juce::Rectangle<float> p) const
    {
        return juce::jmap (juce::jlimit (bottomDb, topDb + 12.0f, db), bottomDb, topDb, p.getBottom(), p.getY() + 4.0f);
    }

    // For each pixel column, the FFT bins its band covers.
    void buildColumns()
    {
        const auto p = spectrumPlot();
        const int width = juce::jmax (2, (int) p.getWidth());
        const double fractions[] { 1.0 / 3.0, 1.0 / 6.0, 1.0 / 12.0 };
        const auto fraction = fractions[juce::jlimit (0, 2, smoothingBox.getSelectedId() - 1)];
        const double slopes[] { 0.0, 3.0, 4.5 };
        slopeDbPerOctave = (float) slopes[juce::jlimit (0, 2, slopeBox.getSelectedId() - 1)];
        const auto rate = readings.sampleRate > 0.0 ? readings.sampleRate : 48000.0;

        columns.assign ((size_t) width, {});

        for (int x = 0; x < width; ++x)
        {
            const auto hz = lowHz * std::pow (highHz / lowHz, (double) x / (width - 1));
            const auto lo = hz * std::pow (2.0, -fraction / 2.0), hi = hz * std::pow (2.0, fraction / 2.0);
            auto& c = columns[(size_t) x];
            c.hz = (float) hz;
            c.first = juce::jlimit (1, fftSize / 2, (int) std::floor (lo / rate * fftSize));
            c.last = juce::jlimit (c.first, fftSize / 2, (int) std::ceil (hi / rate * fftSize));
        }

        hold.assign ((size_t) width, -200.0f);
        columnsDirty = false;
    }

    float columnDb (const std::vector<float>& binPower, const auto& column) const
    {
        double sum = 0.0;

        for (int b = column.first; b <= column.last; ++b)
            sum += binPower[(size_t) b];

        const auto db = 10.0 * std::log10 (sum / (column.last - column.first + 1) + 1.0e-20);
        return (float) db + slopeDbPerOctave * std::log2 (column.hz / 1000.0f);
    }

    void paintSpectrum (juce::Graphics& g)
    {
        if (columnsDirty)
            buildColumns();

        const auto p = spectrumPlot();
        g.setColour (juce::Colour (plot));
        g.fillRoundedRectangle (p, 3.0f);

        g.setFont (uiFont (fonts::small));

        for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const auto x = xForHz (hz, p);
            g.setColour (juce::Colour (grid));
            g.drawVerticalLine ((int) x, p.getY(), p.getBottom());
            g.setColour (juce::Colour (faint));
            g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (40.0f, 16.0f).withCentre ({ x, p.getBottom() + 10.0f }), juce::Justification::centred);
        }

        for (float db = topDb - 12.0f; db > bottomDb; db -= 12.0f)
        {
            g.setColour (juce::Colour (grid));
            g.drawHorizontalLine ((int) yForDb (db, p), p.getX(), p.getRight());
            g.setColour (juce::Colour (faint));
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (30.0f, 12.0f).withPosition (p.getX() + 3.0f, yForDb (db, p) - 13.0f),
                        juce::Justification::left);
        }

        if (columns.empty())
            return;

        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (p.toNearestInt());

        auto curve = [&] (const std::vector<float>& binPower, std::vector<float>* holdTo)
        {
            juce::Path path;

            for (size_t x = 0; x < columns.size(); ++x)
            {
                auto db = columnDb (binPower, columns[x]);

                if (holdTo != nullptr)
                    (*holdTo)[x] = db = juce::jmax (db, (*holdTo)[x] - 0.15f);

                const auto px = p.getX() + (float) x, py = yForDb (db, p);
                if (x == 0) path.startNewSubPath (px, py); else path.lineTo (px, py);
            }

            return path;
        };

        // The reference, as played: its spectrum moved by the gain it gets.
        if (referenceToggle.getToggleState() && reference != nullptr)
            if (const auto spectrum = reference->getReferenceSpectrum();
                spectrum != nullptr && (int) spectrum->db.size() == fftSize / 2 + 1)
            {
                std::vector<float> refPower (spectrum->db.size());
                const auto gain = (float) reference->getReferenceGainDb();

                for (size_t b = 0; b < refPower.size(); ++b)
                    refPower[b] = std::pow (10.0f, (spectrum->db[b] + gain) / 10.0f);

                g.setColour (juce::Colour (teal).withAlpha (0.9f));
                g.strokePath (curve (refPower, nullptr), juce::PathStrokeType (1.6f));
            }

        auto mix = curve (power, nullptr);
        juce::Path fill (mix);
        fill.lineTo (p.getRight(), p.getBottom());
        fill.lineTo (p.getX(), p.getBottom());
        fill.closeSubPath();
        g.setColour (juce::Colour (accent).withAlpha (0.18f));
        g.fillPath (fill);
        g.setColour (juce::Colour (accent));
        g.strokePath (mix, juce::PathStrokeType (1.6f));

        if (holdToggle.getToggleState())
        {
            g.setColour (juce::Colour (text).withAlpha (0.45f));
            g.strokePath (curve (power, &hold), juce::PathStrokeType (1.0f));
        }

        g.setColour (juce::Colour (faint));
        g.drawText (juce::String ("mix (orange)") + (reference != nullptr && reference->getReferenceSpectrum() != nullptr
                                                      && referenceToggle.getToggleState() ? ", reference (teal)" : ""),
                    p.reduced (8.0f, 4.0f), juce::Justification::topRight);
    }

    //==========================================================================
    void paintStereo (juce::Graphics& g) const
    {
        auto area = stereoArea.reduced (10, 0).withTrimmedTop (titleHeight).withTrimmedBottom (10);
        const auto r = readings;

        // The vectorscope: mid up, side across, so a mono signal is a vertical
        // line and wide material a cloud.
        auto scope = area.removeFromTop (area.getWidth()).toFloat();
        g.setColour (juce::Colour (plot));
        g.fillRoundedRectangle (scope, 3.0f);
        g.setColour (juce::Colour (grid));
        g.drawLine (scope.getCentreX(), scope.getY(), scope.getCentreX(), scope.getBottom());
        g.drawLine (scope.getX(), scope.getCentreY(), scope.getRight(), scope.getCentreY());
        g.drawLine (scope.getX(), scope.getY(), scope.getRight(), scope.getBottom(), 0.5f);
        g.drawLine (scope.getRight(), scope.getY(), scope.getX(), scope.getBottom(), 0.5f);

        g.setFont (uiFont (fonts::small));
        g.setColour (juce::Colour (faint));
        g.drawText ("L", scope.reduced (6.0f), juce::Justification::topLeft);
        g.drawText ("R", scope.reduced (6.0f), juce::Justification::topRight);

        const auto half = scope.getWidth() * 0.48f;
        g.setColour (juce::Colour (accent).withAlpha (0.35f));

        for (int i = 0; i < scopePoints; ++i)
        {
            const auto l = left[(size_t) (fftSize - scopePoints + i)] * scopeScale, rr = right[(size_t) (fftSize - scopePoints + i)] * scopeScale;
            const auto side = (l - rr) * 0.7071f, mid = (l + rr) * 0.7071f;
            g.fillRect (scope.getCentreX() + juce::jlimit (-1.0f, 1.0f, side) * half,
                        scope.getCentreY() - juce::jlimit (-1.0f, 1.0f, mid) * half, 1.5f, 1.5f);
        }

        area.removeFromTop (10);

        // A bar from -1 to +1 with a marker.
        auto bar = [&] (juce::Rectangle<int> row, const juce::String& label, double value, juce::Colour colour,
                        const char* leftText, const char* rightText)
        {
            g.setColour (juce::Colour (dim));
            g.drawText (label, row.removeFromTop (14), juce::Justification::centredLeft);
            auto track = row.removeFromTop (12).toFloat();
            g.setColour (juce::Colour (plot));
            g.fillRoundedRectangle (track, 3.0f);
            g.setColour (juce::Colour (grid));
            g.drawVerticalLine ((int) track.getCentreX(), track.getY(), track.getBottom());
            const auto x = juce::jmap ((float) juce::jlimit (-1.0, 1.0, value), -1.0f, 1.0f, track.getX(), track.getRight());
            g.setColour (colour);
            g.fillRect (juce::Rectangle<float> (4.0f, track.getHeight()).withCentre ({ x, track.getCentreY() }));
            g.setColour (juce::Colour (faint));
            g.drawText (leftText, row.removeFromTop (14), juce::Justification::centredLeft);
            g.drawText (rightText, row.withY (row.getY() - 14).withHeight (14), juce::Justification::centredRight);
        };

        bar (area.removeFromTop (42), "Correlation  " + juce::String (r.correlation, 2), r.correlation,
             r.correlation < 0.0 ? juce::Colour (bad) : r.correlation < 0.3 ? juce::Colour (warn) : juce::Colour (good), "-1", "+1");
        area.removeFromTop (6);
        bar (area.removeFromTop (42), "Balance", r.balance, juce::Colour (accent), "L", "R");
        area.removeFromTop (6);

        // The peaks, as two bars.
        g.setColour (juce::Colour (dim));
        g.drawText ("Peak  L " + lufsText (shownPeak[0]) + "  R " + lufsText (shownPeak[1]) + " dB",
                    area.removeFromTop (14), juce::Justification::centredLeft);

        for (int ch = 0; ch < 2; ++ch)
        {
            auto track = area.removeFromTop (8).toFloat();
            area.removeFromTop (3);
            g.setColour (juce::Colour (plot));
            g.fillRect (track);
            const auto fraction = juce::jmap ((float) juce::jlimit (-60.0, 0.0, shownPeak[ch]), -60.0f, 0.0f, 0.0f, 1.0f);
            g.setColour (shownPeak[ch] > -0.3 ? juce::Colour (bad) : shownPeak[ch] > -6.0 ? juce::Colour (warn) : juce::Colour (good));
            g.fillRect (track.withWidth (track.getWidth() * fraction));
        }
    }

    //==========================================================================
    void timerCallback() override
    {
        auto* a = get();

        if (a == nullptr)
        {
            stopTimer();
            return;
        }

        readings = a->getReadings();

        for (int ch = 0; ch < 2; ++ch)
            shownPeak[ch] = juce::jmax (readings.samplePeakDb[ch], shownPeak[ch] - 1.0);

        // The spectrum: the latest fftSize samples, mid, Hann-windowed, its
        // power averaged with what was shown before.
        a->copyRecentSamples (left.data(), right.data(), fftSize);

        for (int i = 0; i < fftSize; ++i)
            frame[(size_t) i] = 0.5f * (left[(size_t) i] + right[(size_t) i]);

        std::fill (frame.begin() + fftSize, frame.end(), 0.0f);
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data());

        const auto norm = 2.0f / (fftSize * 0.5f);
        const auto keep = speedBox.getSelectedId() == 1 ? 0.5f : 0.92f;

        for (size_t b = 0; b < power.size(); ++b)
        {
            const auto m = frame[b] * norm;
            power[b] = keep * power[b] + (1.0f - keep) * m * m;
        }

        // The scope scaled to the material, so a quiet passage still shows
        // its shape.
        float peak = 0.0f;
        for (int i = fftSize - scopePoints; i < fftSize; ++i)
            peak = juce::jmax (peak, std::abs (left[(size_t) i]), std::abs (right[(size_t) i]));

        const auto wanted = 0.9f / juce::jmax (0.02f, peak);
        scopeScale = scopeScale + 0.2f * (wanted - scopeScale);

        historyMomentary.resize ((size_t) plugins::MasterAnalyzerPlugin::historySize);
        historyShortTerm.resize ((size_t) plugins::MasterAnalyzerPlugin::historySize);
        a->copyHistory (historyMomentary.data(), historyShortTerm.data(), plugins::MasterAnalyzerPlugin::historySize);

        repaint();
    }

    te::SafeSelectable<te::Plugin> analyzer;
    plugins::ReferencePlayerPlugin* reference;

    juce::ComboBox targetBox, smoothingBox, slopeBox, speedBox;
    juce::TextButton resetButton { "Reset" };
    juce::ToggleButton resetOnPlay { "Reset on play" }, holdToggle { "Hold" }, referenceToggle { "Reference" };

    juce::Rectangle<int> loudnessArea, historyArea, spectrumArea, stereoArea;

    plugins::MasterAnalyzerPlugin::Readings readings;
    double shownPeak[2] { -INFINITY, -INFINITY };

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<float> frame, power, left, right, hold, historyMomentary, historyShortTerm;

    struct Column { float hz = 0.0f; int first = 1, last = 1; };
    std::vector<Column> columns;
    bool columnsDirty = true;
    float slopeDbPerOctave = 4.5f;

    static constexpr int scopePoints = 2048;
    float scopeScale = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AnalyzerComponent)
};

//==============================================================================
// The analyzer as a window of its own, resizable, remembered where it was.
class AnalyzerWindow : public juce::DocumentWindow
{
public:
    AnalyzerWindow (te::Engine& engine, plugins::MasterAnalyzerPlugin& analyzer, plugins::ReferencePlayerPlugin* reference,
                    std::function<void()> onCloseCallback, std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Analyzer", juce::Colour (0xff232327), juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)), onKey (std::move (keyHandler)), content (analyzer, reference)
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 500;
        settings = std::make_unique<juce::PropertiesFile> (
            engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("windows.settings"), options);

        setContentNonOwned (&content, true);
        useSystemTitleBar (*this);
        setResizable (true, false);
        setResizeLimits (760, 520, 3000, 2000);

        if (const auto saved = juce::Rectangle<int>::fromString (settings->getValue (boundsKey)); ! saved.isEmpty())
            setBounds (saved);
        else
            centreWithSize (getWidth(), getHeight());

        boundsAreLive = true;
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override  { if (onClose) onClose(); }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (onKey != nullptr && onKey (key))
            return true;
        return juce::DocumentWindow::keyPressed (key);
    }

    void moved() override    { juce::DocumentWindow::moved(); storeBounds(); }
    void resized() override  { juce::DocumentWindow::resized(); storeBounds(); }

private:
    static constexpr const char* boundsKey = "analyzerWindowBounds";

    void storeBounds()
    {
        if (boundsAreLive && settings != nullptr)
            settings->setValue (boundsKey, getBounds().toString());
    }

    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    std::unique_ptr<juce::PropertiesFile> settings;
    AnalyzerComponent content;
    bool boundsAreLive = false;
};

} // namespace carve::app
