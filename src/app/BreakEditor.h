#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "RecentMediaList.h"
#include "plugins/BreakAnalysis.h"
#include "plugins/BreakPlugin.h"

namespace carve::app
{

// The loop's waveform with its cuts over it, for the break slicer's Inst tab.
//
// The cuts are the model's: this view is handed them on every refresh and
// reports gestures, and the edit comes back round through the model and the
// sync. The one thing it holds of its own is a cut while it is being dragged,
// drawn where the mouse has it and handed over on release -- so a drag is one
// undo step, and the sync is not asked to re-cut the loop sixty times a
// second.
class BreakSliceView : public juce::Component,
                       private juce::ScrollBar::Listener
{
public:
    BreakSliceView()
    {
        scrollBar.setAutoHide (false);
        scrollBar.addListener (this);
        addAndMakeVisible (scrollBar);
    }

    ~BreakSliceView() override
    {
        scrollBar.removeListener (this);
    }

    // The view: which stretch of the loop is on screen. Cmd (or Ctrl) and
    // the wheel, or a pinch, zoom about the pointer; the wheel alone scrolls;
    // the scrollbar under the waveform does both the usual way.
    void zoomBy (double factor)  { zoomAbout (factor, getWidth() * 0.5f); }
    void zoomToFit()             { setView (0.0, getLengthSeconds()); }
    bool isZoomedIn() const      { return getLengthSeconds() > 0.0 && visibleLength() < getLengthSeconds() - 1.0e-9; }

    std::function<void (int index, double seconds)> onCutMoved;
    std::function<void (double seconds)> onCutAdded;
    std::function<void (int index)> onCutRemoved;
    std::function<void (int slice)> onSliceClicked;
    std::function<void()> onViewChanged;

    void setAudio (std::shared_ptr<const plugins::BreakPlugin::Audio> newAudio)
    {
        if (newAudio == audio)
            return;

        audio = std::move (newAudio);
        peaksWidth = -1;
        setView (0.0, getLengthSeconds());
    }

    void setCuts (std::vector<double> newCuts)
    {
        if (newCuts == cuts)
            return;

        // A cut added or removed renumbers the slices; the selection was a
        // number, so it would now light some other slice.
        if (newCuts.size() != cuts.size())
            selectedSlice = -1;

        cuts = std::move (newCuts);
        repaint();
    }

    const std::vector<double>& getCuts() const  { return cuts; }

    void setPlaying (int slice, double seconds)
    {
        if (slice == playingSlice && std::abs (seconds - playingSeconds) < 1.0e-4)
            return;

        playingSlice = slice;
        playingSeconds = seconds;
        repaint();
    }

    double getLengthSeconds() const  { return audio != nullptr ? audio->getLengthSeconds() : 0.0; }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = contentArea();
        g.setColour (juce::Colour (0xff1c1c20));
        g.fillRect (getLocalBounds());

        if (audio == nullptr || getLengthSeconds() <= 0.0)
            return;

        g.reduceClipRegion (bounds);

        const auto shown = shownCuts();
        const auto waveArea = bounds.withTrimmedTop (numberRowHeight);

        // The slices, alternately shaded so each reads as one block, the
        // sounding one lit.
        for (int i = 0; i < (int) shown.size(); ++i)
        {
            const auto x0 = xFor (shown[(size_t) i]);
            const auto x1 = i + 1 < (int) shown.size() ? xFor (shown[(size_t) i + 1]) : xFor (getLengthSeconds());

            if (x1 < 0.0f || x0 > (float) getWidth())
                continue;

            auto block = juce::Rectangle<float> (x0, (float) waveArea.getY(), x1 - x0, (float) waveArea.getHeight());

            if (i == playingSlice)
                g.setColour (juce::Colour (0xff3a3020));
            else if (i == selectedSlice)
                g.setColour (juce::Colour (0xff2a3240));
            else
                g.setColour (i % 2 == 0 ? juce::Colour (0xff222228) : juce::Colour (0xff1c1c20));

            g.fillRect (block);

            g.setColour (juce::Colour (i == playingSlice ? 0xffffc070 : 0xff8a8a94));
            g.setFont (juce::FontOptions (11.0f));
            // Kept on screen while the slice's start is scrolled off it.
            const auto labelX = juce::jmax (x0, 0.0f);
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (labelX + 3.0f, 0.0f, juce::jmax (0.0f, x1 - labelX - 4.0f),
                                                                    (float) numberRowHeight),
                        juce::Justification::centredLeft, false);
        }

        // The audio before the first cut is not part of any slice.
        if (! shown.empty() && shown.front() > 0.0)
        {
            g.setColour (juce::Colour (0xaa101012));
            g.fillRect (juce::Rectangle<float> (0.0f, (float) waveArea.getY(), xFor (shown.front()),
                                                (float) waveArea.getHeight()));
        }

        drawWaveform (g, waveArea);

        // The cuts.
        for (int i = 0; i < (int) shown.size(); ++i)
        {
            const auto x = xFor (shown[(size_t) i]);
            const bool active = i == draggingCut || i == hoverCut;
            g.setColour (active ? juce::Colours::white : juce::Colour (0xffe0a24f));
            g.drawLine (x, 0.0f, x, (float) bounds.getBottom(), active ? 2.0f : 1.0f);

            // A handle at the top, so a cut reads as something to grab.
            juce::Path handle;
            handle.addTriangle (x - 5.0f, (float) numberRowHeight - 8.0f, x + 5.0f, (float) numberRowHeight - 8.0f,
                                x, (float) numberRowHeight);
            g.fillPath (handle);
        }

        if (playingSlice >= 0)
        {
            const auto x = xFor (playingSeconds);
            g.setColour (juce::Colour (0xccffffff));
            g.drawLine (x, (float) waveArea.getY(), x, (float) waveArea.getBottom(), 1.0f);
        }
    }

    void resized() override
    {
        peaksWidth = -1;
        scrollBar.setBounds (getLocalBounds().removeFromBottom (scrollBarHeight));
        updateScrollBar();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (audio == nullptr)
            return;

        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            zoomAbout (std::pow (2.0, wheel.deltaY * (wheel.isReversed ? -2.0 : 2.0)), e.position.x);
            return;
        }

        // Sideways on a trackpad, or the plain wheel turned into sideways.
        const auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
        setView (viewStart - (wheel.isReversed ? -delta : delta) * visibleLength() * 0.5, visibleLength());
    }

    void mouseMagnify (const juce::MouseEvent& e, float scale) override
    {
        zoomAbout (scale, e.position.x);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto cut = cutAt (e.position.x);

        if (cut != hoverCut)
        {
            hoverCut = cut;
            setMouseCursor (cut >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hoverCut >= 0)
        {
            hoverCut = -1;
            repaint();
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (audio == nullptr)
            return;

        const auto cut = cutAt (e.position.x);

        if (e.mods.isPopupMenu())
        {
            if (cut >= 0 && cuts.size() > 1)
            {
                juce::PopupMenu menu;
                menu.addItem (1, "Remove cut " + juce::String (cut + 1));
                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                                                              .withMousePosition(),
                                    [safe = juce::Component::SafePointer<BreakSliceView> (this), cut] (int result)
                {
                    if (result == 1 && safe != nullptr && safe->onCutRemoved)
                        safe->onCutRemoved (cut);
                });
            }

            return;
        }

        if (cut >= 0)
        {
            draggingCut = cut;
            dragSeconds = cuts[(size_t) cut];
            dragMoved = false;
            repaint();
            return;
        }

        if (const auto slice = sliceAt (secondsFor (e.position.x)); slice >= 0)
        {
            selectedSlice = slice;
            repaint();

            if (onSliceClicked)
                onSliceClicked (slice);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingCut < 0)
            return;

        // Kept between its neighbours here as the model will keep it, so
        // what is drawn is where it lands.
        const auto low = draggingCut > 0 ? cuts[(size_t) draggingCut - 1] + minGap : 0.0;
        const auto high = draggingCut + 1 < (int) cuts.size() ? cuts[(size_t) draggingCut + 1] - minGap
                                                              : getLengthSeconds() - minGap;

        dragSeconds = juce::jlimit (low, juce::jmax (low, high), snap (secondsFor (e.position.x), e.mods));
        dragMoved = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (draggingCut < 0)
            return;

        const auto index = draggingCut;
        draggingCut = -1;
        repaint();

        if (dragMoved && onCutMoved)
            onCutMoved (index, dragSeconds);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (audio == nullptr || cutAt (e.position.x) >= 0)
            return;

        if (onCutAdded)
            onCutAdded (snap (secondsFor (e.position.x), e.mods));
    }

private:
    static constexpr int numberRowHeight = 18;
    static constexpr int scrollBarHeight = 10;
    static constexpr double minGap = 0.005;   // Generator::minBreakSliceSeconds

    // As close as the view goes: a few hundred frames across, where single
    // cycles of a hat are wide enough to put a cut between.
    static constexpr double minVisibleSeconds = 0.01;

    juce::Rectangle<int> contentArea() const  { return getLocalBounds().withTrimmedBottom (scrollBarHeight); }

    double visibleLength() const  { return viewLength > 0.0 ? viewLength : getLengthSeconds(); }

    void setView (double start, double length)
    {
        const auto total = getLengthSeconds();
        length = total > 0.0 ? juce::jlimit (juce::jmin (minVisibleSeconds, total), total, length) : 0.0;
        start = juce::jlimit (0.0, juce::jmax (0.0, total - length), start);

        if (std::abs (start - viewStart) < 1.0e-12 && std::abs (length - viewLength) < 1.0e-12)
        {
            updateScrollBar();
            return;
        }

        viewStart = start;
        viewLength = length;
        updateScrollBar();
        repaint();

        if (onViewChanged)
            onViewChanged();
    }

    // Keeps the moment under the pointer under the pointer.
    void zoomAbout (double factor, float x)
    {
        if (audio == nullptr || factor <= 0.0)
            return;

        const auto anchor = secondsFor (x);
        const auto length = visibleLength() / factor;
        const auto fraction = (double) x / juce::jmax (1, getWidth());
        setView (anchor - fraction * length, length);
    }

    void updateScrollBar()
    {
        scrollBar.setRangeLimits (0.0, juce::jmax (1.0e-6, getLengthSeconds()), juce::dontSendNotification);
        scrollBar.setCurrentRange (viewStart, juce::jmax (1.0e-6, visibleLength()), juce::dontSendNotification);
    }

    void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override
    {
        setView (newRangeStart, visibleLength());
    }

    // The model's cuts, with the one being dragged where the mouse has it.
    std::vector<double> shownCuts() const
    {
        auto shown = cuts;

        if (draggingCut >= 0 && draggingCut < (int) shown.size())
            shown[(size_t) draggingCut] = dragSeconds;

        return shown;
    }

    float xFor (double seconds) const
    {
        const auto length = visibleLength();
        return length > 0.0 ? (float) ((seconds - viewStart) / length * getWidth()) : 0.0f;
    }

    double secondsFor (float x) const
    {
        return juce::jlimit (0.0, getLengthSeconds(),
                             viewStart + (double) x / juce::jmax (1, getWidth()) * visibleLength());
    }

    // To the nearest zero crossing, unless Alt is held.
    double snap (double seconds, const juce::ModifierKeys& mods) const
    {
        if (audio == nullptr || mods.isAltDown())
            return seconds;

        return plugins::breakanalysis::snapToZeroCrossing (audio->buffer, audio->sampleRate, seconds);
    }

    int cutAt (float x) const
    {
        int best = -1;
        float bestDistance = 5.0f;

        for (int i = 0; i < (int) cuts.size(); ++i)
            if (const auto distance = std::abs (xFor (cuts[(size_t) i]) - x); distance <= bestDistance)
            {
                best = i;
                bestDistance = distance;
            }

        return best;
    }

    int sliceAt (double seconds) const
    {
        int slice = -1;

        for (int i = 0; i < (int) cuts.size(); ++i)
            if (seconds >= cuts[(size_t) i])
                slice = i;

        return slice;
    }

    void drawWaveform (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const int width = area.getWidth();

        if (width <= 0)
            return;

        // The peaks per column, computed again only when the width, the view
        // or the loop changes: a few hundred columns of a few frames each.
        if (peaksWidth != width || std::abs (peaksStart - viewStart) > 1.0e-12
             || std::abs (peaksLength - visibleLength()) > 1.0e-12)
        {
            peaksWidth = width;
            peaksStart = viewStart;
            peaksLength = visibleLength();
            peaks.assign ((size_t) width, { 0.0f, 0.0f });

            const auto& buffer = audio->buffer;
            const int frames = buffer.getNumSamples();
            const double firstFrame = viewStart * audio->sampleRate;
            const double framesPerColumn = visibleLength() * audio->sampleRate / width;

            for (int x = 0; x < width; ++x)
            {
                const int from = juce::jlimit (0, juce::jmax (0, frames - 1), (int) (firstFrame + x * framesPerColumn));
                const int to = juce::jlimit (from + 1, frames, (int) (firstFrame + (x + 1) * framesPerColumn));
                float low = 0.0f, high = 0.0f;

                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                {
                    const auto range = buffer.findMinMax (ch, from, juce::jmin (frames, to) - from);
                    low = juce::jmin (low, range.getStart());
                    high = juce::jmax (high, range.getEnd());
                }

                peaks[(size_t) x] = { low, high };
            }
        }

        const auto centre = (float) area.getCentreY();
        const auto half = (float) area.getHeight() * 0.48f;

        g.setColour (juce::Colour (0xff7fb3d5));

        for (int x = 0; x < width; ++x)
        {
            const auto [low, high] = peaks[(size_t) x];
            g.drawVerticalLine (area.getX() + x, centre - high * half, centre - low * half + 1.0f);
        }
    }

    std::shared_ptr<const plugins::BreakPlugin::Audio> audio;
    std::vector<double> cuts;
    std::vector<std::pair<float, float>> peaks;
    int peaksWidth = -1;
    double peaksStart = 0.0, peaksLength = 0.0;

    double viewStart = 0.0, viewLength = 0.0;
    juce::ScrollBar scrollBar { false };

    int hoverCut = -1, draggingCut = -1, selectedSlice = -1;
    double dragSeconds = 0.0;
    bool dragMoved = false;

    int playingSlice = -1;
    double playingSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreakSliceView)
};

//==============================================================================
// The break slicer's Inst tab: which loop, how many bars it is, whether it
// follows the tempo, and where it is cut.
//
// Like the SoundFont editor, it reports gestures and reads the plugin's state
// from a timer: the plugin is what has the loop's audio and knows which slice
// is sounding. The cuts it draws are the model's, handed in by setModel.
class BreakEditor : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    private juce::Timer
{
public:
    std::function<void()> onLoadBreak;
    std::function<void (const juce::File&)> onBreakDropped;
    std::function<void (const juce::File&)> onRecentChosen;
    std::function<bool (const juce::StringArray&)> isInterestedInAudioFiles;

    // Every cut at once: an even division or what Detect found.
    std::function<void (const std::vector<double>&)> onCutsReplaced;
    std::function<void (int index, double seconds)> onCutMoved;
    std::function<void (double seconds)> onCutAdded;
    std::function<void (int index)> onCutRemoved;
    std::function<void (int bars)> onBarsChanged;
    std::function<void (bool)> onRepitchChanged;
    std::function<void (int note, int velocity)> onPreviewNote;

    void setRecentItems (std::vector<RecentMediaList::Item> items)  { recent.setItems (std::move (items)); }

    explicit BreakEditor (plugins::BreakPlugin& slicer) : plugin (&slicer)
    {
        setSize (760, 340);

        loadButton.onClick = [this] { if (onLoadBreak) onLoadBreak(); };
        addAndMakeVisible (loadButton);

        fileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8de));
        fileLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (fileLabel);

        barsLabel.setText ("Bars", juce::dontSendNotification);
        barsLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
        addAndMakeVisible (barsLabel);

        for (int bars : { 1, 2, 4, 8, 16 })
            barsBox.addItem (juce::String (bars), bars);

        barsBox.onChange = [this]
        {
            if (const int bars = barsBox.getSelectedId(); bars > 0 && bars != shownBars && onBarsChanged)
                onBarsChanged (bars);
        };
        addAndMakeVisible (barsBox);

        bpmLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
        addAndMakeVisible (bpmLabel);

        repitchToggle.setTooltip ("Play the whole loop faster or slower with the song's tempo, pitch and all. "
                                  "Off, each slice plays as recorded and only its start follows the beat.");
        repitchToggle.onClick = [this]
        {
            if (onRepitchChanged)
                onRepitchChanged (repitchToggle.getToggleState());
        };
        addAndMakeVisible (repitchToggle);

        slicesLabel.setText ("Slice", juce::dontSendNotification);
        slicesLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
        addAndMakeVisible (slicesLabel);

        for (int i = 0; i < (int) divideButtons.size(); ++i)
        {
            const int count = 4 << i;   // 4, 8, 16, 32
            auto& button = divideButtons[(size_t) i];
            button.setButtonText (juce::String (count));
            button.setTooltip ("Cut the loop into " + juce::String (count) + " equal slices");
            button.onClick = [this, count] { divideEvenly (count); };
            addAndMakeVisible (button);
        }

        detectButton.setTooltip ("Cut at the hits: wherever the loop gets suddenly louder");
        detectButton.onClick = [this] { detect(); };
        addAndMakeVisible (detectButton);

        sensitivity.setSliderStyle (juce::Slider::LinearHorizontal);
        sensitivity.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        sensitivity.setRange (0.0, 1.0, 0.01);
        sensitivity.setValue (0.5, juce::dontSendNotification);
        sensitivity.setTooltip ("How quiet a hit Detect still cuts at: left for only the big hits, right for every ghost note");
        sensitivity.setColour (juce::Slider::trackColourId, juce::Colour (0xffe0a24f));
        addAndMakeVisible (sensitivity);

        zoomOutButton.setTooltip ("Zoom out (cmd-scroll or pinch on the waveform)");
        zoomInButton.setTooltip ("Zoom in (cmd-scroll or pinch on the waveform)");
        fitButton.setTooltip ("Show the whole loop");
        zoomOutButton.onClick = [this] { slices.zoomBy (0.5); };
        zoomInButton.onClick = [this] { slices.zoomBy (2.0); };
        fitButton.onClick = [this] { slices.zoomToFit(); };
        zoomOutButton.setConnectedEdges (juce::Button::ConnectedOnRight);
        zoomInButton.setConnectedEdges (juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
        fitButton.setConnectedEdges (juce::Button::ConnectedOnLeft);

        for (auto* button : { &zoomOutButton, &zoomInButton, &fitButton })
            addAndMakeVisible (button);

        slices.onViewChanged = [this] { fitButton.setEnabled (slices.isZoomedIn()); };
        fitButton.setEnabled (false);

        countLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
        countLabel.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (countLabel);

        slices.onCutMoved = [this] (int index, double seconds) { if (onCutMoved) onCutMoved (index, seconds); };
        slices.onCutAdded = [this] (double seconds) { if (onCutAdded) onCutAdded (seconds); };
        slices.onCutRemoved = [this] (int index) { if (onCutRemoved) onCutRemoved (index); };
        slices.onSliceClicked = [this] (int slice)
        {
            if (onPreviewNote)
                onPreviewNote (plugins::BreakPlugin::firstNote + slice, 100);
        };
        addAndMakeVisible (slices);

        hint.setText ("Click a slice to hear it.  Double-click to cut, drag a cut to move it (Alt: no snap), "
                      "right-click to remove.  Cmd-scroll or pinch to zoom, scroll to move.",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff5e5e68));
        hint.setJustificationType (juce::Justification::centred);
        hint.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (hint);

        recent.onChosen = [this] (const juce::File& file) { if (onRecentChosen) onRecentChosen (file); };
        addChildComponent (recent);

        dropHint.setText ("Load a drum loop, or drop one here", juce::dontSendNotification);
        dropHint.setColour (juce::Label::textColourId, juce::Colour (0xff5e5e68));
        dropHint.setJustificationType (juce::Justification::centred);
        addChildComponent (dropHint);

        refreshFromPlugin();
        startTimerHz (30);
    }

    // The model's half: where it is cut, how many bars, repitched or not, and
    // the bar length to turn bars into a tempo with.
    void setModel (const std::vector<double>& cuts, int bars, bool repitch, double beatsPerBar)
    {
        slices.setCuts (cuts);
        shownBars = bars;
        shownBeatsPerBar = beatsPerBar;

        if (barsBox.indexOfItemId (bars) < 0)
            barsBox.addItem (juce::String (bars), bars);

        barsBox.setSelectedId (bars, juce::dontSendNotification);
        repitchToggle.setToggleState (repitch, juce::dontSendNotification);
        updateLabels();
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
        auto area = getLocalBounds().reduced (12);

        auto top = area.removeFromTop (26);
        loadButton.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (10);
        repitchToggle.setBounds (top.removeFromRight (90));
        top.removeFromRight (8);
        bpmLabel.setBounds (top.removeFromRight (84));
        barsBox.setBounds (top.removeFromRight (56));
        barsLabel.setBounds (top.removeFromRight (36));
        top.removeFromRight (8);
        fileLabel.setBounds (top);

        area.removeFromTop (8);
        auto row = area.removeFromTop (24);
        slicesLabel.setBounds (row.removeFromLeft (40));

        for (auto& button : divideButtons)
        {
            button.setBounds (row.removeFromLeft (40));
            row.removeFromLeft (4);
        }

        row.removeFromLeft (12);
        detectButton.setBounds (row.removeFromLeft (64));
        row.removeFromLeft (6);
        sensitivity.setBounds (row.removeFromLeft (110));
        fitButton.setBounds (row.removeFromRight (40));
        zoomInButton.setBounds (row.removeFromRight (28));
        zoomOutButton.setBounds (row.removeFromRight (28));
        row.removeFromRight (10);
        countLabel.setBounds (row);

        area.removeFromTop (8);
        hint.setBounds (area.removeFromBottom (20));
        area.removeFromBottom (4);
        slices.setBounds (area);

        // In the waveform's place while there is no loop.
        auto empty = area;
        dropHint.setBounds (empty.removeFromBottom (24));
        recent.setBounds (empty.withSizeKeepingCentre (juce::jmin (empty.getWidth(), 360), empty.getHeight()));
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return isInterestedInAudioFiles && isInterestedInAudioFiles (files);
    }

    void fileDragEnter (const juce::StringArray&, int, int) override  { setDragOver (true); }
    void fileDragExit (const juce::StringArray&) override             { setDragOver (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDragOver (false);

        for (const auto& path : files)
            if (isInterestedInAudioFiles && isInterestedInAudioFiles (juce::StringArray (path)))
            {
                if (onBreakDropped)
                    onBreakDropped (juce::File (path));

                return;
            }
    }

private:
    void timerCallback() override  { refreshFromPlugin(); }

    void refreshFromPlugin()
    {
        auto* slicer = dynamic_cast<plugins::BreakPlugin*> (plugin.get());

        if (slicer == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        auto audio = slicer->getAudio();
        const auto error = slicer->getLoadError();

        if (! shownOnce || audio != shownAudio || error != shownError)
        {
            shownOnce = true;
            shownAudio = audio;
            shownError = error;
            slices.setAudio (audio);

            const bool empty = audio == nullptr;
            slices.setVisible (! empty);
            recent.setVisible (empty);
            dropHint.setVisible (empty);

            for (auto& button : divideButtons)
                button.setEnabled (! empty);

            detectButton.setEnabled (! empty);
            sensitivity.setEnabled (! empty);
            zoomOutButton.setEnabled (! empty);
            zoomInButton.setEnabled (! empty);
            fitButton.setEnabled (slices.isZoomedIn());
            updateLabels();
        }

        slices.setPlaying (slicer->getPlayingSlice(), slicer->getPlayingSeconds());
    }

    void updateLabels()
    {
        const auto length = shownAudio != nullptr ? shownAudio->getLengthSeconds() : 0.0;

        if (shownError.isNotEmpty())
            fileLabel.setText (shownError, juce::dontSendNotification);
        else if (shownAudio == nullptr)
            fileLabel.setText ("No loop loaded", juce::dontSendNotification);
        else
            fileLabel.setText (shownAudio->file.getFileName() + "   (" + juce::String (length, 2) + " s)",
                               juce::dontSendNotification);

        fileLabel.setColour (juce::Label::textColourId,
                             juce::Colour (shownError.isNotEmpty() ? 0xffe0a060 : 0xffd8d8de));

        bpmLabel.setText (length > 0.0 ? juce::String (shownBars * shownBeatsPerBar * 60.0 / length, 1) + " BPM"
                                       : juce::String(),
                          juce::dontSendNotification);

        const auto count = (int) slices.getCuts().size();
        countLabel.setText (shownAudio != nullptr ? juce::String (count) + (count == 1 ? " slice" : " slices")
                                                  : juce::String(),
                            juce::dontSendNotification);
    }

    void divideEvenly (int count)
    {
        const auto length = slices.getLengthSeconds();

        if (length <= 0.0 || ! onCutsReplaced)
            return;

        std::vector<double> cuts;

        for (int i = 0; i < count; ++i)
            cuts.push_back (length * i / count);

        onCutsReplaced (cuts);
    }

    void detect()
    {
        if (shownAudio == nullptr || ! onCutsReplaced)
            return;

        onCutsReplaced (plugins::breakanalysis::detectTransients (shownAudio->buffer, shownAudio->sampleRate,
                                                                  sensitivity.getValue(), 64));
    }

    void setDragOver (bool over)
    {
        if (dragOver != over)
        {
            dragOver = over;
            repaint();
        }
    }

    // Gone with its track, which an undo of the generator's creation does.
    te::SafeSelectable<te::Plugin> plugin;

    bool shownOnce = false;
    std::shared_ptr<const plugins::BreakPlugin::Audio> shownAudio;
    juce::String shownError;
    int shownBars = 4;
    double shownBeatsPerBar = 4.0;
    bool dragOver = false;

    juce::TextButton loadButton { "Load Loop..." };
    juce::Label fileLabel, barsLabel, bpmLabel, slicesLabel, countLabel, hint, dropHint;
    juce::ComboBox barsBox;
    juce::ToggleButton repitchToggle { "Repitch" };
    std::array<juce::TextButton, 4> divideButtons;
    juce::TextButton detectButton { "Detect" };
    juce::TextButton zoomOutButton { "-" }, zoomInButton { "+" }, fitButton { "Fit" };
    juce::Slider sensitivity;
    BreakSliceView slices;
    RecentMediaList recent;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreakEditor)
};

} // namespace carve::app
