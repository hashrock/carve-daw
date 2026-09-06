#include "DrumSynthPlugin.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier kickTuneId ("kickTune"), kickDecayId ("kickDecay"),
                           kickSweepId ("kickSweep"), kickDriveId ("kickDrive"),
                           snareSnappyId ("snareSnappy"), snareDecayId ("snareDecay"),
                           clapDecayId ("clapDecay"),
                           closedHatDecayId ("closedHatDecay"), openHatDecayId ("openHatDecay"),
                           tomDecayId ("tomDecay"),
                           kickLevelId ("kickLevel"), rimLevelId ("rimLevel"),
                           snareLevelId ("snareLevel"), clapLevelId ("clapLevel"),
                           closedHatLevelId ("closedHatLevel"), openHatLevelId ("openHatLevel"),
                           tomLevelId ("tomLevel"), cowbellLevelId ("cowbellLevel");

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    // The six oscillators behind the 808's hat and cymbal, at the frequencies
    // the service manual gives. Not harmonically related to each other, which
    // is the whole point: their sum is a clangorous, pitchless buzz, and what
    // the filters keep of it is the sizzle.
    constexpr std::array<double, 6> hatFrequencies { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };

    // ...and the two behind the cowbell.
    constexpr std::array<double, 2> cowbellFrequencies { 587.0, 845.0 };

    inline double sine (double phase) noexcept   { return std::sin (phase * twoPi); }
    inline double square (double phase) noexcept { return phase < 0.5 ? 1.0 : -1.0; }

    inline void advance (double& phase, double freq, double sampleRate) noexcept
    {
        phase += freq / sampleRate;
        if (phase >= 1.0)
            phase -= std::floor (phase);
    }

    // Per-sample multiplier for an exponential decay that falls to 1/e in
    // `seconds`, so every envelope here is one multiply a sample.
    inline double decayCoefficient (double seconds, double sampleRate) noexcept
    {
        return std::exp (-1.0 / (juce::jmax (0.001, seconds) * sampleRate));
    }

    inline double softClip (double x, double gain) noexcept
    {
        return std::tanh (x * gain) / std::tanh (juce::jmax (1.0, gain));
    }

    juce::String percentText (float v)   { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }
    float percentValue (const juce::String& s)  { return s.getFloatValue() * 0.01f; }

    juce::String secondsText (float v)
    {
        return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms"
                        : juce::String (v, 2) + " s";
    }

    float secondsValue (const juce::String& s)
    {
        const auto v = s.getFloatValue();
        return s.containsIgnoreCase ("ms") ? v * 0.001f : v;
    }
} // namespace

const char* DrumSynthPlugin::xmlTypeName = "carveDrumSynth";

//==============================================================================
int DrumSynthPlugin::getNoteForDrum (Drum drum)
{
    switch (drum)
    {
        case Drum::kick:      return 36;
        case Drum::rim:       return 37;
        case Drum::snare:     return 38;
        case Drum::clap:      return 39;
        case Drum::lowTom:    return 41;
        case Drum::closedHat: return 42;
        case Drum::midTom:    return 45;
        case Drum::openHat:   return 46;
        case Drum::highTom:   return 48;
        case Drum::cowbell:   return 56;
    }

    return 36;
}

const char* DrumSynthPlugin::getDrumName (Drum drum)
{
    switch (drum)
    {
        case Drum::kick:      return "Kick";
        case Drum::rim:       return "Rim";
        case Drum::snare:     return "Snare";
        case Drum::clap:      return "Clap";
        case Drum::closedHat: return "CH";
        case Drum::openHat:   return "OH";
        case Drum::lowTom:    return "Low Tom";
        case Drum::midTom:    return "Mid Tom";
        case Drum::highTom:   return "High Tom";
        case Drum::cowbell:   return "Cowbell";
    }

    return "";
}

//==============================================================================
DrumSynthPlugin::DrumSynthPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    kickTuneValue.referTo (state, kickTuneId, um, 50.0f);
    kickDecayValue.referTo (state, kickDecayId, um, 0.5f);
    kickSweepValue.referTo (state, kickSweepId, um, 0.5f);
    kickDriveValue.referTo (state, kickDriveId, um, 0.2f);
    snareSnappyValue.referTo (state, snareSnappyId, um, 0.6f);
    snareDecayValue.referTo (state, snareDecayId, um, 0.05f);
    clapDecayValue.referTo (state, clapDecayId, um, 0.05f);
    closedHatDecayValue.referTo (state, closedHatDecayId, um, 0.06f);
    openHatDecayValue.referTo (state, openHatDecayId, um, 0.5f);
    tomDecayValue.referTo (state, tomDecayId, um, 0.35f);

    kickLevelValue.referTo (state, kickLevelId, um, 0.0f);
    rimLevelValue.referTo (state, rimLevelId, um, 0.0f);
    snareLevelValue.referTo (state, snareLevelId, um, 0.0f);
    clapLevelValue.referTo (state, clapLevelId, um, 0.0f);
    closedHatLevelValue.referTo (state, closedHatLevelId, um, 0.0f);
    openHatLevelValue.referTo (state, openHatLevelId, um, 0.0f);
    tomLevelValue.referTo (state, tomLevelId, um, 0.0f);
    cowbellLevelValue.referTo (state, cowbellLevelId, um, 0.0f);

    auto hz = [] (float v) { return juce::String (juce::roundToInt (v)) + " Hz"; };
    auto hzValue = [] (const juce::String& s) { return s.getFloatValue(); };

    kickTune  = addParam ("kickTune",  TRANS ("Kick Tune"),  { 30.0f, 90.0f }, hz, hzValue);
    kickDecay = addParam ("kickDecay", TRANS ("Kick Decay"), { 0.1f, 2.0f, 0.0f, 0.5f }, secondsText, secondsValue);
    kickSweep = addParam ("kickSweep", TRANS ("Kick Sweep"), { 0.0f, 1.0f }, percentText, percentValue);
    kickDrive = addParam ("kickDrive", TRANS ("Kick Drive"), { 0.0f, 1.0f }, percentText, percentValue);

    snareSnappy = addParam ("snareSnappy", TRANS ("Snare Snappy"), { 0.0f, 1.0f }, percentText, percentValue);
    snareDecay  = addParam ("snareDecay",  TRANS ("Snare Decay"),  { 0.05f, 0.6f, 0.0f, 0.6f }, secondsText, secondsValue);
    clapDecay   = addParam ("clapDecay",   TRANS ("Clap Decay"),   { 0.05f, 0.8f, 0.0f, 0.6f }, secondsText, secondsValue);

    closedHatDecay = addParam ("closedHatDecay", TRANS ("Closed Hat Decay"), { 0.02f, 0.3f, 0.0f, 0.5f }, secondsText, secondsValue);
    openHatDecay   = addParam ("openHatDecay",   TRANS ("Open Hat Decay"),   { 0.1f, 1.5f, 0.0f, 0.5f }, secondsText, secondsValue);
    tomDecay       = addParam ("tomDecay",       TRANS ("Tom Decay"),        { 0.1f, 1.0f, 0.0f, 0.6f }, secondsText, secondsValue);

    auto db = [] (float v) { return juce::String (v, 1) + " dB"; };
    auto dbValue = [] (const juce::String& s) { return s.getFloatValue(); };
    const juce::NormalisableRange<float> levelRange { -24.0f, 6.0f };

    kickLevel      = addParam ("kickLevel",      TRANS ("Kick Level"),       levelRange, db, dbValue);
    rimLevel       = addParam ("rimLevel",       TRANS ("Rim Level"),        levelRange, db, dbValue);
    snareLevel     = addParam ("snareLevel",     TRANS ("Snare Level"),      levelRange, db, dbValue);
    clapLevel      = addParam ("clapLevel",      TRANS ("Clap Level"),       levelRange, db, dbValue);
    closedHatLevel = addParam ("closedHatLevel", TRANS ("Closed Hat Level"), levelRange, db, dbValue);
    openHatLevel   = addParam ("openHatLevel",   TRANS ("Open Hat Level"),   levelRange, db, dbValue);
    tomLevel       = addParam ("tomLevel",       TRANS ("Tom Level"),        levelRange, db, dbValue);
    cowbellLevel   = addParam ("cowbellLevel",   TRANS ("Cowbell Level"),    levelRange, db, dbValue);

    kickTune->attachToCurrentValue (kickTuneValue);
    kickDecay->attachToCurrentValue (kickDecayValue);
    kickSweep->attachToCurrentValue (kickSweepValue);
    kickDrive->attachToCurrentValue (kickDriveValue);
    snareSnappy->attachToCurrentValue (snareSnappyValue);
    snareDecay->attachToCurrentValue (snareDecayValue);
    clapDecay->attachToCurrentValue (clapDecayValue);
    closedHatDecay->attachToCurrentValue (closedHatDecayValue);
    openHatDecay->attachToCurrentValue (openHatDecayValue);
    tomDecay->attachToCurrentValue (tomDecayValue);
    kickLevel->attachToCurrentValue (kickLevelValue);
    rimLevel->attachToCurrentValue (rimLevelValue);
    snareLevel->attachToCurrentValue (snareLevelValue);
    clapLevel->attachToCurrentValue (clapLevelValue);
    closedHatLevel->attachToCurrentValue (closedHatLevelValue);
    openHatLevel->attachToCurrentValue (openHatLevelValue);
    tomLevel->attachToCurrentValue (tomLevelValue);
    cowbellLevel->attachToCurrentValue (cowbellLevelValue);

    for (int i = 0; i < numDrums; ++i)
        voices[(size_t) i].drum = (Drum) i;
}

DrumSynthPlugin::~DrumSynthPlugin()
{
    notifyListenersOfDeletion();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void DrumSynthPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;
    killAll();
}

void DrumSynthPlugin::deinitialise()
{
}

void DrumSynthPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, kickTuneValue, kickDecayValue, kickSweepValue, kickDriveValue,
                                      snareSnappyValue, snareDecayValue, clapDecayValue,
                                      closedHatDecayValue, openHatDecayValue, tomDecayValue,
                                      kickLevelValue, rimLevelValue, snareLevelValue, clapLevelValue,
                                      closedHatLevelValue, openHatLevelValue, tomLevelValue,
                                      cowbellLevelValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

//==============================================================================
void DrumSynthPlugin::killAll()
{
    for (auto& voice : voices)
    {
        voice.active = false;
        voice.filterA.reset();
        voice.filterB.reset();
    }
}

double DrumSynthPlugin::noise() noexcept
{
    // xorshift32: plenty for noise, and no locks or allocation on the audio
    // thread the way juce::Random would bring.
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;
    return (double) noiseState / 2147483648.0 - 1.0;
}

void DrumSynthPlugin::trigger (Drum drum, float velocity)
{
    // Both hats are one circuit: hitting either restarts it, which is how the
    // closed hat chokes the open one.
    const auto slot = (drum == Drum::openHat) ? Drum::closedHat : drum;
    auto& v = voices[(size_t) slot];

    v.drum = drum;
    v.active = true;
    v.t = 0.0;
    // Velocity and the drum's own level, folded into one gain at the hit.
    const auto& level = drum == Drum::kick      ? kickLevel
                : drum == Drum::rim       ? rimLevel
                : drum == Drum::snare     ? snareLevel
                : drum == Drum::clap      ? clapLevel
                : drum == Drum::closedHat ? closedHatLevel
                : drum == Drum::openHat   ? openHatLevel
                : drum == Drum::cowbell   ? cowbellLevel
                                          : tomLevel;

    v.gain = (0.25 + 0.75 * (double) velocity) * te::dbToGain (level->getCurrentValue());
    v.amp = 1.0;
    v.pitchEnv = 1.0;
    v.burst = 1.0;

    // The oscillators keep running between hits (as the circuits do), so only
    // the pitched drums, whose attack depends on where the wave starts, reset.
    auto restartPhases = [&v] { v.phase.fill (0.0); };

    switch (drum)
    {
        case Drum::kick:
            v.baseFreq = kickTune->getCurrentValue();
            v.sweep = kickSweep->getCurrentValue();
            v.drive = kickDrive->getCurrentValue();
            v.decaySeconds = kickDecay->getCurrentValue();
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.pitchCoef = decayCoefficient (0.045, sampleRate);    // the drop-in
            v.burstCoef = decayCoefficient (0.004, sampleRate);    // the click
            restartPhases();
            break;

        case Drum::snare:
            v.snappy = snareSnappy->getCurrentValue();
            v.decaySeconds = snareDecay->getCurrentValue();
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);          // the noise
            v.burstCoef = decayCoefficient (v.decaySeconds * 0.55, sampleRate); // the two-tone body, shorter
            v.pitchCoef = decayCoefficient (0.02, sampleRate);
            v.filterA.set (1200.0, 0.7, sampleRate);                              // high-pass on the noise
            restartPhases();
            break;

        case Drum::clap:
            v.burst = 0.0;   // amp is the only envelope here; see the idle test in renderVoiceSample
            v.decaySeconds = clapDecay->getCurrentValue();
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.filterA.set (1100.0, 1.3, sampleRate);                              // the band the clap lives in
            break;

        case Drum::closedHat:
        case Drum::openHat:
            v.burst = 0.0;   // likewise
            v.decaySeconds = (drum == Drum::openHat ? openHatDecay : closedHatDecay)->getCurrentValue();
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.filterA.set (10000.0, 1.5, sampleRate);                             // band-pass: the sizzle
            v.filterB.set (7500.0, 0.7, sampleRate);                              // high-pass: and nothing below it
            break;

        case Drum::lowTom:
        case Drum::midTom:
        case Drum::highTom:
        {
            const double base = drum == Drum::lowTom ? 80.0 : drum == Drum::midTom ? 120.0 : 165.0;
            const double length = drum == Drum::lowTom ? 1.0 : drum == Drum::midTom ? 0.85 : 0.7;
            v.baseFreq = base;
            v.decaySeconds = tomDecay->getCurrentValue() * length;
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.pitchCoef = decayCoefficient (0.06, sampleRate);
            v.burstCoef = decayCoefficient (0.015, sampleRate);                   // the 808 tom's puff of noise
            v.filterA.set (2000.0, 0.7, sampleRate);
            restartPhases();
            break;
        }

        case Drum::rim:
            v.baseFreq = 480.0;
            v.decaySeconds = 0.05;
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.pitchCoef = decayCoefficient (0.005, sampleRate);
            v.burstCoef = decayCoefficient (0.006, sampleRate);
            v.filterA.set (3000.0, 0.7, sampleRate);
            restartPhases();
            break;

        case Drum::cowbell:
            v.decaySeconds = 0.32;
            v.ampCoef = decayCoefficient (v.decaySeconds, sampleRate);
            v.burstCoef = decayCoefficient (0.02, sampleRate);                    // the clank before the ring
            v.filterA.set (2640.0, 1.0, sampleRate);
            break;
    }
}

float DrumSynthPlugin::renderVoiceSample (Voice& v)
{
    double out = 0.0;
    double low = 0.0, band = 0.0, high = 0.0;

    switch (v.drum)
    {
        case Drum::kick:
        {
            // A sine that starts up to five times too high and drops to pitch
            // in about 50ms -- that drop *is* the 808 kick's attack -- plus a
            // click, then pushed into a soft clip by Drive.
            const auto freq = v.baseFreq * (1.0 + 5.0 * v.sweep * v.pitchEnv);
            advance (v.phase[0], freq, sampleRate);
            const auto body = sine (v.phase[0]) * v.amp + 0.3 * v.burst;
            out = softClip (body, 1.0 + 8.0 * v.drive) * 1.4;   // the loudest drum in the kit
            v.amp *= v.ampCoef;
            v.pitchEnv *= v.pitchCoef;
            v.burst *= v.burstCoef;
            break;
        }

        case Drum::snare:
        {
            // Two bridged-T tones (180 and 330Hz, with a little pitch drop)
            // for the shell, high-passed noise for the wires. Snappy is how
            // much of the noise gets in.
            const auto bend = 1.0 + 0.6 * v.pitchEnv;
            advance (v.phase[0], 180.0 * bend, sampleRate);
            advance (v.phase[1], 330.0 * bend, sampleRate);
            const auto body = (sine (v.phase[0]) + 0.7 * sine (v.phase[1])) * v.burst * 0.55;
            v.filterA.process (noise(), low, band, high);
            out = body + high * v.amp * v.snappy * 0.9;
            v.amp *= v.ampCoef;
            v.burst *= v.burstCoef;
            v.pitchEnv *= v.pitchCoef;
            break;
        }

        case Drum::clap:
        {
            // Band-passed noise under three quick bursts 10ms apart, then a
            // tail -- the 808 fakes several hands with a stepped envelope, and
            // it is those steps that read as a clap rather than a snare.
            double env;

            if (v.t < 0.03)
            {
                env = std::exp (-std::fmod (v.t, 0.01) / 0.0035);
            }
            else
            {
                v.amp *= v.ampCoef;
                env = v.amp * 0.9;
            }

            v.filterA.process (noise(), low, band, high);
            out = band * env * 2.5;
            break;
        }

        case Drum::closedHat:
        case Drum::openHat:
        {
            // Six squares summed, band-passed at 10kHz, high-passed at 7.5kHz,
            // and a plain decay. Nothing else: this is the circuit.
            double sum = 0.0;

            for (size_t i = 0; i < hatFrequencies.size(); ++i)
            {
                advance (v.phase[i], hatFrequencies[i], sampleRate);
                sum += square (v.phase[i]);
            }

            v.filterA.process (sum / 6.0, low, band, high);
            v.filterB.process (band, low, band, high);
            out = high * v.amp * 3.6;
            v.amp *= v.ampCoef;
            break;
        }

        case Drum::lowTom:
        case Drum::midTom:
        case Drum::highTom:
        {
            // A sine with a gentler pitch drop than the kick, and the little
            // puff of high-passed noise the 808 toms open with.
            advance (v.phase[0], v.baseFreq * (1.0 + 1.0 * v.pitchEnv), sampleRate);
            v.filterA.process (noise(), low, band, high);
            out = sine (v.phase[0]) * v.amp * 0.85 + high * v.burst * 0.25;
            v.amp *= v.ampCoef;
            v.pitchEnv *= v.pitchCoef;
            v.burst *= v.burstCoef;
            break;
        }

        case Drum::rim:
        {
            // A very short, hard-clipped tone with a tick of noise on top.
            advance (v.phase[0], v.baseFreq * (1.0 + 0.8 * v.pitchEnv), sampleRate);
            v.filterA.process (noise(), low, band, high);
            out = softClip (sine (v.phase[0]) * v.amp, 4.0) * 0.7 + high * v.burst * 0.6;
            v.amp *= v.ampCoef;
            v.pitchEnv *= v.pitchCoef;
            v.burst *= v.burstCoef;
            break;
        }

        case Drum::cowbell:
        {
            // Two squares a fifth-and-a-bit apart, band-passed for the clank,
            // with a bit of the raw pair kept so the pitch stays audible.
            double sum = 0.0;

            for (size_t i = 0; i < cowbellFrequencies.size(); ++i)
            {
                advance (v.phase[i], cowbellFrequencies[i], sampleRate);
                sum += square (v.phase[i]);
            }

            sum *= 0.5;
            v.filterA.process (sum, low, band, high);
            const auto env = 0.6 * v.burst + 0.4 * v.amp;
            out = (band * 0.8 + sum * 0.25) * env * 1.5;
            v.amp *= v.ampCoef;
            v.burst *= v.burstCoef;
            break;
        }
    }

    v.t += 1.0 / sampleRate;

    // Every envelope here decays for good, so the voice is over once the
    // slowest of them is below hearing. The clap's stepped start keeps amp at
    // 1 for its first 30ms, which is why t is checked too.
    if (v.amp < 1.0e-4 && v.burst < 1.0e-4)
        v.active = false;

    return (float) (out * v.gain);
}

void DrumSynthPlugin::render (float* out, int numSamples)
{
    for (auto& voice : voices)
    {
        if (! voice.active)
            continue;

        for (int i = 0; i < numSamples && voice.active; ++i)
            out[i] += renderVoiceSample (voice);
    }
}

void DrumSynthPlugin::applyToBuffer (const te::PluginRenderContext& fc)
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
        killAll();

    // Render mono into channel 0, hit by hit: the stretch up to each note-on
    // first, then the trigger, so a hit lands on its own sample rather than at
    // the start of the block.
    auto* out = buffer.getWritePointer (0, fc.bufferStartSample);
    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            if (! m.isNoteOn())
                continue;

            const int at = juce::jlimit (0, fc.bufferNumSamples,
                                         (int) m.getTimeStamp() - fc.bufferStartSample);

            if (at > pos)
            {
                render (out + pos, at - pos);
                pos = at;
            }

            const int note = m.getNoteNumber();
            const auto velocity = m.getFloatVelocity();

            switch (note)
            {
                case 36:            trigger (Drum::kick, velocity); break;
                case 37:            trigger (Drum::rim, velocity); break;
                case 38: case 40:   trigger (Drum::snare, velocity); break;
                case 39:            trigger (Drum::clap, velocity); break;
                case 41: case 43:   trigger (Drum::lowTom, velocity); break;
                case 42: case 44:   trigger (Drum::closedHat, velocity); break;
                case 45: case 47:   trigger (Drum::midTom, velocity); break;
                case 46:            trigger (Drum::openHat, velocity); break;
                case 48: case 50:   trigger (Drum::highTom, velocity); break;
                case 56:            trigger (Drum::cowbell, velocity); break;
                default: break;     // no drum on this key: silence, not a wrong drum
            }
        }
    }

    if (pos < fc.bufferNumSamples)
        render (out + pos, fc.bufferNumSamples - pos);

    // The same signal on both sides; the track's pan is where stereo comes in.
    if (numChannels > 1)
        buffer.copyFrom (1, fc.bufferStartSample, buffer, 0, fc.bufferStartSample, fc.bufferNumSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
