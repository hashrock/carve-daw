#include "SettingsComponent.h"
#include "Fonts.h"

namespace carve::app
{

namespace
{
    const juce::Colour backgroundColour { 0xff232327 };
    const juce::Colour textColour { 0xffd8d8dc };
    const juce::Colour dimTextColour { 0xff9a9aa4 };

    constexpr int midiRowHeight = 24;

    void styleLabel (juce::Label& label, juce::Colour colour, float height)
    {
        label.setFont (uiFont (height));
        label.setColour (juce::Label::textColourId, colour);
    }
} // namespace

SettingsComponent::SettingsComponent (te::Engine& engineToUse)
    : engine (engineToUse)
{
    // No heading: the window is called Settings, and saying it twice in the
    // same 20 pixels is not saying it twice as clearly.
    styleLabel (audioLabel, textColour, fonts::normal);
    audioLabel.setText ("Audio output", juce::dontSendNotification);

    styleLabel (midiLabel, textColour, fonts::normal);
    midiLabel.setText ("MIDI input", juce::dontSendNotification);

    styleLabel (midiHintLabel, dimTextColour, fonts::small);
    midiHintLabel.setText ("What a keyboard plays goes to the selected generator, "
                           "and to the record button.",
                           juce::dontSendNotification);

    // No inputs: 0 in, 1-2 out, no MIDI options (they are below, and JUCE's
    // are about recording into the device manager rather than about this app).
    audioSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (
        engine.getDeviceManager().deviceManager,
        0, 0,     // input channels: none, ever
        1, 2,     // output channels
        false,    // no MIDI input list of its own
        false,    // no MIDI output selector
        true,     // stereo pairs
        false);   // no "advanced" disclosure -- the buffer size is not advanced

    midiViewport.setViewedComponent (&midiList, false);
    midiViewport.setScrollBarsShown (true, false);

    for (auto* c : std::initializer_list<juce::Component*> {
             &audioLabel, &midiLabel, &midiHintLabel, &midiViewport })
        addAndMakeVisible (c);

    addAndMakeVisible (*audioSelector);

    engine.getDeviceManager().addChangeListener (this);
    rebuildMidiInputs();

    setSize (preferredWidth, preferredHeight);
}

SettingsComponent::~SettingsComponent()
{
    engine.getDeviceManager().removeChangeListener (this);
}

void SettingsComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildMidiInputs();
}

void SettingsComponent::rebuildMidiInputs()
{
    auto devices = engine.getDeviceManager().getMidiInDevices();

    // Physical devices only: tracktion's "all MIDI ins" is a view of these
    // rather than a device anybody plugged in, and offering both would be
    // offering the same keyboard twice.
    std::erase_if (devices, [] (const std::shared_ptr<te::MidiInputDevice>& device)
    {
        return device == nullptr
                || device->getDeviceType() != te::InputDevice::physicalMidiDevice;
    });

    // The same devices in the same order: leave the buttons alone rather than
    // rebuilding them under the pointer every time the engine rescans (which
    // it does every few seconds).
    bool sameList = devices.size() == midiRows.size();

    for (size_t i = 0; sameList && i < devices.size(); ++i)
        sameList = midiRows[i].deviceID == devices[i]->getDeviceID();

    if (sameList)
    {
        for (size_t i = 0; i < devices.size(); ++i)
            midiRows[i].button->setToggleState (devices[i]->isEnabled(), juce::dontSendNotification);

        return;
    }

    midiRows.clear();

    for (const auto& device : devices)
    {
        MidiRow row;
        row.deviceID = device->getDeviceID();
        row.button = std::make_unique<juce::ToggleButton> (device->getName());
        row.button->setToggleState (device->isEnabled(), juce::dontSendNotification);
        row.button->setColour (juce::ToggleButton::textColourId, textColour);

        // Held weakly on purpose: the device list is rebuilt by the engine
        // whenever the hardware changes, so the device this row named may be
        // gone by the time the button is clicked.
        row.button->onClick = [this, deviceID = row.deviceID, button = row.button.get()]
        {
            for (const auto& d : engine.getDeviceManager().getMidiInDevices())
                if (d != nullptr && d->getDeviceID() == deviceID)
                    d->setEnabled (button->getToggleState());
        };

        midiList.addAndMakeVisible (*row.button);
        midiRows.push_back (std::move (row));
    }

    resized();
}

void SettingsComponent::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    // An empty list with nothing said about it reads as a list that has not
    // loaded yet.
    if (midiRows.empty())
    {
        g.setColour (dimTextColour);
        g.setFont (uiFont (fonts::small));
        g.drawText ("No MIDI inputs found", midiViewport.getBounds(),
                    juce::Justification::centredLeft);
    }

    g.setColour (juce::Colour (0xff3a3a40));

    // The line above the MIDI half, drawn where resized() puts the label.
    const auto y = getHeight() - 24 - (midiRows.empty() ? midiRowHeight : (int) midiRows.size() * midiRowHeight)
                     - 18 - 18 - 20;
    g.drawHorizontalLine (juce::jmax (0, y), 14.0f, (float) getWidth() - 14.0f);
}

void SettingsComponent::resized()
{
    auto area = getLocalBounds().reduced (14, 12);

    // The MIDI half takes what it needs from the bottom; the audio selector
    // gets the rest, because it is the half with a table in it.
    const auto listHeight = juce::jlimit (midiRowHeight, 5 * midiRowHeight,
                                          juce::jmax (1, (int) midiRows.size()) * midiRowHeight);

    auto midiArea = area.removeFromBottom (listHeight + 18 + 18 + 12);
    midiLabel.setBounds (midiArea.removeFromTop (18));
    midiHintLabel.setBounds (midiArea.removeFromTop (18));
    midiArea.removeFromTop (6);
    midiViewport.setBounds (midiArea.removeFromTop (listHeight));

    midiList.setSize (midiViewport.getMaximumVisibleWidth(),
                      juce::jmax (listHeight, (int) midiRows.size() * midiRowHeight));

    for (size_t i = 0; i < midiRows.size(); ++i)
        midiRows[i].button->setBounds (0, (int) i * midiRowHeight, midiList.getWidth(), midiRowHeight);

    area.removeFromBottom (10);
    audioLabel.setBounds (area.removeFromTop (18));
    area.removeFromTop (4);
    audioSelector->setBounds (area);
}

} // namespace carve::app
