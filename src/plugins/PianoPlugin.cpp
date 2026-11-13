#include "PianoPlugin.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins
{

const char* PianoPlugin::xmlTypeName = "carvePiano";

PianoPlugin::PianoPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();
    const piano::Options defaults;

    // Hardness is stored as an exponent: the felt's stiffness is 4^v times
    // the modelled one, so the knob's centre is the instrument as designed.
    hardnessValue.referTo (state, "hardness", um, 0.0f);
    knockValue.referTo (state, "knock", um, (float) defaults.knock);
    detuneValue.referTo (state, "detune", um, (float) defaults.detune);
    sympatheticValue.referTo (state, "sympathetic", um, (float) defaults.sympathetic);
    brightnessValue.referTo (state, "brightness", um, (float) defaults.brightness);
    boardValue.referTo (state, "board", um, (float) defaults.board);
    reverbValue.referTo (state, "reverb", um, (float) defaults.reverb);
    volumeValue.referTo (state, "volume", um, (float) defaults.volume);
    pedalValue.referTo (state, "pedal", um, 0.0f);

    auto times = [] (float v) { return juce::String::charToString (0x00d7) + juce::String (v, 2); };
    auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto percentValue = [] (const juce::String& s) { return s.getFloatValue() * 0.01f; };
    auto plain = [] (const juce::String& s) { return s.retainCharacters ("0123456789.-").getFloatValue(); };

    hardness = addParam ("hardness", TRANS ("Hammer Hardness"), { -2.0f, 2.0f },
                         [] (float v) { return juce::String::charToString (0x00d7) + juce::String (std::pow (4.0f, v), 2); },
                         [] (const juce::String& s)
                         {
                             const auto factor = s.retainCharacters ("0123456789.").getFloatValue();
                             return factor > 0.0f ? std::log (factor) / std::log (4.0f) : 0.0f;
                         });
    knock = addParam ("knock", TRANS ("Knock"), { 0.0f, 4.0f }, times, plain);
    detune = addParam ("detune", TRANS ("Unison Detune"), { 0.0f, 5.0f }, times, plain);
    sympathetic = addParam ("sympathetic", TRANS ("String Resonance"), { 0.0f, 3.0f }, times, plain);
    brightness = addParam ("brightness", TRANS ("Brightness"), { 0.0f, 2.0f }, percent, percentValue);
    board = addParam ("board", TRANS ("Soundboard"), { 0.0f, 2.0f }, times, plain);
    reverb = addParam ("reverb", TRANS ("Room"), { 0.0f, 0.6f }, percent, percentValue);
    volume = addParam ("volume", TRANS ("Volume"), { 0.0f, 1.5f },
                       [] (float v) { return v <= 0.0f ? juce::String ("-inf dB")
                                                       : juce::String (juce::Decibels::gainToDecibels (v / 0.8f), 1) + " dB"; },
                       [] (const juce::String& s) { return 0.8f * juce::Decibels::decibelsToGain (s.getFloatValue()); });

    pedal = addParam ("pedal", TRANS ("Sustain Pedal"), { 0.0f, 1.0f, 1.0f },
                      [] (float v) { return v >= 0.5f ? juce::String ("down") : juce::String ("up"); },
                      [] (const juce::String& s) { return s.trim().equalsIgnoreCase ("down") || s.getFloatValue() >= 0.5f ? 1.0f : 0.0f; });

    hardness->attachToCurrentValue (hardnessValue);
    knock->attachToCurrentValue (knockValue);
    detune->attachToCurrentValue (detuneValue);
    sympathetic->attachToCurrentValue (sympatheticValue);
    brightness->attachToCurrentValue (brightnessValue);
    board->attachToCurrentValue (boardValue);
    reverb->attachToCurrentValue (reverbValue);
    volume->attachToCurrentValue (volumeValue);
    pedal->attachToCurrentValue (pedalValue);
}

PianoPlugin::~PianoPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void PianoPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, hardnessValue, knockValue, detuneValue, sympatheticValue,
                                      brightnessValue, boardValue, reverbValue, volumeValue, pedalValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

piano::Options PianoPlugin::readOptions() const
{
    piano::Options o;
    o.hardness = std::pow (4.0, (double) hardness->getCurrentValue());
    o.knock = knock->getCurrentValue();
    o.detune = detune->getCurrentValue();
    o.sympathetic = sympathetic->getCurrentValue();
    o.brightness = brightness->getCurrentValue();
    o.board = board->getCurrentValue();
    o.reverb = reverb->getCurrentValue();
    o.volume = volume->getCurrentValue();
    return o;
}

void PianoPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    model.prepare (info.sampleRate);   // allocates only when the rate is new
    model.setOptions (readOptions());
    keysDown[0] = keysDown[1] = 0;
    pedalDown = false;
    midiPedal = false;
}

void PianoPlugin::setKey (int note, bool down)
{
    if (! juce::isPositiveAndBelow (note, 128))
        return;

    const auto bit = std::uint64_t (1) << (note & 63);
    auto& word = keysDown[note >> 6];

    if (down)
        word.fetch_or (bit, std::memory_order_relaxed);
    else
        word.fetch_and (~bit, std::memory_order_relaxed);
}

void PianoPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = buffer.getNumChannels();

    if (numChannels == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
    {
        model.allNotesOff();
        keysDown[0] = keysDown[1] = 0;
        pedalDown = false;
        midiPedal = false;
    }

    model.setOptions (readOptions());

    // The pedal is down if either the switch or the keyboard says so.
    auto updatePedal = [this]
    {
        const bool down = midiPedal || pedal->getCurrentValue() >= 0.5f;

        if (down != pedalDown.load (std::memory_order_relaxed))
        {
            model.setPedal (down);
            pedalDown = down;
        }
    };

    updatePedal();

    auto* left = buffer.getWritePointer (0, fc.bufferStartSample);
    auto* right = numChannels > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr;

    // Mono out gets the two sides summed and halved.
    auto renderTo = [&] (int from, int to)
    {
        if (to <= from)
            return;

        if (right != nullptr)
        {
            model.process (left + from, right + from, to - from);
        }
        else
        {
            // Rare enough (a mono track) that a stack buffer in pieces will do.
            float l[256], r[256];

            while (from < to)
            {
                const int n = juce::jmin (256, to - from);
                std::fill_n (l, n, 0.0f);
                std::fill_n (r, n, 0.0f);
                model.process (l, r, n);

                for (int i = 0; i < n; ++i)
                    left[from + i] = 0.5f * (l[i] + r[i]);

                from += n;
            }
        }
    };

    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                renderTo (pos, at);
                pos = at;
            }

            if (m.isNoteOn())
            {
                model.noteOn (m.getNoteNumber(), m.getFloatVelocity());
                setKey (m.getNoteNumber(), true);
            }
            else if (m.isNoteOff())
            {
                model.noteOff (m.getNoteNumber());
                setKey (m.getNoteNumber(), false);
            }
            else if (m.isSustainPedalOn() || m.isSustainPedalOff())
            {
                midiPedal = m.isSustainPedalOn();
                updatePedal();
            }
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                model.allNotesOff();
                keysDown[0] = keysDown[1] = 0;
                pedalDown = false;
                midiPedal = false;
                updatePedal();
            }
        }
    }

    renderTo (pos, fc.bufferNumSamples);
    voiceCount = model.getVoiceCount();

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
