#include "EffectGraphs.h"
#include "Fonts.h"

namespace carve::app
{

namespace
{
    // What the plot covers. The gain range is the plugin's own, so a handle
    // dragged to the top is a band at its limit rather than at the edge of a
    // picture that happens to end there.
    constexpr float minHz = 20.0f;
    constexpr float maxHz = 20000.0f;
    constexpr float gainRangeDb = te::EqualiserPlugin::maxGain;

    // Big enough that the band's number inside it is readable at the app's
    // minimum font size, which is what sets the size of the handle.
    constexpr float handleRadius = 8.5f;
    constexpr float grabRadius = 14.0f;

    const juce::Colour background { 0xff1c1c20 };
    const juce::Colour grid { 0xff32323a };
    const juce::Colour gridZero { 0xff4a4a54 };
    const juce::Colour text { 0xff8a8a94 };
    const juce::Colour curve { 0xffe0a24f };

    // A colour per band, so a handle and the part of the curve it is
    // responsible for read as the same thing.
    const juce::Colour bandColours[] { juce::Colour (0xff6fb7c9), juce::Colour (0xff7fc07f),
                                       juce::Colour (0xffd88fd8), juce::Colour (0xffe0a24f) };
} // namespace

//==============================================================================
EqualiserGraph::EqualiserGraph (te::EqualiserPlugin& eq) : plugin (eq)
{
    setWantsKeyboardFocus (false);
    startTimerHz (15);
}

std::array<EqualiserGraph::Band, 4> EqualiserGraph::bands()
{
    if (plugin == nullptr)
        return {};

    return { Band { "Low", plugin->loFreq, plugin->loGain, plugin->loQ },
             Band { "Mid 1", plugin->midFreq1, plugin->midGain1, plugin->midQ1 },
             Band { "Mid 2", plugin->midFreq2, plugin->midGain2, plugin->midQ2 },
             Band { "High", plugin->hiFreq, plugin->hiGain, plugin->hiQ } };
}

juce::Rectangle<float> EqualiserGraph::plotArea() const
{
    // Room at the bottom for the frequency labels.
    return getLocalBounds().toFloat().reduced (8.0f, 6.0f).withTrimmedBottom (16.0f);
}

float EqualiserGraph::frequencyToX (float hz) const
{
    const auto area = plotArea();
    const auto proportion = std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
    return area.getX() + area.getWidth() * (float) proportion;
}

float EqualiserGraph::xToFrequency (float x) const
{
    const auto area = plotArea();
    const auto proportion = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / area.getWidth());
    return minHz * std::pow (maxHz / minHz, proportion);
}

float EqualiserGraph::gainToY (float db) const
{
    const auto area = plotArea();
    return area.getCentreY() - area.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, db / gainRangeDb);
}

float EqualiserGraph::yToGain (float y) const
{
    const auto area = plotArea();
    return juce::jlimit (-gainRangeDb, gainRangeDb,
                         (area.getCentreY() - y) / (area.getHeight() * 0.5f) * gainRangeDb);
}

int EqualiserGraph::bandAt (juce::Point<float> position) const
{
    if (plugin == nullptr)
        return -1;

    auto self = const_cast<EqualiserGraph*> (this);
    auto closest = -1;
    auto closestDistance = grabRadius;

    const auto all = self->bands();

    for (int i = 0; i < (int) all.size(); ++i)
    {
        if (all[(size_t) i].frequency == nullptr)
            continue;

        const juce::Point<float> handle { frequencyToX (all[(size_t) i].frequency->getCurrentValue()),
                                          gainToY (all[(size_t) i].gain->getCurrentValue()) };

        if (const auto distance = handle.getDistanceFrom (position); distance < closestDistance)
        {
            closestDistance = distance;
            closest = i;
        }
    }

    return closest;
}

//==============================================================================
void EqualiserGraph::paint (juce::Graphics& g)
{
    g.fillAll (background);

    const auto area = plotArea();

    // The decades, plus the thirds inside them: the lines an ear looks for.
    g.setFont (uiFont (fonts::small));

    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f,
                      5000.0f, 10000.0f, 20000.0f })
    {
        const auto x = frequencyToX (hz);
        const auto labelled = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;

        g.setColour (labelled ? gridZero : grid);
        g.drawVerticalLine ((int) x, area.getY(), area.getBottom());

        if (labelled)
        {
            g.setColour (text);
            g.drawText (hz < 1000.0f ? juce::String ((int) hz) : juce::String ((int) (hz / 1000.0f)) + "k",
                        juce::Rectangle<float> (x - 20.0f, area.getBottom(), 40.0f, 16.0f),
                        juce::Justification::centred);
        }
    }

    for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
    {
        g.setColour (db == 0.0f ? gridZero : grid);
        g.drawHorizontalLine ((int) gainToY (db), area.getX(), area.getRight());
    }

    if (plugin == nullptr)
        return;

    // The curve, straight from the filters that are running.
    juce::Path path;

    for (float x = area.getX(); x <= area.getRight(); x += 1.0f)
    {
        const auto db = plugin->getDBGainAtFrequency (xToFrequency (x));
        const auto y = gainToY (db);

        if (x == area.getX())
            path.startNewSubPath (x, y);
        else
            path.lineTo (x, y);
    }

    // Filled down to the zero line, which is what makes a cut read as a cut
    // rather than as a differently-shaped boost.
    {
        auto filled = path;
        filled.lineTo (area.getRight(), gainToY (0.0f));
        filled.lineTo (area.getX(), gainToY (0.0f));
        filled.closeSubPath();

        g.setColour (curve.withAlpha (0.18f));
        g.fillPath (filled);
    }

    g.setColour (curve);
    g.strokePath (path, juce::PathStrokeType (1.6f));

    // The handles.
    const auto all = bands();

    for (int i = 0; i < (int) all.size(); ++i)
    {
        const auto& band = all[(size_t) i];

        if (band.frequency == nullptr)
            continue;

        const juce::Point<float> handle { frequencyToX (band.frequency->getCurrentValue()),
                                          gainToY (band.gain->getCurrentValue()) };
        const auto colour = bandColours[i];
        const auto active = i == draggedBand || i == hoveredBand;

        g.setColour (colour.withAlpha (active ? 0.35f : 0.2f));
        g.fillEllipse (juce::Rectangle<float> (handleRadius * 3.0f, handleRadius * 3.0f).withCentre (handle));

        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f).withCentre (handle));

        g.setColour (background);
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText (juce::String (i + 1),
                    juce::Rectangle<float> (handleRadius * 2.0f, handleRadius * 2.0f).withCentre (handle),
                    juce::Justification::centred);
    }

    // What the pointer is over, spelled out: a handle's position says the
    // frequency and the gain but never the Q.
    if (const auto shown = draggedBand >= 0 ? draggedBand : hoveredBand; shown >= 0)
    {
        const auto& band = all[(size_t) shown];

        if (band.frequency != nullptr)
        {
            const auto hz = band.frequency->getCurrentValue();
            const juce::String description =
                juce::String (band.name) + "   "
                 + (hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz"
                                 : juce::String (hz * 0.001f, 2) + " kHz")
                 + "   " + juce::String (band.gain->getCurrentValue(), 1) + " dB"
                 + "   Q " + juce::String (band.q->getCurrentValue(), 2);

            g.setColour (bandColours[shown]);
            g.setFont (uiFont (fonts::small));
            g.drawText (description, area.reduced (4.0f, 2.0f), juce::Justification::topLeft);
        }
    }
}

//==============================================================================
void EqualiserGraph::mouseDown (const juce::MouseEvent& e)
{
    draggedBand = bandAt (e.position);
    repaint();
}

void EqualiserGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (draggedBand < 0 || plugin == nullptr)
        return;

    const auto all = bands();
    const auto& band = all[(size_t) draggedBand];

    if (band.frequency == nullptr)
        return;

    band.frequency->setParameter (juce::jlimit (te::EqualiserPlugin::minFreq,
                                                te::EqualiserPlugin::maxFreq,
                                                xToFrequency (e.position.x)),
                                  juce::sendNotification);
    band.gain->setParameter (yToGain (e.position.y), juce::sendNotification);
    repaint();
}

void EqualiserGraph::mouseUp (const juce::MouseEvent& e)
{
    draggedBand = -1;
    hoveredBand = bandAt (e.position);
    repaint();
}

void EqualiserGraph::mouseMove (const juce::MouseEvent& e)
{
    if (const auto band = bandAt (e.position); band != hoveredBand)
    {
        hoveredBand = band;
        setMouseCursor (band >= 0 ? juce::MouseCursor::DraggingHandCursor
                                  : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void EqualiserGraph::mouseExit (const juce::MouseEvent&)
{
    hoveredBand = -1;
    repaint();
}

void EqualiserGraph::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (plugin == nullptr)
        return;

    // Flat, which is the one gain nobody wants to hunt for with a mouse.
    if (const auto band = bandAt (e.position); band >= 0)
    {
        bands()[(size_t) band].gain->setParameter (0.0f, juce::sendNotification);
        repaint();
    }
}

void EqualiserGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (plugin == nullptr)
        return;

    const auto band = draggedBand >= 0 ? draggedBand : bandAt (e.position);

    if (band < 0)
        return;

    // Q on the wheel: it is the third dimension of a handle that only has two,
    // and the one a mouse is otherwise no use for.
    auto q = bands()[(size_t) band].q;

    if (q == nullptr)
        return;

    q->setParameter (juce::jlimit (te::EqualiserPlugin::minQ, te::EqualiserPlugin::maxQ,
                                   q->getCurrentValue() * (1.0f + wheel.deltaY * 1.5f)),
                     juce::sendNotification);
    repaint();
}

void EqualiserGraph::timerCallback()
{
    if (plugin == nullptr)
    {
        setEnabled (false);
        stopTimer();
        return;
    }

    // Cheap: the plugin only rebuilds its curve when something moved.
    repaint();
}

//==============================================================================
namespace
{
    // What the meters cover. Overtop's threshold goes down to -60dB, and a
    // little headroom above zero keeps a hot band from pinning silently.
    constexpr float minLevelDb = -66.0f;
    constexpr float maxLevelDb = 6.0f;

    const char* bandNames[] { "LOW", "MID", "HIGH" };

    // dB per repaint the meter is allowed to fall. At 30Hz that is a 45dB/s
    // decay: slow enough to read a transient, fast enough to follow a part.
    constexpr float meterDecayDb = 1.5f;
} // namespace

OvertopGraph::OvertopGraph (plugins::OvertopPlugin& overtop) : plugin (overtop)
{
    startTimerHz (30);
}

juce::Rectangle<float> OvertopGraph::plotArea() const
{
    // Room down the right for the dB scale, and along the bottom for the band
    // names.
    return getLocalBounds().toFloat().reduced (8.0f, 6.0f)
                                     .withTrimmedRight (36.0f)
                                     .withTrimmedBottom (16.0f);
}

juce::Rectangle<float> OvertopGraph::columnFor (int band) const
{
    const auto area = plotArea();
    const auto width = area.getWidth() / 3.0f;
    return area.withWidth (width).translated (width * (float) band, 0.0f).reduced (10.0f, 0.0f);
}

float OvertopGraph::levelToY (float db) const
{
    const auto area = plotArea();
    const auto proportion = (juce::jlimit (minLevelDb, maxLevelDb, db) - minLevelDb)
                             / (maxLevelDb - minLevelDb);
    return area.getBottom() - area.getHeight() * proportion;
}

float OvertopGraph::yToLevel (float y) const
{
    const auto area = plotArea();
    const auto proportion = juce::jlimit (0.0f, 1.0f, (area.getBottom() - y) / area.getHeight());
    return minLevelDb + proportion * (maxLevelDb - minLevelDb);
}

void OvertopGraph::paint (juce::Graphics& g)
{
    g.fillAll (background);

    const auto area = plotArea();

    g.setFont (uiFont (fonts::small));

    for (float db : { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f })
    {
        const auto y = levelToY (db);
        g.setColour (db == 0.0f ? gridZero : grid);
        g.drawHorizontalLine ((int) y, area.getX(), area.getRight());

        g.setColour (text);
        g.drawText (juce::String ((int) db),
                    juce::Rectangle<float> (area.getRight() + 2.0f, y - 8.0f, 34.0f, 16.0f),
                    juce::Justification::centredLeft);
    }

    if (plugin == nullptr)
        return;

    const auto thresholdDb = plugin->thresholdDb != nullptr ? plugin->thresholdDb->getCurrentValue() : -30.0f;
    const auto thresholdY = levelToY (thresholdDb);

    for (int band = 0; band < 3; ++band)
    {
        const auto column = columnFor (band);
        const auto levelY = levelToY (shownLevelDb[(size_t) band]);

        g.setColour (juce::Colour (0xff26262c));
        g.fillRect (column.withTop (area.getY()));

        // The bar: up to the threshold in one colour, past it in another --
        // the two halves of what this effect does.
        const auto boosting = shownGainDb[(size_t) band] > 0.05f;
        const auto reducing = shownGainDb[(size_t) band] < -0.05f;

        g.setColour (boosting ? juce::Colour (0xff6fb7c9)
                   : reducing ? juce::Colour (0xffe0a24f)
                              : juce::Colour (0xff7a7a84));
        g.fillRect (column.withTop (levelY));

        // What it is being given, as a mark above (a boost) or below (a
        // reduction) the top of the bar.
        if (boosting || reducing)
        {
            const auto gainY = levelToY (shownLevelDb[(size_t) band] + shownGainDb[(size_t) band]);

            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.drawHorizontalLine ((int) gainY, column.getX(), column.getRight());

            g.setFont (uiFont (fonts::small));
            g.drawText ((shownGainDb[(size_t) band] > 0.0f ? "+" : "")
                         + juce::String (shownGainDb[(size_t) band], 1),
                        column.withY (gainY - 17.0f).withHeight (16.0f),
                        juce::Justification::centred);
        }

        g.setColour (text);
        g.setFont (uiFont (fonts::small, juce::Font::bold));
        g.drawText (bandNames[band], column.withY (area.getBottom()).withHeight (16.0f),
                    juce::Justification::centred);
    }

    // The threshold, across all three: one setting, one line.
    g.setColour (juce::Colour (0xffd8d8dc));
    g.drawHorizontalLine ((int) thresholdY, area.getX() - 2.0f, area.getRight() + 2.0f);

    g.setFont (uiFont (fonts::small, juce::Font::bold));
    g.drawText (juce::String (thresholdDb, 1) + " dB",
                juce::Rectangle<float> (area.getX(), thresholdY - 17.0f, 74.0f, 16.0f),
                juce::Justification::centredLeft);

    // A grip, so it looks like the draggable thing it is.
    g.fillRect (juce::Rectangle<float> (10.0f, 3.0f).withCentre ({ area.getRight() - 4.0f, thresholdY }));
}

void OvertopGraph::mouseDown (const juce::MouseEvent& e)
{
    if (plugin == nullptr || plugin->thresholdDb == nullptr)
        return;

    // Anywhere in the plot: the line is thin, and there is nothing else here
    // to grab.
    draggingThreshold = plotArea().expanded (4.0f).contains (e.position);

    if (draggingThreshold)
        mouseDrag (e);
}

void OvertopGraph::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingThreshold || plugin == nullptr || plugin->thresholdDb == nullptr)
        return;

    const auto range = plugin->thresholdDb->getValueRange();
    plugin->thresholdDb->setParameter (juce::jlimit (range.getStart(), range.getEnd(),
                                                     yToLevel (e.position.y)),
                                       juce::sendNotification);
    repaint();
}

void OvertopGraph::mouseUp (const juce::MouseEvent&)
{
    draggingThreshold = false;
}

void OvertopGraph::timerCallback()
{
    if (plugin == nullptr)
    {
        setEnabled (false);
        stopTimer();
        return;
    }

    // A bypassed plugin is not called at all, so its last reading would
    // otherwise sit there.
    const auto live = plugin->isEnabled();

    for (int band = 0; band < 3; ++band)
    {
        const auto level = live ? plugin->getBandLevelDb (band) : minLevelDb;
        shownLevelDb[(size_t) band] = juce::jmax (level, shownLevelDb[(size_t) band] - meterDecayDb);
        shownGainDb[(size_t) band] = live ? plugin->getBandGainDb (band) : 0.0f;
    }

    repaint();
}

//==============================================================================
EffectGraph createEffectGraph (te::Plugin& plugin)
{
    if (auto* eq = dynamic_cast<te::EqualiserPlugin*> (&plugin))
        return { std::make_unique<EqualiserGraph> (*eq), EqualiserGraph::preferredHeight };

    if (auto* overtop = dynamic_cast<plugins::OvertopPlugin*> (&plugin))
        return { std::make_unique<OvertopGraph> (*overtop), OvertopGraph::preferredHeight };

    return {};
}

} // namespace carve::app
