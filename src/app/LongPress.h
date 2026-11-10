#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// A long press, delivered as a right click.
//
// The app was written for a mouse, and its second button does a great deal:
// every context menu, and deleting -- a right click on a note, a clip or the
// selection takes it away. A touch screen has no second button, so without
// this none of that can be reached at all. Holding a finger still is the
// gesture iOS users already know for "the other thing this does", so it is
// sent to the component under the finger as a right click, exactly as a mouse
// would send it; nothing that handles one has to know the difference.
//
// It listens to every mouse event in the app (add it to the Desktop as a
// global listener) and does nothing until a press has stayed put for the
// hold time.
class LongPressAsRightClick : public juce::MouseListener,
                              private juce::Timer
{
public:
    LongPressAsRightClick() = default;

    void mouseDown (const juce::MouseEvent& e) override
    {
        stopTimer();

        // Its own right click arrives back here, as would a second finger:
        // neither is the start of a press to time.
        if (sending || e.mods.isPopupMenu() || e.source.getIndex() != 0)
            return;

        target = e.eventComponent;
        position = e.position;
        source = e.source.getIndex();
        startTimer (holdMs);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // A finger is never perfectly still; past this it is a drag.
        if (isTimerRunning() && e.source.getIndex() == source
            && e.getDistanceFromDragStart() > slopPixels)
            stopTimer();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.source.getIndex() == source)
            stopTimer();
    }

private:
    static constexpr int holdMs = 500;
    static constexpr int slopPixels = 10;

    void timerCallback() override
    {
        stopTimer();

        if (target == nullptr || ! target->isShowing())
            return;

        auto mouse = juce::Desktop::getInstance().getMouseSource (source);

        if (mouse == nullptr)
            return;

        const juce::ScopedValueSetter<bool> scope (sending, true);
        const auto now = juce::Time::getCurrentTime();

        auto event = [&] (juce::ModifierKeys mods)
        {
            return juce::MouseEvent (*mouse, position, mods, juce::MouseInputSource::defaultPressure,
                                     juce::MouseInputSource::defaultOrientation,
                                     juce::MouseInputSource::defaultRotation,
                                     juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                                     target.getComponent(), target.getComponent(), now, position, now, 1, false);
        };

        const auto keys = juce::ModifierKeys::currentModifiers.withoutMouseButtons();

        // The press that has been held is a left button down as far as the
        // component knows, with whatever that started -- a move, a rubber
        // band -- still under way. That is ended first, so the right click
        // arrives at a component that is at rest, as it would from a mouse.
        // Each call may have deleted the component, hence the checks.
        target->mouseUp (event (keys.withFlags (juce::ModifierKeys::leftButtonModifier)));

        if (target != nullptr)
            target->mouseDown (event (keys.withFlags (juce::ModifierKeys::rightButtonModifier)));

        if (target != nullptr)
            target->mouseUp (event (keys.withFlags (juce::ModifierKeys::rightButtonModifier)));
    }

    juce::Component::SafePointer<juce::Component> target;
    juce::Point<float> position;
    int source = 0;
    bool sending = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LongPressAsRightClick)
};

} // namespace carve::app
