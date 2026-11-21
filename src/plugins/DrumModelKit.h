#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <vector>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "DrumModel.h"

namespace carve::plugins::drummodel
{

// The pieces of the kit, by General MIDI note (as the PhysKit live player maps
// them): 36 kick, 38 snare, 40 rimshot, 50 / 48 / 45 the 10", 12" and 16"
// toms, 42 / 46 / 44 hi-hat closed / open / pedal, 49 / 52 crash / choke,
// 51 / 53 ride / bell. Anything else is silent.
enum class Piece { none, kick, snare, rimshot, tomHigh, tomMid, tomFloor,
                   hatClosed, hatOpen, hatPedal, crash, crashChoke, ride, rideBell };

Piece pieceForNote (int note);
const char* pieceName (Piece);

// The stick (or beater) speed a MIDI velocity (0..1) strikes at, in m/s:
// the sample kit's ranges, exponential in the velocity, as live.html.
double tomSpeed (double velocity);      // 0.4 .. 8
double snareSpeed (double velocity);    // 0.35 .. 9
double rimshotSpeed (double velocity);  // 2.5 .. 12
double kickSpeed (double velocity);     // 0.5 .. 6

// Every setting, in the units the knobs show (live.html's sliders).
struct Settings
{
    struct Drum
    {
        double tune = 0;    // semitones
        double mute = 0;    // log2 of the factor on the decay rates
        double glide = 1;   // factor on the tension modulation (pitch glide)
        double stick = 0;   // log2 of the factor on the contact stiffness
    };

    std::array<Drum, 3> toms;   // 10", 12", 16"
    Drum kick { 0, 2.321928094887362, 1, 0 };   // mute x5: the model has no blanket
    Drum snare;
    double snareWires = 1;      // the strainer: x the wires' preload; off below 0.03
    bool allRimshots = false;   // note 38 plays rimshots too

    struct Cymbal { double tune = 0, level = 1; };
    Cymbal hihat, crash, ride { 0, 0.5 };

    double close = 1, overhead = 1, room = 0.2;
    double snareLevel = 1, kickLevel = 1, volume = 0.8;
};

//==============================================================================
// The whole kit at the host's rate: the five modelled drums each at its
// model's rate, the cymbals as samples of the dry kit (their physics runs
// far slower than real time), a synthetic room and a limiter, as live.html
// mixes them. load() takes a while (it settles the snare's wires and decodes
// the cymbals) and is not for the audio thread; after it and prepare(),
// nothing allocates on the audio thread.
//
// The drums are played from a cache of hits: each (drum, plain or rimshot,
// velocity layer) is rendered once, from rest, on a thread of the kit's own
// with engines of its own, and played back as a sample after that -- a stroke
// costs almost nothing, and a hit on a ringing drum simply overlaps the
// last one. Only what is played gets rendered (a stroke the cache does not
// have yet is solved live, as the drums were before, while the thread
// renders it for next time); turning a drum's knobs throws its hits away and
// renders the ones it had again. Nothing is rendered at load.
class Kit
{
public:
    Kit();
    ~Kit();

    // From the data compiled in (assets/instruments/modelled-drums). False if
    // anything is missing.
    bool load();
    bool isLoaded() const   { return loaded; }

    void prepare (double hostRate, int maxBlockSize);
    void setSettings (const Settings&);   // audio thread: applies what changed

    // At the current point of the stream.
    void hit (Piece, double velocity);
    void allNotesOff();   // stops every sound at once

    // Writes n samples (host rate).
    void process (float* left, float* right, int n);

    // For the editor: how busy each drum is (active modes; 0 = at rest) and
    // the cymbal voices playing.
    struct Activity { int kick = 0, snare = 0, toms[3] {}, cymbals = 0; };
    Activity getActivity() const;

    // Tests: the engines alone.
    ModalDrum& getTom (int i)   { return toms[(size_t) i]; }
    ModalDrum& getKick()        { return kick; }
    Snare& getSnare()           { return snare; }

    // Whether a hit would come from the cache now (from the audio thread, or
    // the thread that calls process()). Off, every hit is solved live.
    bool isCached (Piece, double velocity);
    void setCacheEnabled (bool shouldUse)   { cacheEnabled = shouldUse; }

    // The bytes the cache holds now (any thread; for the curious).
    std::size_t getCacheBytes() const;

    // Strokes to have ready before they are played -- the song's notes, so
    // that playing it never waits on the live engines (any thread, once
    // loaded). Kept through knob turns: they are rendered again as the drum
    // is then. A stroke played is rendered whether kept or not.
    static constexpr int numStrokes = 5 * 2 * 8;   // numDrums * numVariants * numLayers
    using Strokes = std::bitset<(size_t) numStrokes>;
    static int strokeIndex (Piece, double velocity, bool allRimshots);   // -1 for a cymbal or nothing
    void keepStrokes (const Strokes&);

    struct CymbalSample;
    struct HitSample;
    class Renderer;

    static constexpr int numDrums = 5;      // the 10", 12", 16" toms, the kick, the snare
    static constexpr int numLayers = 8;     // velocity layers, 0 .. 1 evenly
    static constexpr int numVariants = 2;   // plain, rimshot (the snare)
    static_assert (numStrokes == numDrums * numVariants * numLayers);

private:
    struct CymbalVoice
    {
        const CymbalSample* sample = nullptr;
        double pos = 0, rate = 1;
        float gain = 1, level = 1;
        bool choking = false;
        int group = 0, offBy = 0, bus = 0;
    };

    struct HitVoice
    {
        HitSample* sample = nullptr;
        int pos = 0;
        float gain = 1;
    };

    // One stroke of one drum. The audio thread owns `current`; the renderer
    // hands a new sample over through `incoming` and is asked for one by
    // setting `wanted` to the drum's generation (a hit), or by `keep`.
    struct Slot
    {
        std::atomic<HitSample*> incoming { nullptr };
        std::atomic<std::uint32_t> wanted { 0 };
        std::atomic<bool> keep { false };
        HitSample* current = nullptr;
    };

    // What the renderer needs to render a drum as it is set: the engine's
    // setParam values, published with a sequence lock. An even `seq` is the
    // drum's generation; it changes whenever a knob of the drum does.
    struct DrumState
    {
        std::atomic<std::uint32_t> seq { 0 };
        std::array<std::atomic<double>, 6> params {};
    };

    void renderModelBus (int numSource);
    void renderCymbals (float* l, float* r, int n);
    void renderHits (int drumBus, float* l, float* r, int n);   // drumBus 0: the 48kHz bus, 1: the kick's
    void startCymbal (Piece, double velocity);
    void applySettings (bool force);
    bool playCached (int drum, int variant, double velocity);
    void publishDrum (int drum, const std::array<double, 6>&);
    void retire (HitSample*);
    void stopHitVoices();
    Slot& slot (int drum, int variant, int layer)   { return slots[(size_t) ((drum * numVariants + variant) * numLayers + layer)]; }

    bool loaded = false;
    std::array<ModalDrum, 3> toms;
    ModalDrum kick;
    Snare snare;
    std::shared_ptr<const std::vector<CymbalSample>> cymbals;
    std::array<CymbalVoice, 24> voices;
    juce::Random random;

    bool cacheEnabled = true;
    std::array<Slot, (size_t) (numDrums * numVariants * numLayers)> slots;
    std::array<DrumState, (size_t) numDrums> drumStates;
    std::array<HitVoice, 32> hitVoices;
    std::unique_ptr<Renderer> renderer;

    Settings settings, applied;
    bool settingsChanged = true;

    double hostRate = 48000, busRate = 48000, kickRate = 16000;
    int maxBlock = 512;
    Resampler busResampler, kickResampler;
    std::vector<float> busL, busR, snareBuf, kickL, kickR, tmpL, tmpR, roomL, roomR;

    juce::dsp::Convolution roomConvolution { juce::dsp::Convolution::NonUniform { 512 } };
    double limiterEnv = 0;   // dB of gain reduction
    double limAttack = 0, limRelease = 0;
};

} // namespace carve::plugins::drummodel
