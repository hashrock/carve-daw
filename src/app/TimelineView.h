#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Machinery the playlist and the piano roll both need. They are different
// enough to be separate components -- one shows generators against time, the
// other pitches against time -- but they zoom identically, and were each
// carrying their own copy of it.

namespace carve::app
{

// Zoom that keeps whatever is under anchorX under anchorX afterwards.
//
// The order is easy to get subtly wrong, which is the reason this is shared
// rather than written twice: the anchor beat has to be read before the scale
// moves, the view has to be resized to the new scale before the viewport is
// repositioned, and the anchor's offset has to be measured in screen space
// rather than in content space.
template <typename BeatAtX, typename XAtBeat, typename ApplyScale>
void zoomAroundAnchor (const juce::Component& view, float anchorX,
                       BeatAtX&& beatAtX, XAtBeat&& xAtBeat, ApplyScale&& applyScale)
{
    auto* viewport = view.findParentComponentOfClass<juce::Viewport>();
    const auto anchorBeat = beatAtX (anchorX);
    const auto anchorScreenX = juce::roundToInt (anchorX)
                                   - (viewport != nullptr ? viewport->getViewPositionX() : 0);

    applyScale();

    if (viewport != nullptr)
        viewport->setViewPosition (juce::jmax (0, juce::roundToInt (xAtBeat (anchorBeat)) - anchorScreenX),
                                   viewport->getViewPositionY());
}

} // namespace carve::app
