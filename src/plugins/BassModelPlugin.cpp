#include "BassModelPlugin.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins
{

const char* BassModelPlugin::xmlTypeName = "carveBassModel";

namespace
{
    const juce::StringArray styleNames { "Fingers", "Pick", "Thumb", "Pop", "Slap & Pop" };
    const juce::StringArray legatoNames { "Retrigger", "Hammer-on", "Slide" };

    // Notes this close together started as one chord, and are plucked.
    constexpr double chordWindow = 0.02;
}

BassModelPlugin::BassModelPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();
    const bassmodel::Options d;

    styleValue.referTo (state, "style", um, (float) d.style);
    pluckValue.referTo (state, "pluck", um, (float) d.pluck);
    pickupValue.referTo (state, "pickup", um, (float) d.pickup);
    heightValue.referTo (state, "height", um, (float) d.height);
    toneValue.referTo (state, "tone", um, (float) d.tone);
    actionValue.referTo (state, "action", um, (float) d.action);
    sustainValue.referTo (state, "sustain", um, (float) d.sustain);
    brightValue.referTo (state, "bright", um, (float) d.bright);
    muteValue.referTo (state, "mute", um, (float) d.mute);
    slideValue.referTo (state, "slide", um, (float) d.slide);
    fretlessValue.referTo (state, "fretless", um, d.fretless ? 1.0f : 0.0f);
    legatoValue.referTo (state, "legato", um, (float) d.legato);
    driveValue.referTo (state, "drive", um, (float) d.drive);
    ampValue.referTo (state, "amp", um, (float) d.amp);
    volumeValue.referTo (state, "volume", um, (float) d.volume);

    auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto percentValue = [] (const juce::String& s) { return s.getFloatValue() * 0.01f; };
    auto times = [] (float v) { return juce::String::charToString (0x00d7) + juce::String (v, 2); };
    auto plain = [] (const juce::String& s) { return s.retainCharacters ("0123456789.-").getFloatValue(); };
    auto choice = [] (const juce::StringArray& names)
    {
        return [names] (float v) { return names[juce::jlimit (0, juce::jmax (0, names.size() - 1), juce::roundToInt (v))]; };
    };
    auto choiceValue = [] (const juce::StringArray& names)
    {
        return [names] (const juce::String& s)
        {
            const int i = names.indexOf (s.trim(), true);
            return (float) (i >= 0 ? i : juce::jlimit (0, juce::jmax (0, names.size() - 1), s.getIntValue()));
        };
    };

    style = addParam ("style", TRANS ("Playing Style"), { 0.0f, 4.0f, 1.0f }, choice (styleNames), choiceValue (styleNames));
    pluck = addParam ("pluck", TRANS ("Pluck Position"), { 0.06f, 0.45f },
                      [] (float v) { return juce::String (juce::roundToInt (v * bassmodel::scaleLength * 100.0)) + " cm"; },
                      [] (const juce::String& s) { return s.retainCharacters ("0123456789.").getFloatValue() / (float) (bassmodel::scaleLength * 100.0); });
    pickup = addParam ("pickup", TRANS ("Pickup Blend"), { 0.0f, 1.0f },
                       [] (float v) { return v < 0.02f ? juce::String ("neck") : v > 0.98f ? juce::String ("bridge")
                                                       : juce::String (juce::roundToInt (v * 100.0f)) + "% bridge"; },
                       [] (const juce::String& s) { return s.retainCharacters ("0123456789.").getFloatValue() * 0.01f; });
    height = addParam ("height", TRANS ("Pickup Height"), { 0.0f, 1.0f }, percent, percentValue);
    tone = addParam ("tone", TRANS ("Tone"), { 0.0f, 1.0f }, percent, percentValue);
    action = addParam ("action", TRANS ("Action"), { 0.0f, 1.0f }, percent, percentValue);
    sustain = addParam ("sustain", TRANS ("Sustain"), { 0.2f, 2.0f }, times, plain);
    bright = addParam ("bright", TRANS ("String Age"), { 0.0f, 1.0f },
                       [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% new"; }, percentValue);
    mute = addParam ("mute", TRANS ("Palm Mute"), { 0.0f, 1.0f }, percent, percentValue);
    slide = addParam ("slide", TRANS ("Slide Time"), { 0.02f, 0.4f },
                      [] (float v) { return juce::String (juce::roundToInt (v * 1000.0f)) + " ms"; },
                      [] (const juce::String& s) { return s.retainCharacters ("0123456789.").getFloatValue() * 0.001f; });
    fretless = addParam ("fretless", TRANS ("Fretless Slides"), { 0.0f, 1.0f, 1.0f },
                         [] (float v) { return v >= 0.5f ? juce::String ("on") : juce::String ("off"); },
                         [] (const juce::String& s) { return s.trim().equalsIgnoreCase ("on") || s.getFloatValue() >= 0.5f ? 1.0f : 0.0f; });
    legato = addParam ("legato", TRANS ("Legato"), { 0.0f, 2.0f, 1.0f }, choice (legatoNames), choiceValue (legatoNames));
    drive = addParam ("drive", TRANS ("Drive"), { 0.0f, 1.0f }, percent, percentValue);
    amp = addParam ("amp", TRANS ("DI / Amp"), { 0.0f, 1.0f }, percent, percentValue);
    volume = addParam ("volume", TRANS ("Volume"), { 0.0f, 1.5f },
                       [] (float v) { return v <= 0.0f ? juce::String ("-inf dB")
                                                       : juce::String (juce::Decibels::gainToDecibels (v / 0.8f), 1) + " dB"; },
                       [] (const juce::String& s) { return 0.8f * juce::Decibels::decibelsToGain (s.getFloatValue()); });

    style->attachToCurrentValue (styleValue);
    pluck->attachToCurrentValue (pluckValue);
    pickup->attachToCurrentValue (pickupValue);
    height->attachToCurrentValue (heightValue);
    tone->attachToCurrentValue (toneValue);
    action->attachToCurrentValue (actionValue);
    sustain->attachToCurrentValue (sustainValue);
    bright->attachToCurrentValue (brightValue);
    mute->attachToCurrentValue (muteValue);
    slide->attachToCurrentValue (slideValue);
    fretless->attachToCurrentValue (fretlessValue);
    legato->attachToCurrentValue (legatoValue);
    drive->attachToCurrentValue (driveValue);
    amp->attachToCurrentValue (ampValue);
    volume->attachToCurrentValue (volumeValue);
}

BassModelPlugin::~BassModelPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void BassModelPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, styleValue, pluckValue, pickupValue, heightValue, toneValue, actionValue,
                                      sustainValue, brightValue, muteValue, slideValue, fretlessValue, legatoValue,
                                      driveValue, ampValue, volumeValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

bassmodel::Options BassModelPlugin::readOptions() const
{
    bassmodel::Options o;
    o.style = juce::roundToInt (style->getCurrentValue());
    o.pluck = pluck->getCurrentValue();
    o.pickup = pickup->getCurrentValue();
    o.height = height->getCurrentValue();
    o.tone = tone->getCurrentValue();
    o.action = action->getCurrentValue();
    o.sustain = sustain->getCurrentValue();
    o.bright = bright->getCurrentValue();
    o.mute = mute->getCurrentValue();
    o.slide = slide->getCurrentValue();
    o.fretless = fretless->getCurrentValue() >= 0.5f;
    o.legato = juce::roundToInt (legato->getCurrentValue());
    o.drive = drive->getCurrentValue();
    o.amp = amp->getCurrentValue();
    o.volume = volume->getCurrentValue();
    return o;
}

void BassModelPlugin::setFactoryPreset (int index)
{
    if (! juce::isPositiveAndBelow (index, bassmodel::numFactoryPresets))
        return;

    const auto p = bassmodel::getFactoryPreset (index);
    const std::pair<te::AutomatableParameter*, double> values[]
    {
        { style.get(), p.style }, { pluck.get(), p.pluck }, { pickup.get(), p.pickup }, { height.get(), p.height },
        { tone.get(), p.tone }, { action.get(), p.action }, { sustain.get(), p.sustain }, { bright.get(), p.bright },
        { mute.get(), p.mute }, { slide.get(), p.slide }, { fretless.get(), p.fretless ? 1.0 : 0.0 },
        { drive.get(), p.drive }, { amp.get(), p.amp }, { volume.get(), p.volume },
    };

    // The legato setting is how the part is written, not the sound: a preset leaves it.
    for (const auto& [parameter, value] : values)
        parameter->setParameter ((float) value, juce::sendNotification);
}

int BassModelPlugin::getMatchingFactoryPreset() const
{
    const auto o = readOptions();

    for (int i = 0; i < bassmodel::numFactoryPresets; ++i)
    {
        const auto p = bassmodel::getFactoryPreset (i);
        auto near = [] (double a, double b) { return std::abs (a - b) < 1e-4; };

        if (o.style == p.style && o.fretless == p.fretless
             && near (o.pluck, p.pluck) && near (o.pickup, p.pickup) && near (o.height, p.height)
             && near (o.tone, p.tone) && near (o.action, p.action) && near (o.sustain, p.sustain)
             && near (o.bright, p.bright) && near (o.mute, p.mute) && near (o.slide, p.slide)
             && near (o.drive, p.drive) && near (o.amp, p.amp) && near (o.volume, p.volume))
            return i;
    }

    return -1;
}

void BassModelPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    model.setOptions (readOptions());
    model.prepare (info.sampleRate);   // the dispersion fits only when the rate is new
    numKeys = 0;
    samplePosition = 0;
}

// A key down: plucked, or, while another is held, joined to it as the Legato
// setting says.
void BassModelPlugin::startNote (int note, float velocity)
{
    const int mode = model.getOptions().legato;
    const Key* previous = numKeys > 0 ? &keys[(size_t) numKeys - 1] : nullptr;
    const bool join = previous != nullptr && mode != (int) bassmodel::Legato::retrigger
                       && (double) (samplePosition - previous->at) > chordWindow * sampleRate;

    model.noteOn (note, velocity, join && mode == (int) bassmodel::Legato::hammer, join && mode == (int) bassmodel::Legato::slide);

    for (int i = 0; i < numKeys; ++i)
        if (keys[(size_t) i].note == note)
        {
            std::move (keys.begin() + i + 1, keys.begin() + numKeys, keys.begin() + i);
            --numKeys;
            break;
        }

    if (numKeys == (int) keys.size())
    {
        std::move (keys.begin() + 1, keys.end(), keys.begin());
        --numKeys;
    }

    keys[(size_t) numKeys++] = { note, velocity, samplePosition };
}

// A key up. The last one let go while an earlier one is still held goes back
// to that one (pulled off or slid back), as a trill does.
void BassModelPlugin::endNote (int note)
{
    int index = -1;

    for (int i = 0; i < numKeys; ++i)
        if (keys[(size_t) i].note == note)
            index = i;

    if (index < 0)
    {
        model.noteOff (note);
        return;
    }

    const bool wasLast = index == numKeys - 1;
    const float velocity = keys[(size_t) index].velocity;
    std::move (keys.begin() + index + 1, keys.begin() + numKeys, keys.begin() + index);
    --numKeys;

    const int mode = model.getOptions().legato;

    if (wasLast && numKeys > 0 && mode != (int) bassmodel::Legato::retrigger)
        model.noteOn (keys[(size_t) numKeys - 1].note, velocity * 0.7,
                      mode == (int) bassmodel::Legato::hammer, mode == (int) bassmodel::Legato::slide);

    model.noteOff (note);
}

void BassModelPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = buffer.getNumChannels();

    if (numChannels == 0)
        return;

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
    {
        model.allNotesOff();
        numKeys = 0;
    }

    model.setOptions (readOptions());

    auto* out = buffer.getWritePointer (0, fc.bufferStartSample);
    int pos = 0;

    auto renderTo = [&] (int to)
    {
        if (to > pos)
        {
            model.process (out + pos, to - pos);
            samplePosition += to - pos;
            pos = to;
        }
    };

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            renderTo (juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate)));

            if (m.isNoteOn())
                startNote (m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
                endNote (m.getNoteNumber());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                model.allNotesOff();
                numKeys = 0;
            }
        }
    }

    renderTo (fc.bufferNumSamples);

    // Mono: the same on both sides.
    for (int ch = 1; ch < numChannels; ++ch)
        if (ch < 2)
            buffer.copyFrom (ch, fc.bufferStartSample, buffer, 0, fc.bufferStartSample, fc.bufferNumSamples);
        else
            buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);

    for (int si = 0; si < bassmodel::numStrings; ++si)
    {
        const auto v = model.view (si);
        auto& s = stringStates[(size_t) si];
        s.fret.store (v.fret, std::memory_order_relaxed);
        s.held.store (v.held, std::memory_order_relaxed);

        for (size_t k = 0; k < v.shape.size(); ++k)
            s.shape[k].store (v.shape[k], std::memory_order_relaxed);
    }
}

} // namespace carve::plugins
