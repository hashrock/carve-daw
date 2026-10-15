#include "TypingKeyboard.h"

namespace carve::app
{

namespace
{
    // Semitones above the home note, in key order: a s d f g h j k l ; are the
    // white keys, w e t y u o p the black ones between them.
    constexpr const char* keys = "awsedftgyhujkolp;";

    constexpr int velocity = 100;
    constexpr int maxOctaveShift = 4;

    // Key releases are not reliably delivered to whoever saw the press -- the
    // focus can move while a key is down -- so they are polled instead, and
    // only while something is held.
    constexpr int releasePollMs = 10;

    int semitoneFor (const juce::KeyPress& key)
    {
        const auto c = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) key.getKeyCode());

        for (int i = 0; keys[i] != 0; ++i)
            if (c == (juce::juce_wchar) keys[i])
                return i;

        return -1;
    }
} // namespace

TypingKeyboard::TypingKeyboard()
{
    juce::Desktop::getInstance().addFocusChangeListener (this);
}

TypingKeyboard::~TypingKeyboard()
{
    juce::Desktop::getInstance().removeFocusChangeListener (this);
    attachTo (nullptr);
}

void TypingKeyboard::setEnabled (bool shouldBeEnabled)
{
    if (enabled == shouldBeEnabled)
        return;

    enabled = shouldBeEnabled;

    if (! enabled)
        releaseAll();

    attachTo (enabled ? juce::Component::getCurrentlyFocusedComponent() : nullptr);
}

void TypingKeyboard::setHomeNote (int midiNote)
{
    homeNote = midiNote;
    octaveShift = 0;
}

bool TypingKeyboard::keyPressed (const juce::KeyPress& key, juce::Component*)
{
    if (! enabled)
        return false;

    const auto mods = key.getModifiers();

    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    if (dynamic_cast<juce::TextEditor*> (juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false;

    if (key == juce::KeyPress ('z') || key == juce::KeyPress ('x'))
    {
        octaveShift = juce::jlimit (-maxOctaveShift, maxOctaveShift,
                                    octaveShift + (key == juce::KeyPress ('x') ? 1 : -1));
        return true;
    }

    const auto semitone = semitoneFor (key);

    if (semitone < 0)
        return false;

    // Auto-repeat: the key is already sounding.
    for (const auto& h : held)
        if (h.keyCode == key.getKeyCode())
            return true;

    const auto note = homeNote + octaveShift * 12 + semitone;

    if (note < 0 || note > 127)
        return true;

    held.add ({ key.getKeyCode(), note });
    state.noteOn (1, note, velocity / 127.0f);
    startTimer (releasePollMs);
    return true;
}

void TypingKeyboard::timerCallback()
{
    for (int i = held.size(); --i >= 0;)
    {
        if (juce::KeyPress::isKeyCurrentlyDown (held[i].keyCode))
            continue;

        state.noteOff (1, held[i].note, 0.0f);
        held.remove (i);
    }

    if (held.isEmpty())
        stopTimer();
}

void TypingKeyboard::releaseAll()
{
    for (const auto& h : held)
        state.noteOff (1, h.note, 0.0f);

    held.clear();
    stopTimer();
}

void TypingKeyboard::globalFocusChanged (juce::Component* focused)
{
    if (enabled)
        attachTo (focused);
}

void TypingKeyboard::attachTo (juce::Component* component)
{
    if (attachedTo == component)
        return;

    if (auto* previous = attachedTo.get())
        previous->removeKeyListener (this);

    attachedTo = component;

    if (component != nullptr)
        component->addKeyListener (this);
}

} // namespace carve::app
