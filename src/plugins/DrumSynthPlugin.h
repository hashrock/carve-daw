#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// An 808-style drum machine, as an internal instrument.
//
// Deliberately small. Anyone who wants a deep drum synth will reach for a
// plugin; this is here so a song can have a kick, snare, clap, hats, toms and
// a cowbell that travel with it, sound like *the* drum machine, and never need
// a sample folder or a plugin scan. So: one voice per drum, the way the
// original had one circuit per drum, every voice a one-shot, and the knobs
// kept to what changes the feel -- the kick gets five, most drums get one, a
// few get none.
//
// The hi-hat is the one place accuracy was worth the code. Its metallic tone
// comes from six square waves at the 808's own frequencies, summed, band-
// passed high and high-passed again, which is what gives it that sizzle a
// filtered-noise hat never has. Closed and open are the same voice with two
// decay times, so the closed hat chokes the open one exactly as on the
// machine.
//
// Notes follow the General MIDI kit where it has the drum, so a pattern drawn
// against this generator reads the same anywhere else:
//
//   36 kick   37 rim   38/40 snare   39 clap   41/43 low tom   42/44 closed hat
//   45/47 mid tom   46 open hat   48/50 high tom   56 cowbell
class DrumSynthPlugin : public te::Plugin
{
public:
    explicit DrumSynthPlugin (te::PluginCreationInfo);
    ~DrumSynthPlugin() override;

    static const char* getPluginName()  { return NEEDS_TRANS ("808 Drums"); }
    static const char* xmlTypeName;

    juce::String getName() const override             { return "808 Drums"; }
    juce::String getPluginType() override             { return xmlTypeName; }
    juce::String getShortName (int) override          { return "808"; }
    juce::String getSelectableDescription() override  { return TRANS ("808 Drums"); }

    int getNumOutputChannelsGivenInputs (int numInputChannels) override  { return juce::jmin (numInputChannels, 2); }
    bool takesMidiInput() override                    { return true; }
    bool takesAudioInput() override                   { return false; }
    bool isSynth() override                           { return true; }
    bool producesAudioWhenNoAudioInput() override     { return true; }

    void initialise (const te::PluginInitialisationInfo&) override;
    void deinitialise() override;
    void applyToBuffer (const te::PluginRenderContext&) override;

    void restorePluginStateFromValueTree (const juce::ValueTree&) override;

    // The drums, in the order the editor lists them and the pads trigger them.
    enum class Drum { kick, rim, snare, clap, closedHat, openHat, lowTom, midTom, highTom, cowbell };
    static constexpr int numDrums = 10;

    static int getNoteForDrum (Drum);
    static const char* getDrumName (Drum);

    // What a drum is doing, for the editor's pads to light from. The synth is
    // the only thing that knows: its voices are where a hit turns into sound,
    // whether the note came from a clip or from a pad being clicked.
    //
    // A hit count as well as the flag, for the same reason the note monitor in
    // front of a drum kit keeps one: a closed hat is over between two timer
    // ticks, and a counter that moved says there was a hit where the flag has
    // already cleared. Written on the audio thread, read from the message
    // thread, nothing locked.
    struct DrumActivity
    {
        uint32_t hits = 0;       // triggers so far; wraps, compare for change only
        bool sounding = false;   // a voice is still ringing this drum
    };

    DrumActivity getActivity (Drum) const;

    // Kick: pitch, length, how far the pitch falls in, how hard it is pushed,
    // and how much click sits on the front of it. Everything else gets a decay
    // at most -- and every drum gets a level, because balancing the kit
    // against itself is the one adjustment nobody should have to leave the
    // instrument for.
    juce::CachedValue<float> kickTuneValue, kickDecayValue, kickSweepValue, kickDriveValue, kickClickValue,
                             snareSnappyValue, snareDecayValue, clapDecayValue,
                             closedHatDecayValue, openHatDecayValue, tomDecayValue,
                             kickLevelValue, rimLevelValue, snareLevelValue, clapLevelValue,
                             closedHatLevelValue, openHatLevelValue, tomLevelValue, cowbellLevelValue;
    te::AutomatableParameter::Ptr kickTune, kickDecay, kickSweep, kickDrive, kickClick,
                                  snareSnappy, snareDecay, clapDecay,
                                  closedHatDecay, openHatDecay, tomDecay,
                                  kickLevel, rimLevel, snareLevel, clapLevel,
                                  closedHatLevel, openHatLevel, tomLevel, cowbellLevel;

private:
    // Zavalishin's trapezoidal state-variable filter: stable at any cutoff,
    // which the hat needs -- its band sits at 10kHz, where the textbook
    // Chamberlin form has already blown up.
    struct SVF
    {
        void set (double fc, double q, double sampleRate) noexcept
        {
            const auto g = std::tan (juce::MathConstants<double>::pi * juce::jmin (fc, sampleRate * 0.49) / sampleRate);
            k = 1.0 / q;
            a1 = 1.0 / (1.0 + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void reset() noexcept  { ic1 = ic2 = 0.0; }

        // Returns low/band/high for one sample; the caller picks.
        void process (double in, double& low, double& band, double& high) noexcept
        {
            const auto v3 = in - ic2;
            const auto v1 = a1 * ic1 + a2 * v3;
            const auto v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0 * v1 - ic1;
            ic2 = 2.0 * v2 - ic2;
            low = v2;
            band = v1;
            high = in - k * v1 - v2;
        }

        double k = 1.0, a1 = 0.0, a2 = 0.0, a3 = 0.0, ic1 = 0.0, ic2 = 0.0;
    };

    // One drum circuit. Which drum it is decides what render() does with the
    // same handful of oscillators, envelopes and filters.
    struct Voice
    {
        Drum drum = Drum::kick;
        bool active = false;
        double t = 0.0;                    // seconds since the hit
        double gain = 1.0;                 // velocity
        double amp = 0.0, ampCoef = 0.0;   // exponential decay, per sample
        double pitchEnv = 0.0, pitchCoef = 0.0;
        double burst = 0.0, burstCoef = 0.0;   // a second, faster decay (noise, click)
        double decaySeconds = 0.3;
        double baseFreq = 50.0, sweep = 0.0, drive = 0.0, click = 0.25, snappy = 0.6;
        std::array<double, 6> phase {};
        SVF filterA, filterB;
    };

    // Copies what the voices are doing into the atomics getActivity reads.
    void publishActivity();

    void trigger (Drum, float velocity);
    void render (float* out, int numSamples);
    float renderVoiceSample (Voice&);
    void killAll();

    double noise() noexcept;

    std::array<Voice, numDrums> voices;
    std::array<std::atomic<uint32_t>, numDrums> hitCounts;
    std::array<std::atomic<bool>, numDrums> soundingDrums;
    double sampleRate = 44100.0;
    uint32_t noiseState = 0x9e3779b9u;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DrumSynthPlugin)
};

} // namespace carve::plugins
