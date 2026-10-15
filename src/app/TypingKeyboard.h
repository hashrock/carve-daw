#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace carve::app
{

// The computer keyboard as a piano, for everyone who has an idea and no MIDI
// keyboard on the desk -- which is most people with a laptop.
//
// The layout everybody already knows from Live and Logic: the home row is the
// white keys from A, the row above it the black ones, Z and X shift an
// octave. What it plays goes into a MidiKeyboardState, the same thing a
// hardware keyboard drives, so MidiInputController hears it the way it hears
// any device and everything downstream of that -- sounding, recording,
// capture -- comes for free.
//
// While it is on, those letters are notes and not shortcuts: the piano roll's
// D and E are the obvious casualties, and switching it off (Cmd+K, or the
// transport bar) gives them back. Getting in ahead of the view that has focus
// is the awkward part: JUCE offers a key to the focused component first and
// only lets it bubble up if that one declines, so this follows the focus
// around and listens on whatever component holds it, where key listeners are
// asked before the component itself. A text field keeps its keys -- typing a
// pattern name must not play it.
class TypingKeyboard : public juce::KeyListener,
                       private juce::FocusChangeListener,
                       private juce::Timer
{
public:
    TypingKeyboard();
    ~TypingKeyboard() override;

    juce::MidiKeyboardState state;

    bool isEnabled() const  { return enabled; }
    void setEnabled (bool);

    // Where the A key sits before any Z/X: middle C for an instrument, the
    // kick for a drum generator, so the first key pressed is a sound either way.
    void setHomeNote (int midiNote);

    // Also the fallback for when nothing has focus and the key went straight
    // to a window, which forwards it to MainComponent's global keys.
    bool keyPressed (const juce::KeyPress&, juce::Component* originator) override;

private:
    void globalFocusChanged (juce::Component* focused) override;
    void timerCallback() override;

    void attachTo (juce::Component*);
    void releaseAll();

    bool enabled = false;
    int homeNote = 60;
    int octaveShift = 0;

    // The keys down, as (key code, the note it started), so a key let go
    // after Z/X ends the note it began rather than the one it would play now.
    struct HeldKey
    {
        int keyCode = 0;
        int note = 0;
    };

    juce::Array<HeldKey> held;

    juce::WeakReference<juce::Component> attachedTo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TypingKeyboard)
};

} // namespace carve::app
