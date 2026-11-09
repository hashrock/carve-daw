#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "Fonts.h"
#include "KnobPanel.h"
#include "PresetManager.h"
#include "plugins/GabberKickPlugin.h"

namespace carve::app
{

namespace gabber = plugins::gabber;

// The Gabber Kick's panel, top to bottom in signal order: the source, its
// drive stages, the output. The sound is mostly in the shapes of things, so
// the shapes are what is edited -- pitch fall, loudness, each stage's EQ sweep
// and transfer curve are graphs with handles to drag, and knobs are left for
// the values that have no shape.
//
// Everything reads the patch from the plugin when it paints and writes it
// back whole on an edit, so a preset or an undo needs no path of its own into
// the panel. The drive stages share one panel under a row of tabs, so a long
// chain costs a tab rather than a screen; the panel is addressed by index in
// the chain, and picking a tab just points it somewhere else.
class GabberKickEditor : public juce::Component,
                         private juce::Timer
{
public:
    std::function<void (int note, int velocity)> onPreviewNote;

    // The window opens at the default and can be pulled larger; the graphs
    // take up whatever it is given. Below the minimum, the viewport scrolls.
    static constexpr int defaultWidth = 860, minWidth = 720;
    static int getMinHeight()  { return minHeight; }

    explicit GabberKickEditor (plugins::GabberKickPlugin& kick)
        : plugin (&kick), presetBar (kick)
    {
        addAndMakeVisible (presetBar);

        hit.onClick = [this]
        {
            if (onPreviewNote)
                onPreviewNote (plugins::GabberKickPlugin::rootNote, 127);
        };
        hit.setTooltip ("Play the kick (MIDI note 36; other notes repitch it)");
        addAndMakeVisible (hit);

        status.setFont (uiFont (fonts::small));
        status.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, juce::Colour (dimText));
        addAndMakeVisible (status);

        buildSource();
        buildOutput();

        stageTabs = std::make_unique<StageTabs> (*this);
        addAndMakeVisible (*stageTabs);
        stagePanel = std::make_unique<StagePanel> (*this);
        addAndMakeVisible (*stagePanel);
        noStages.setFont (uiFont (fonts::small));
        noStages.setJustificationType (juce::Justification::centred);
        noStages.setColour (juce::Label::textColourId, juce::Colour (dimText));
        noStages.setText ("No drive stages: the kick goes straight to the output. + adds one.", juce::dontSendNotification);
        addChildComponent (noStages);

        setSize (defaultWidth, minHeight);
        selectStage (0);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));
        g.setColour (juce::Colour (panelColour));

        for (auto area : { sourceArea, outputArea, stageArea })
            g.fillRoundedRectangle (area.toFloat(), 3.0f);

        g.setColour (juce::Colour (dimText));
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText ("KICK", sourceArea.reduced (8, 3).removeFromTop (titleHeight), juce::Justification::centredLeft);
        g.drawText ("OUTPUT", outputArea.reduced (8, 3).removeFromTop (titleHeight), juce::Justification::centredLeft);
        g.drawText ("DRIVE STAGES", stagesLabelArea.withTrimmedLeft (8), juce::Justification::centredLeft);

        g.setFont (uiFont (fonts::small));
        g.drawText ("EQ -> Shaper, left to right. Drag a tab to reorder.",
                    stagesLabelArea.withTrimmedLeft (104), juce::Justification::centredLeft);

        // Ties the selected tab to the panel it opens.
        g.setColour (juce::Colour (startColour));
        g.fillRect (stageArea.withHeight (2));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        presetBar.setBounds (area.removeFromTop (PresetBar::height));
        area.reduce (margin, margin);

        auto top = area.removeFromTop (rowHeight);
        hit.setBounds (top.removeFromLeft (70));
        status.setBounds (top);
        area.removeFromTop (gap);

        const auto extra = juce::jmax (0, getHeight() - minHeight);
        const auto extraSource = extra * 3 / 10, extraOutput = extra * 2 / 10;

        sourceArea = area.removeFromTop (sourceHeight + extraSource);
        {
            auto inner = sourceArea.reduced (8, 3);
            inner.removeFromTop (titleHeight);
            auto knobs = inner.removeFromRight (sourceKnobsWidth);
            pitchGraph->setBounds (inner.removeFromLeft ((inner.getWidth() - gap) / 2));
            inner.removeFromLeft (gap);
            ampGraph->setBounds (inner);
            sourceKnobs->setBounds (knobs.withTrimmedLeft (gap));
        }
        area.removeFromTop (gap);

        stagesLabelArea = area.removeFromTop (stagesLabelHeight);
        stageTabs->setBounds (area.removeFromTop (tabHeight));
        stageArea = area.removeFromTop (stageHeight + extra - extraSource - extraOutput);
        stagePanel->setBounds (stageArea);
        noStages.setBounds (stageArea);
        area.removeFromTop (gap);

        outputArea = area.removeFromTop (outputHeight + extraOutput);
        {
            auto inner = outputArea.reduced (8, 3);
            inner.removeFromTop (titleHeight);
            auto controls = inner.removeFromRight (sourceKnobsWidth);
            waveform->setBounds (inner);
            controls.removeFromLeft (gap);
            outputKnobs->setBounds (controls.removeFromLeft (70));
            normalize.setBounds (controls.removeFromTop (rowHeight));
            controls.removeFromTop (4);
            exportButton.setBounds (controls.removeFromTop (rowHeight));
        }
    }

private:
    using Patch = gabber::Patch;
    using Stage = gabber::Stage;
    using Shape = gabber::Shape;

    static constexpr juce::uint32 panelColour = 0xff2a2a31, dimText = 0xffb8b8c0,
                                  startColour = 0xffe0a24f, endColour = 0xff9a86f0,
                                  gridColour = 0xff3a3a44, plotColour = 0xff1d1d22;

    static constexpr int margin = 6, gap = 6, rowHeight = 24, titleHeight = 14,
                         sourceHeight = 150, stageHeight = 176, tabHeight = 26, stagesLabelHeight = 20, outputHeight = 116,
                         sourceKnobsWidth = 170;

    static constexpr int minHeight = PresetBar::height + margin * 2 + rowHeight + gap + sourceHeight + gap
                                     + stagesLabelHeight + tabHeight + stageHeight + gap + outputHeight;

    //==========================================================================
    // A plot with draggable handles. Both axes run 0..1 in plot space, y down;
    // what that means in Hz or seconds is up to whoever fills it in.
    class Graph : public juce::Component
    {
    public:
        struct Curve   { std::function<double (double)> y; juce::Colour colour; };
        struct Handle  { double x, y; juce::Colour colour; juce::String label; };

        std::function<std::vector<Curve>()> getCurves;
        std::function<std::vector<Handle>()> getHandles;
        std::function<void (int, double, double)> onDrag;
        juce::StringArray axis;
        juce::String title;
        bool showDiagonal = false;

        void paint (juce::Graphics& g) override
        {
            const auto plot = getPlot();

            g.setColour (juce::Colour (dimText));
            g.setFont (uiFont (fonts::small));
            g.drawText (title, getLocalBounds().removeFromTop (headerHeight), juce::Justification::centredLeft);

            g.setColour (juce::Colour (plotColour));
            g.fillRect (plot);

            g.setColour (juce::Colour (gridColour));

            for (int i = 1; i < 4; ++i)
            {
                g.drawVerticalLine ((int) (plot.getX() + plot.getWidth() * (float) i / 4.0f), plot.getY(), plot.getBottom());
                g.drawHorizontalLine ((int) (plot.getY() + plot.getHeight() * (float) i / 4.0f), plot.getX(), plot.getRight());
            }

            if (showDiagonal)
                g.drawLine ({ plot.getBottomLeft(), plot.getTopRight() });

            {
                juce::Graphics::ScopedSaveState clip (g);
                g.reduceClipRegion (plot.toNearestInt());

                if (getCurves)
                {
                    for (const auto& curve : getCurves())
                    {
                        juce::Path path;
                        const int steps = juce::jmax (2, (int) plot.getWidth() / 2);

                        for (int i = 0; i <= steps; ++i)
                        {
                            const auto x = (double) i / steps;
                            const auto px = plot.getX() + (float) x * plot.getWidth();
                            const auto py = plot.getY() + (float) juce::jlimit (-0.2, 1.2, curve.y (x)) * plot.getHeight();

                            if (i == 0)
                                path.startNewSubPath (px, py);
                            else
                                path.lineTo (px, py);
                        }

                        g.setColour (curve.colour);
                        g.strokePath (path, juce::PathStrokeType (1.5f));
                    }
                }
            }

            g.setFont (uiFont (fonts::small));

            if (getHandles)
            {
                for (const auto& handle : getHandles())
                {
                    const auto p = toScreen (handle.x, handle.y);
                    g.setColour (handle.colour);
                    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (p));

                    if (handle.label.isNotEmpty())
                    {
                        auto box = juce::Rectangle<float> (110.0f, 12.0f).withCentre (p.translated (0.0f, p.y < plot.getY() + 20.0f ? 13.0f : -13.0f));
                        box.setX (juce::jlimit (plot.getX(), juce::jmax (plot.getX(), plot.getRight() - box.getWidth()), box.getX()));
                        g.drawText (handle.label, box, juce::Justification::centred);
                    }
                }
            }

            g.setColour (juce::Colour (dimText));

            for (int i = 0; i < axis.size(); ++i)
            {
                const auto cx = plot.getX() + plot.getWidth() * (float) i / (float) juce::jmax (1, axis.size() - 1);
                auto box = juce::Rectangle<float> (50.0f, (float) axisHeight).withCentre ({ cx, plot.getBottom() + (float) axisHeight / 2.0f });
                box.setX (juce::jlimit (0.0f, (float) getWidth() - box.getWidth(), box.getX()));
                const auto justify = i == 0 ? juce::Justification::centredLeft
                                            : i == axis.size() - 1 ? juce::Justification::centredRight
                                                                   : juce::Justification::centred;
                g.drawText (axis[i], box, justify);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            active = -1;

            if (! getHandles)
                return;

            auto best = 14.0f;
            const auto handles = getHandles();

            for (int i = 0; i < (int) handles.size(); ++i)
            {
                const auto d = toScreen (handles[(size_t) i].x, handles[(size_t) i].y).getDistanceFrom (e.position);

                if (d < best)
                {
                    best = d;
                    active = i;
                }
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (active < 0 || ! onDrag)
                return;

            const auto plot = getPlot();
            onDrag (active,
                    juce::jlimit (0.0, 1.0, (double) ((e.position.x - plot.getX()) / plot.getWidth())),
                    juce::jlimit (0.0, 1.0, (double) ((e.position.y - plot.getY()) / plot.getHeight())));
        }

        void mouseUp (const juce::MouseEvent&) override  { active = -1; }

    private:
        static constexpr int headerHeight = 13, axisHeight = 12;

        juce::Rectangle<float> getPlot() const
        {
            return getLocalBounds().withTrimmedTop (headerHeight).withTrimmedBottom (axisHeight)
                                   .reduced (1, 0).toFloat();
        }

        juce::Point<float> toScreen (double x, double y) const
        {
            const auto plot = getPlot();
            return { plot.getX() + (float) x * plot.getWidth(), plot.getY() + (float) y * plot.getHeight() };
        }

        int active = -1;
    };

    //==========================================================================
    // A knob over one number in the patch, live as it turns: re-rendering a
    // kick is cheap, and hearing the change while dragging is the point.
    class PatchKnob : public Knob
    {
    public:
        PatchKnob (const juce::String& knobName, double minimum, double maximum, double interval,
                   const juce::String& suffix, double defaultValue,
                   std::function<double()> getter, std::function<void (double)> setter)
            : Knob (knobName, KnobSize::small), get (std::move (getter)), set (std::move (setter))
        {
            dial.setRange (minimum, maximum, interval);
            dial.setTextValueSuffix (suffix);
            dial.setDoubleClickReturnValue (true, defaultValue);
            dial.setBipolar (minimum < 0.0 && maximum > 0.0);
            dial.onDragEnd = [this] { updateLabel(); };
            dial.onValueChange = [this]
            {
                if (isRefreshing)
                    return;

                set (dial.getValue());
                updateLabel();
            };

            refresh();
        }

        void refresh() override
        {
            if (dial.isMouseButtonDown())
                return;

            const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
            dial.setValue (get(), juce::dontSendNotification);

            if (dial.isMouseOver())
                updateLabel();
        }

    protected:
        juce::String getValueText() override  { return dial.getTextFromValue (dial.getValue()); }

    private:
        std::function<double()> get;
        std::function<void (double)> set;
        bool isRefreshing = false;
    };

    std::unique_ptr<KnobSection> makeSection()
    {
        auto section = std::make_unique<KnobSection> (juce::String());
        section->setCentred (true);
        section->setDrawsBackground (false);
        return section;
    }

    //==========================================================================
    // One drive stage: EQ, then shaper.
    class StagePanel : public juce::Component
    {
    public:
        explicit StagePanel (GabberKickEditor& editorToUse) : editor (editorToUse)
        {
            enabled.setClickingTogglesState (true);
            enabled.onClick = [this]
            {
                const auto on = enabled.getToggleState();
                change ([on] (Stage& s) { s.enabled = on; });
            };

            up.onClick = [this] { swapWith (index - 1); };
            down.onClick = [this] { swapWith (index + 1); };
            up.setTooltip ("Move this stage earlier in the chain");
            down.setTooltip ("Move this stage later in the chain");
            duplicate.onClick = [this]
            {
                const auto i = index;
                editor.edit ([i] (Patch& p)
                {
                    if (i < (int) p.stages.size())
                        p.stages.insert (p.stages.begin() + i + 1, Stage (p.stages[(size_t) i]));
                });
                editor.selectStage (i + 1);
            };
            remove.onClick = [this]
            {
                const auto i = index;
                editor.edit ([i] (Patch& p)
                {
                    if (i < (int) p.stages.size())
                        p.stages.erase (p.stages.begin() + i);
                });
                editor.selectStage (i);
            };

            for (auto* button : { &enabled, &up, &down, &duplicate, &remove })
                addAndMakeVisible (*button);

            shape.setJustificationType (juce::Justification::centredLeft);

            for (int i = 0; i < gabber::numShapes; ++i)
                shape.addItem (gabber::getShapeName ((Shape) i), i + 1);

            shape.onChange = [this]
            {
                if (isRefreshing)
                    return;

                const auto picked = (Shape) (shape.getSelectedId() - 1);
                change ([picked] (Stage& s) { s.shape = picked; s.points = gabber::getShapePoints (picked); });
            };
            addAndMakeVisible (shape);

            buildEq();
            buildShaper();
            refresh();
        }

        void setIndex (int newIndex)  { index = newIndex; refresh(); }

        void refresh()
        {
            const auto stage = read();
            const juce::ScopedValueSetter<bool> svs (isRefreshing, true);

            enabled.setToggleState (stage.enabled, juce::dontSendNotification);
            enabled.setButtonText (stage.enabled ? "On" : "Off");
            shape.setSelectedId ((int) stage.shape + 1, juce::dontSendNotification);
            up.setEnabled (index > 0);
            down.setEnabled (index < editor.getStageCount() - 1);
            eqKnobs->refresh();
            shaperKnobs->refresh();
            eq.repaint();
            curve.repaint();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            if (! read().enabled)
                g.fillAll (juce::Colour (0x66232327));
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8, 3);
            auto head = area.removeFromTop (20);

            enabled.setBounds (head.removeFromLeft (38));
            head.removeFromLeft (4);
            up.setBounds (head.removeFromLeft (26));
            head.removeFromLeft (2);
            down.setBounds (head.removeFromLeft (26));
            head.removeFromLeft (2);
            duplicate.setBounds (head.removeFromLeft (70));
            head.removeFromLeft (2);
            remove.setBounds (head.removeFromLeft (56));
            shape.setBounds (head.removeFromRight (120));
            area.removeFromTop (2);

            auto left = area.removeFromLeft ((area.getWidth() - gap) / 2);
            area.removeFromLeft (gap);
            eqKnobs->setBounds (left.removeFromRight (52));
            eq.setBounds (left);
            shaperKnobs->setBounds (area.removeFromRight (136));
            curve.setBounds (area);
        }

    private:
        Stage read() const
        {
            const auto patch = editor.readPatch();
            return index < (int) patch.stages.size() ? patch.stages[(size_t) index] : Stage();
        }

        void change (std::function<void (Stage&)> apply)
        {
            const auto i = index;
            editor.edit ([i, apply = std::move (apply)] (Patch& p)
            {
                if (i < (int) p.stages.size())
                    apply (p.stages[(size_t) i]);
            });
        }

        void swapWith (int other)
        {
            const auto i = index;
            editor.edit ([i, other] (Patch& p)
            {
                if (other >= 0 && other < (int) p.stages.size() && i < (int) p.stages.size())
                    std::swap (p.stages[(size_t) i], p.stages[(size_t) other]);
            });
            editor.selectStage (other);
        }

        static double toPlotX (double hz)  { return std::log (hz / 20.0) / std::log (900.0); }
        static double toHz (double x)      { return 20.0 * std::pow (900.0, x); }

        void buildEq()
        {
            eq.title = "DYNAMIC EQ  start / end";
            eq.axis = { "20", "110", "600", "3.3k", "18k" };
            eq.getCurves = [this]
            {
                const auto s = read();

                auto response = [q = s.q] (double hz, double gain)
                {
                    return std::function<double (double)> ([=] (double x)
                    {
                        return 0.5 - gabber::getEqResponseDb (toHz (x), hz, gain, q) / 48.0;
                    });
                };

                return std::vector<Graph::Curve> { { response (s.startFreq, s.startGain), juce::Colour (startColour) },
                                                   { response (s.endFreq, s.endGain), juce::Colour (endColour) } };
            };
            eq.getHandles = [this]
            {
                const auto s = read();
                auto label = [] (double hz, double db)
                {
                    return juce::String (juce::roundToInt (hz)) + " Hz / " + juce::String (db, 1) + " dB";
                };

                return std::vector<Graph::Handle>
                {
                    { toPlotX (s.startFreq), 0.5 - s.startGain / 48.0, juce::Colour (startColour), label (s.startFreq, s.startGain) },
                    { toPlotX (s.endFreq), 0.5 - s.endGain / 48.0, juce::Colour (endColour), label (s.endFreq, s.endGain) }
                };
            };
            eq.onDrag = [this] (int handle, double x, double y)
            {
                change ([=] (Stage& s)
                {
                    (handle == 0 ? s.startFreq : s.endFreq) = toHz (x);
                    (handle == 0 ? s.startGain : s.endGain) = (0.5 - y) * 48.0;
                });
            };
            addAndMakeVisible (eq);

            eqKnobs = editor.makeSection();
            eqKnobs->addKnob (std::make_unique<PatchKnob> ("Q", 0.2, 10.0, 0.1, "", 1.0,
                                                           [this] { return read().q; },
                                                           [this] (double v) { change ([v] (Stage& s) { s.q = v; }); }));
            addAndMakeVisible (*eqKnobs);
        }

        void buildShaper()
        {
            curve.title = "FREE CURVE  input -> output";
            curve.axis = { "-1", "0", "+1" };
            curve.showDiagonal = true;
            curve.getCurves = [this]
            {
                const auto points = read().points;

                return std::vector<Graph::Curve>
                {
                    { [points] (double x) { return (1.0 - gabber::evaluateCurve (points, 2.0 * x - 1.0)) / 2.0; },
                      juce::Colour (startColour) }
                };
            };
            curve.getHandles = [this]
            {
                std::vector<Graph::Handle> handles;

                for (const auto& point : read().points)
                    handles.push_back ({ (point.x + 1.0) / 2.0, (1.0 - point.y) / 2.0, juce::Colour (startColour), {} });

                return handles;
            };
            curve.onDrag = [this] (int handle, double x, double y)
            {
                change ([=] (Stage& s)
                {
                    if (handle >= (int) s.points.size())
                        return;

                    auto& point = s.points[(size_t) handle];
                    point.y = 1.0 - 2.0 * y;

                    // The two end points pin the curve's span: they move up
                    // and down only, and the rest stay in order between them.
                    if (handle > 0 && handle < (int) s.points.size() - 1)
                        point.x = juce::jlimit (s.points[(size_t) handle - 1].x + 0.025,
                                                s.points[(size_t) handle + 1].x - 0.025, 2.0 * x - 1.0);
                });
            };
            addAndMakeVisible (curve);

            shaperKnobs = editor.makeSection();

            auto knob = [this] (const char* name, double lo, double hi, double step, const char* suffix, double def,
                                double Stage::* field, double scale)
            {
                shaperKnobs->addKnob (std::make_unique<PatchKnob> (name, lo, hi, step, suffix, def,
                    [this, field, scale] { return read().*field * scale; },
                    [this, field, scale] (double v) { change ([=] (Stage& s) { s.*field = v / scale; }); }));
            };

            knob ("Drive", -12.0, 36.0, 0.5, " dB", 12.0, &Stage::drive, 1.0);
            knob ("Mix", 0.0, 100.0, 1.0, " %", 85.0, &Stage::mix, 100.0);
            knob ("Out", -24.0, 12.0, 0.5, " dB", -3.0, &Stage::output, 1.0);
            addAndMakeVisible (*shaperKnobs);
        }

        GabberKickEditor& editor;
        int index = 0;
        juce::TextButton enabled { "On" }, up { "<" }, down { ">" }, duplicate { "Duplicate" }, remove { "Delete" };
        juce::ComboBox shape;
        Graph eq, curve;
        std::unique_ptr<KnobSection> eqKnobs, shaperKnobs;
        bool isRefreshing = false;
    };

    //==========================================================================
    // One tab per stage, in chain order, and a + at the end. A bypassed
    // stage's tab is dimmed, so the chain can be read without visiting it.
    class StageTabs : public juce::Component,
                      public juce::SettableTooltipClient
    {
    public:
        explicit StageTabs (GabberKickEditor& editorToUse) : editor (editorToUse)
        {
            setRepaintsOnMouseActivity (true);
            setTooltip ("Click to edit a stage, drag to change its place in the chain");
        }

        void paint (juce::Graphics& g) override
        {
            const auto stages = editor.readPatch().stages;
            const auto count = (int) stages.size();
            const auto hover = isMouseOver() ? tabAt (getMouseXYRelative().x, count) : none;
            const auto tabWidth = getTabWidth (count);

            g.setFont (uiFont (fonts::small + 1.0f, juce::Font::bold));

            auto drawTab = [&] (int tab, juce::Rectangle<int> area, const juce::String& text, bool on)
            {
                const auto selected = tab == editor.selectedStage;
                juce::Path shape;
                shape.addRoundedRectangle ((float) area.getX(), (float) area.getY(), (float) area.getWidth(),
                                           (float) area.getHeight(), 4.0f, 4.0f, true, true, false, false);

                g.setColour (selected ? juce::Colour (startColour).withAlpha (on ? 1.0f : 0.55f)
                                      : juce::Colour (tab == hover ? 0xff555562 : 0xff414149));
                g.fillPath (shape);
                g.setColour (selected ? juce::Colour (0xff232327)
                                      : juce::Colour (0xfff0f0f4).withAlpha (on ? 1.0f : 0.45f));
                g.drawText (text, area, juce::Justification::centred);
            };

            auto labelFor = [tabWidth] (int i)
            {
                return (tabWidth >= minWidthForWord ? "Stage " : "") + juce::String (i + 1);
            };

            for (int i = 0; i < count; ++i)
                if (! (dragActive && i == dragged))
                    drawTab (i, getTabArea (i, tabWidth), labelFor (i), stages[(size_t) i].enabled);

            // The "+" is the way to a second stage, so it reads as a button.
            const auto plusArea = getTabArea (count, tabWidth).withWidth (plusWidth).reduced (0, 2).translated (0, 2);
            g.setColour (juce::Colour (startColour).withAlpha (hover == plus ? 1.0f : 0.75f));
            g.drawRoundedRectangle (plusArea.toFloat().reduced (0.5f), 4.0f, 1.0f);
            g.setFont (uiFont (fonts::small + 4.0f, juce::Font::bold));
            g.drawText ("+", plusArea, juce::Justification::centred);
            g.setFont (uiFont (fonts::small + 1.0f, juce::Font::bold));

            // The tab in hand, over the others, under the mouse.
            if (dragActive && juce::isPositiveAndBelow (dragged, count))
                drawTab (dragged,
                         getTabArea (dragged, tabWidth)
                             .withX (juce::jlimit (0, (count - 1) * (tabWidth + tabGap), dragX)),
                         labelFor (dragged), stages[(size_t) dragged].enabled);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const auto count = editor.getStageCount();
            const auto tab = tabAt (e.x, count);

            dragged = none;
            dragActive = false;

            if (tab == plus)
            {
                editor.edit ([] (Patch& p) { p.stages.push_back (gabber::makeStage()); });
                editor.selectStage (count);
            }
            else if (tab != none)
            {
                editor.selectStage (tab);
                dragged = tab;
                grabOffset = e.x - getTabArea (tab, getTabWidth (count)).getX();
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragged == none || (! dragActive && std::abs (e.getDistanceFromDragStartX()) < dragThreshold))
                return;

            dragActive = true;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            dragX = e.x - grabOffset;

            const auto count = editor.getStageCount();
            const auto step = getTabWidth (count) + tabGap;
            const auto target = juce::jlimit (0, juce::jmax (0, count - 1), (dragX + step / 2) / step);

            if (target != dragged)
            {
                editor.moveStage (dragged, target);
                dragged = target;
            }

            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            dragged = none;
            dragActive = false;
            setMouseCursor (juce::MouseCursor::NormalCursor);
            repaint();
        }

    private:
        static constexpr int none = -2, plus = -1, tabGap = 3, plusWidth = 32, maxTabWidth = 84,
                             minWidthForWord = 62, dragThreshold = 4;

        int getTabWidth (int count) const
        {
            return count == 0 ? maxTabWidth
                              : juce::jlimit (16, maxTabWidth, (getWidth() - plusWidth) / count - tabGap);
        }

        juce::Rectangle<int> getTabArea (int i, int tabWidth) const
        {
            return { i * (tabWidth + tabGap), 0, tabWidth, getHeight() };
        }

        int tabAt (int x, int count) const
        {
            const auto tabWidth = getTabWidth (count);

            for (int i = 0; i < count; ++i)
                if (getTabArea (i, tabWidth).getHorizontalRange().contains (x))
                    return i;

            return getTabArea (count, tabWidth).withWidth (plusWidth).getHorizontalRange().contains (x) ? plus : none;
        }

        GabberKickEditor& editor;
        int dragged = none, grabOffset = 0, dragX = 0;
        bool dragActive = false;
    };

    //==========================================================================
    class Waveform : public juce::Component
    {
    public:
        explicit Waveform (GabberKickEditor& editorToUse) : editor (editorToUse) {}

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (plotColour));

            const auto kick = editor.getKick() != nullptr ? editor.getKick()->getRenderedKick() : nullptr;

            if (kick == nullptr || kick->samples.empty())
                return;

            const auto middle = (float) getHeight() * 0.5f;
            const auto scale = (float) getHeight() * 0.48f;
            const auto size = kick->samples.size();

            g.setColour (juce::Colour (startColour));

            for (int x = 0; x < getWidth(); ++x)
            {
                const auto from = (size_t) x * size / (size_t) getWidth();
                const auto to = juce::jmax (from + 1, (size_t) (x + 1) * size / (size_t) getWidth());

                auto lo = 0.0f, hi = 0.0f;

                for (auto i = from; i < to && i < size; ++i)
                {
                    lo = juce::jmin (lo, kick->samples[i]);
                    hi = juce::jmax (hi, kick->samples[i]);
                }

                g.drawVerticalLine (x, middle - hi * scale, juce::jmax (middle - hi * scale + 1.0f, middle - lo * scale));
            }
        }

    private:
        GabberKickEditor& editor;
    };

    //==========================================================================
    plugins::GabberKickPlugin* getKick() const  { return dynamic_cast<plugins::GabberKickPlugin*> (plugin.get()); }

    Patch readPatch() const
    {
        if (auto* k = getKick())
            return k->getPatch();

        return {};
    }

    int getStageCount() const  { return (int) readPatch().stages.size(); }

    void edit (const std::function<void (Patch&)>& apply)
    {
        if (auto* k = getKick())
        {
            auto patch = k->getPatch();
            apply (patch);
            k->setPatch (patch);
        }
    }

    void buildSource()
    {
        pitchGraph = std::make_unique<Graph>();
        pitchGraph->title = "PITCH  Hz / ms";
        pitchGraph->axis = { "0", "30", "60", "90", "120 ms" };
        pitchGraph->getCurves = [this]
        {
            const auto p = readPatch();

            return std::vector<Graph::Curve>
            {
                { [p] (double x) { return 1.0 - (p.base + p.sweep * std::exp (-x * 0.12 / p.pitchDecay)) / 800.0; },
                  juce::Colour (startColour) }
            };
        };
        pitchGraph->getHandles = [this]
        {
            const auto p = readPatch();
            const juce::Colour c (startColour);

            return std::vector<Graph::Handle>
            {
                { 0.0, 1.0 - (p.base + p.sweep) / 800.0, c, juce::String (juce::roundToInt (p.base + p.sweep)) + " Hz" },
                { p.pitchDecay / 0.12, 1.0 - (p.base + p.sweep / std::exp (1.0)) / 800.0, c,
                  juce::String (juce::roundToInt (p.pitchDecay * 1000.0)) + " ms" },
                { 1.0, 1.0 - p.base / 800.0, c, juce::String (juce::roundToInt (p.base)) + " Hz" }
            };
        };
        pitchGraph->onDrag = [this] (int handle, double x, double y)
        {
            edit ([=] (Patch& p)
            {
                const auto hz = (1.0 - y) * 800.0;

                if (handle == 0)      p.sweep = juce::jlimit (0.0, 700.0, hz - p.base);
                else if (handle == 1) p.pitchDecay = juce::jlimit (0.002, 0.1, x * 0.12);
                else                  p.base = juce::jlimit (25.0, 120.0, hz);
            });
        };
        addAndMakeVisible (*pitchGraph);

        ampGraph = std::make_unique<Graph>();
        ampGraph->title = "AMPLITUDE  attack / length";
        ampGraph->axis = { "0", "75", "300", "675", "1200 ms" };
        ampGraph->getCurves = [this]
        {
            const auto p = readPatch();

            return std::vector<Graph::Curve>
            {
                { [p] (double x)
                  {
                      const auto t = x * x * 1.2;
                      return t > p.duration ? 1.0
                                            : 1.0 - std::sin (juce::jmin (1.0, t / juce::jmax (0.00001, p.attack)) * juce::MathConstants<double>::halfPi)
                                                      * std::exp (-5.0 * t / p.duration);
                  },
                  juce::Colour (startColour) }
            };
        };
        ampGraph->getHandles = [this]
        {
            const auto p = readPatch();
            const juce::Colour c (startColour);

            return std::vector<Graph::Handle>
            {
                { std::sqrt (p.attack / 1.2), 0.0, c, juce::String (p.attack * 1000.0, 1) + " ms" },
                { std::sqrt (p.duration / 1.2), 1.0, c, juce::String (juce::roundToInt (p.duration * 1000.0)) + " ms" }
            };
        };
        ampGraph->onDrag = [this] (int handle, double x, double)
        {
            edit ([=] (Patch& p)
            {
                if (handle == 0)
                    p.attack = juce::jlimit (0.0, 0.01, x * x * 1.2);
                else
                    p.duration = juce::jlimit (0.2, 1.2, x * x * 1.2);
            });
        };
        addAndMakeVisible (*ampGraph);

        sourceKnobs = makeSection();

        auto knob = [this] (const char* name, double lo, double hi, double step, const char* suffix, double def, double Patch::* field)
        {
            sourceKnobs->addKnob (std::make_unique<PatchKnob> (name, lo, hi, step, suffix, def,
                [this, field] { return readPatch().*field; },
                [this, field] (double v) { edit ([=] (Patch& p) { p.*field = v; }); }));
        };

        knob ("Tune", 25.0, 120.0, 0.5, " Hz", 55.0, &Patch::base);
        knob ("Click", 0.0, 1.0, 0.01, "", 0.22, &Patch::click);
        knob ("Noise", 0.0, 1.0, 0.01, "", 0.12, &Patch::noise);
        addAndMakeVisible (*sourceKnobs);
    }

    void buildOutput()
    {
        waveform = std::make_unique<Waveform> (*this);
        addAndMakeVisible (*waveform);

        outputKnobs = makeSection();
        outputKnobs->addKnob (std::make_unique<PatchKnob> ("Output", -24.0, 12.0, 0.5, " dB", 0.0,
            [this] { return readPatch().gain; },
            [this] (double v) { edit ([v] (Patch& p) { p.gain = v; }); }));
        addAndMakeVisible (*outputKnobs);

        normalize.setTooltip ("Normalize the kick's peak to -1 dBFS");
        normalize.onClick = [this]
        {
            const auto on = normalize.getToggleState();
            edit ([on] (Patch& p) { p.normalize = on; });
        };
        addAndMakeVisible (normalize);

        exportButton.onClick = [this] { exportWav(); };
        addAndMakeVisible (exportButton);
    }

    void exportWav()
    {
        auto* k = getKick();

        if (k == nullptr)
            return;

        const auto kick = k->getRenderedKick();

        if (kick == nullptr || kick->samples.empty())
            return;

        chooser = std::make_unique<juce::FileChooser> ("Export kick", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                                          .getChildFile ("gabber-kick.wav"), "*.wav");

        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [kick] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();

            if (file == juce::File())
                return;

            file.deleteFile();
            auto stream = file.createOutputStream();

            if (stream == nullptr)
                return;

            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (stream.get(), kick->sampleRate, 1, 16, {}, 0));

            if (writer == nullptr)
                return;

            stream.release();   // the writer owns it now

            const float* channels[] { kick->samples.data() };
            writer->writeFromFloatArrays (channels, 1, (int) kick->samples.size());
        });
    }

    void moveStage (int from, int to)
    {
        edit ([from, to] (Patch& p)
        {
            const auto size = (int) p.stages.size();

            if (from == to || ! juce::isPositiveAndBelow (from, size) || ! juce::isPositiveAndBelow (to, size))
                return;

            auto stage = std::move (p.stages[(size_t) from]);
            p.stages.erase (p.stages.begin() + from);
            p.stages.insert (p.stages.begin() + to, std::move (stage));
        });

        selectStage (to);
    }

    void selectStage (int index)
    {
        const auto count = getStageCount();

        selectedStage = juce::jlimit (0, juce::jmax (0, count - 1), index);
        stagePanel->setVisible (count > 0);
        noStages.setVisible (count == 0);
        stagePanel->setIndex (selectedStage);
        stageTabs->repaint();
    }

    void timerCallback() override
    {
        auto* k = getKick();

        if (k == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        if (const auto revision = k->getPatchRevision(); revision != seenRevision)
        {
            seenRevision = revision;

            // Also what catches a preset landing with fewer stages than the
            // selected tab.
            selectStage (selectedStage);

            sourceKnobs->refresh();
            outputKnobs->refresh();
            pitchGraph->repaint();
            ampGraph->repaint();
            normalize.setToggleState (readPatch().normalize, juce::dontSendNotification);
        }

        if (const auto kick = k->getRenderedKick(); kick != seenKick)
        {
            seenKick = kick;
            waveform->repaint();

            if (kick != nullptr)
            {
                status.setText (kick->limited ? "PEAK PROTECTED - GAIN REDUCED"
                                              : "Peak " + juce::String (juce::Decibels::gainToDecibels (kick->peak), 1) + " dBFS",
                                juce::dontSendNotification);
            }
        }
    }

    te::SafeSelectable<te::Plugin> plugin;
    PresetBar presetBar;
    juce::TextButton hit { "HIT" }, exportButton { "Export WAV" };
    juce::ToggleButton normalize { "Normalize" };
    juce::Label status, noStages;
    std::unique_ptr<Graph> pitchGraph, ampGraph;
    std::unique_ptr<KnobSection> sourceKnobs, outputKnobs;
    std::unique_ptr<Waveform> waveform;
    std::unique_ptr<StageTabs> stageTabs;
    std::unique_ptr<StagePanel> stagePanel;
    int selectedStage = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> sourceArea, outputArea, stageArea, stagesLabelArea;
    std::uint64_t seenRevision = 0;
    std::shared_ptr<const plugins::GabberKickPlugin::RenderedKick> seenKick;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GabberKickEditor)
};

} // namespace carve::app
