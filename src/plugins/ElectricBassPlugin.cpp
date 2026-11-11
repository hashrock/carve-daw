#include "ElectricBassPlugin.h"

#include <algorithm>
#include <mutex>

#include "InstrumentData.h"

namespace carve::plugins
{

namespace
{
    const juce::Identifier pickId ("pick"), muteId ("mute"), attackId ("attack"), attackEqId ("attackEq"),
                           toneId ("tone"), bodyId ("body"), buzzId ("buzz"), sustainId ("sustain"),
                           slideTimeId ("slideTime"), tapId ("tap"), tuneId ("tune"), levelId ("level"),
                           techniqueId ("technique"), monoId ("mono");

    constexpr int maxVoices = 10;
    constexpr int lowestNote = 12, highestNote = 96;

    // The prototype's timings: the crossfades of a hammer-on, of a slide's
    // start and of its landing, and how quickly a plucked note silences the
    // one before it on a mono bass.
    constexpr double hammerFade = 0.006, slideInFade = 0.003, arriveFade = 0.025;
    constexpr double monoCutSeconds = 0.012, releaseSeconds = 0.03;

    // How close to its end a ringing note still takes a legato move.
    constexpr double legatoTailSeconds = 0.08;

    const bass::Kit* loadKit()
    {
        static std::once_flag once;
        static std::unique_ptr<bass::Kit> kit;

        std::call_once (once, []
        {
            const auto blob = findInstrumentData ("electric-bass.bas");

            if (blob.data != nullptr)
                kit = bass::Kit::parse (blob.data, blob.size);

            if (kit == nullptr)
                juce::Logger::writeToLog ("Electric Bass: the kit did not load");
        });

        return kit.get();
    }

    juce::String signedText (float v, int decimals, const char* unit)
    {
        return (v > 0.0f ? "+" : "") + juce::String (v, decimals) + unit;
    }
} // namespace

//==============================================================================
// One voice, with the gain moves the prototype made with Web Audio ramps:
// a linear ramp to a target (a crossfade), an exponential release, and a
// delayed start (a slide's landing, waiting for the bend to arrive).
struct ElectricBassPlugin::PlayVoice
{
    bass::Voice voice;
    bool active = false;
    int note = -1;
    float velocity = 0.0f;

    double gv = 1.0;              // the level this voice is meant to sound at
    double gain = 0.0;
    double rampTarget = 0.0, rampStep = 0.0;
    int rampLeft = 0;
    bool dieAfterRamp = false;

    bool releasing = false;
    double releaseCoef = 1.0;

    int delay = 0;                // samples before this voice starts
    double arrivalFade = 0.0;     // the crossfade to make when it does

    // A slide holding its level up: a gain that grows by `boostCoefficient`
    // a sample for `boostLeft` samples, against the decay under it.
    double boost = 1.0, boostCoefficient = 1.0;
    int boostLeft = 0;

    bool tapOnly = false;
    PlayVoice* next = nullptr;      // a slide's pending landing
    PlayVoice* slideFrom = nullptr; // the bent voice a landing takes over from
    std::uint64_t age = 0;

    void rampTo (double target, double seconds, double fs, bool die)
    {
        rampTarget = target;
        rampLeft = juce::jmax (1, juce::roundToInt (seconds * fs));
        rampStep = (target - gain) / rampLeft;
        dieAfterRamp = die;
        releasing = false;
    }

    double getTau() const   { return voice.getPosition() / voice.getSampleRate(); }

    // The level for the next sample, false once the voice is over.
    bool stepGain()
    {
        if (boostLeft > 0)
        {
            boost *= boostCoefficient;
            --boostLeft;
        }

        if (rampLeft > 0)
        {
            gain += rampStep;

            if (--rampLeft == 0)
            {
                gain = rampTarget;

                if (dieAfterRamp)
                    return false;
            }
        }
        else if (releasing)
        {
            gain *= releaseCoef;

            if (gain < 1.0e-4)
                return false;
        }

        return true;
    }
};

//==============================================================================
const char* ElectricBassPlugin::xmlTypeName = "carveElectricBass";

const char* ElectricBassPlugin::getTechniqueName (Technique t)
{
    switch (t)
    {
        case Technique::pluck:  return "Pluck";
        case Technique::hammer: return "Hammer";
        case Technique::slide:  return "Slide";
    }

    return "";
}

const bass::Kit* ElectricBassPlugin::getKit()
{
    return loadKit();
}

ElectricBassPlugin::ElectricBassPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    auto um = getUndoManager();

    pickValue.referTo (state, pickId, um, 0.0f);
    muteValue.referTo (state, muteId, um, 0.0f);
    attackValue.referTo (state, attackId, um, 0.0f);
    attackEqValue.referTo (state, attackEqId, um, 0.5f);
    toneValue.referTo (state, toneId, um, 0.0f);
    bodyValue.referTo (state, bodyId, um, 0.0f);
    buzzValue.referTo (state, buzzId, um, 0.0f);
    sustainValue.referTo (state, sustainId, um, 1.0f);
    slideTimeValue.referTo (state, slideTimeId, um, 0.12f);
    tapValue.referTo (state, tapId, um, -14.0f);
    slideHoldValue.referTo (state, "slideHold", um, 0.8f);
    outFretsValue.referTo (state, "outFrets", um, 7.0f);
    tuneValue.referTo (state, tuneId, um, 0.0f);
    levelValue.referTo (state, levelId, um, 0.0f);
    techniqueValue.referTo (state, techniqueId, um, "hammer");
    fretlessValue.referTo (state, "fretless", um, false);
    monoValue.referTo (state, monoId, um, true);

    auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto percentValue = [] (const juce::String& s) { return s.getFloatValue() * 0.01f; };
    auto plain = [] (const juce::String& s) { return s.getFloatValue(); };

    pick = addParam ("pick", TRANS ("Pluck Position"), { 0.0f, 0.4f },
                     [] (float v) { return v < 0.03f ? juce::String ("as recorded")
                                                     : juce::String (juce::roundToInt (v * 100.0f)) + "% from bridge"; },
                     percentValue);
    mute = addParam ("mute", TRANS ("Palm Mute"), { 0.0f, 1.0f },
                     [percent] (float v) { return v <= 0.0f ? juce::String ("off") : percent (v); }, percentValue);
    attack = addParam ("attack", TRANS ("Attack"), { -30.0f, 12.0f },
                       [] (float v) { return signedText (v, 1, " dB"); }, plain);
    attackEq = addParam ("attackEq", TRANS ("Attack Evenness"), { 0.0f, 1.0f }, percent, percentValue);

    tone = addParam ("tone", TRANS ("Tone"), { -6.0f, 6.0f },
                     [] (float v) { return signedText (v, 1, " dB/oct"); }, plain);
    body = addParam ("body", TRANS ("Body"), { -12.0f, 12.0f },
                     [] (float v) { return signedText (v, 1, " dB"); }, plain);
    buzz = addParam ("buzz", TRANS ("Fret Buzz"), { -61.0f, 12.0f },
                     [] (float v) { return v <= -60.0f ? juce::String ("off") : signedText (v, 1, " dB"); }, plain);

    juce::NormalisableRange<float> sustainRange { 0.25f, 3.0f };
    sustainRange.setSkewForCentre (1.0f);
    sustain = addParam ("sustain", TRANS ("Sustain"), sustainRange,
                        [] (float v) { return juce::String::charToString (0x00d7) + juce::String (v, 2); }, plain);

    slideTime = addParam ("slideTime", TRANS ("Slide Time"), { 0.03f, 0.6f },
                          [] (float v) { return juce::String (juce::roundToInt (v * 1000.0f)) + " ms"; },
                          [] (const juce::String& s) { return s.getFloatValue() * 0.001f; });
    tap = addParam ("tap", TRANS ("Fret Tap"), { -40.0f, 0.0f },
                    [] (float v) { return v <= -40.0f ? juce::String ("off") : juce::String (v, 1) + " dB"; }, plain);

    slideHold = addParam ("slideHold", TRANS ("Slide Hold"), { 0.0f, 1.0f },
                          [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% held"; },
                          [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

    outFrets = addParam ("outFrets", TRANS ("Out Frets"), { 1.0f, 12.0f, 1.0f },
                         [] (float v) { return juce::String (juce::roundToInt (v)) + (juce::roundToInt (v) == 1 ? " fret" : " frets"); },
                         [] (const juce::String& s) { return s.getFloatValue(); });

    tune = addParam ("tune", TRANS ("Tune"), { -100.0f, 100.0f },
                     [] (float v) { return signedText (v, 0, " ct"); }, plain);
    level = addParam ("level", TRANS ("Level"), { -24.0f, 12.0f },
                      [] (float v) { return signedText (v, 1, " dB"); }, plain);

    ampDriveValue.referTo (state, "ampDrive", um, 0.3f);
    ampBlendValue.referTo (state, "ampBlend", um, 0.6f);
    ampBassValue.referTo (state, "ampBass", um, 0.0f);
    ampMidValue.referTo (state, "ampMid", um, 0.0f);
    ampMidFreqValue.referTo (state, "ampMidFreq", um, 700.0f);
    ampTrebleValue.referTo (state, "ampTreble", um, 0.0f);
    ampLevelValue.referTo (state, "ampLevel", um, 0.0f);
    ampOnValue.referTo (state, "ampOn", um, false);

    auto eqDb = [] (float v) { return signedText (v, 1, " dB"); };
    ampDrive = addParam ("ampDrive", TRANS ("Amp Drive"), { 0.0f, 1.0f }, percent, percentValue);
    ampBlend = addParam ("ampBlend", TRANS ("Amp Blend"), { 0.0f, 1.0f },
                         [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% driven"; }, percentValue);
    ampBass = addParam ("ampBass", TRANS ("Amp Bass"), { -15.0f, 15.0f }, eqDb, plain);
    ampMid = addParam ("ampMid", TRANS ("Amp Mid"), { -15.0f, 15.0f }, eqDb, plain);

    juce::NormalisableRange<float> midRange { 200.0f, 3000.0f };
    midRange.setSkewForCentre (700.0f);
    ampMidFreq = addParam ("ampMidFreq", TRANS ("Amp Mid Freq"), midRange,
                           [] (float v) { return v < 1000.0f ? juce::String (juce::roundToInt (v)) + " Hz" : juce::String (v / 1000.0f, 2) + " kHz"; },
                           [] (const juce::String& s) { return s.containsIgnoreCase ("k") ? s.getFloatValue() * 1000.0f : s.getFloatValue(); });
    ampTreble = addParam ("ampTreble", TRANS ("Amp Treble"), { -15.0f, 15.0f }, eqDb, plain);
    ampLevel = addParam ("ampLevel", TRANS ("Amp Level"), { -24.0f, 12.0f }, eqDb, plain);

    ampDrive->attachToCurrentValue (ampDriveValue);
    ampBlend->attachToCurrentValue (ampBlendValue);
    ampBass->attachToCurrentValue (ampBassValue);
    ampMid->attachToCurrentValue (ampMidValue);
    ampMidFreq->attachToCurrentValue (ampMidFreqValue);
    ampTreble->attachToCurrentValue (ampTrebleValue);
    ampLevel->attachToCurrentValue (ampLevelValue);

    pick->attachToCurrentValue (pickValue);
    mute->attachToCurrentValue (muteValue);
    attack->attachToCurrentValue (attackValue);
    attackEq->attachToCurrentValue (attackEqValue);
    tone->attachToCurrentValue (toneValue);
    body->attachToCurrentValue (bodyValue);
    buzz->attachToCurrentValue (buzzValue);
    sustain->attachToCurrentValue (sustainValue);
    slideTime->attachToCurrentValue (slideTimeValue);
    tap->attachToCurrentValue (tapValue);
    slideHold->attachToCurrentValue (slideHoldValue);
    outFrets->attachToCurrentValue (outFretsValue);
    tune->attachToCurrentValue (tuneValue);
    level->attachToCurrentValue (levelValue);

    readSwitches();
    state.addListener (&watcher);

    if (auto* kit = loadKit())
    {
        spec.reserve (*kit);

        for (int i = 0; i < maxVoices; ++i)
        {
            auto v = std::make_unique<PlayVoice>();
            v->voice.reserve (*kit);
            voices.push_back (std::move (v));
        }
    }
}

ElectricBassPlugin::~ElectricBassPlugin()
{
    notifyListenersOfDeletion();
    state.removeListener (&watcher);

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void ElectricBassPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, pickValue, muteValue, attackValue, attackEqValue,
                                      toneValue, bodyValue, buzzValue, sustainValue,
                                      slideTimeValue, tapValue, slideHoldValue, outFretsValue, tuneValue, levelValue,
                                      techniqueValue, monoValue, fretlessValue,
                                      ampDriveValue, ampBlendValue, ampBassValue, ampMidValue, ampMidFreqValue,
                                      ampTrebleValue, ampLevelValue, ampOnValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();

    readSwitches();
}

void ElectricBassPlugin::readSwitches()
{
    const auto name = techniqueValue.get();
    technique.store (name == "pluck" ? 0 : name == "slide" ? 2 : 1, std::memory_order_relaxed);
    mono.store (monoValue.get(), std::memory_order_relaxed);
    fretless.store (fretlessValue.get(), std::memory_order_relaxed);
    ampOn.store (ampOnValue.get(), std::memory_order_relaxed);
}

void ElectricBassPlugin::setFretless (bool shouldBeFretless)
{
    fretlessValue = shouldBeFretless;
}

void ElectricBassPlugin::setAmpOn (bool on)
{
    ampOnValue = on;
}

fx::BassAmp::Settings ElectricBassPlugin::getAmpSettings() const
{
    fx::BassAmp::Settings s;
    s.drive = ampDrive->getCurrentValue();
    s.blend = ampBlend->getCurrentValue();
    s.bassDb = ampBass->getCurrentValue();
    s.midDb = ampMid->getCurrentValue();
    s.midHz = ampMidFreq->getCurrentValue();
    s.trebleDb = ampTreble->getCurrentValue();
    s.levelDb = ampLevel->getCurrentValue();
    return s;
}

ElectricBassPlugin::Technique ElectricBassPlugin::getTechnique() const
{
    return (Technique) technique.load (std::memory_order_relaxed);
}

void ElectricBassPlugin::setTechnique (Technique t)
{
    techniqueValue = t == Technique::pluck ? "pluck" : t == Technique::slide ? "slide" : "hammer";
}

bool ElectricBassPlugin::isMono() const
{
    return mono.load (std::memory_order_relaxed);
}

void ElectricBassPlugin::setMono (bool shouldBeMono)
{
    monoValue = shouldBeMono;
}

bass::Options ElectricBassPlugin::makeOptions() const
{
    bass::Options o;
    const auto p = pick->getCurrentValue();
    o.pick = p < 0.03f ? 0.0 : p;
    o.mute = mute->getCurrentValue();
    o.attack = attack->getCurrentValue();
    o.attackEq = attackEq->getCurrentValue();
    o.bright = tone->getCurrentValue();
    o.body = body->getCurrentValue();
    o.buzz = buzz->getCurrentValue();
    o.decay = sustain->getCurrentValue();
    o.gain = level->getCurrentValue();
    return o;
}

//==============================================================================
void ElectricBassPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    killAll();
    scratch.assign ((size_t) juce::jmax (info.blockSizeSamples, 512), 0.0f);
    amp.prepare (info.sampleRate, juce::jmax (info.blockSizeSamples, 512));
}

void ElectricBassPlugin::killAll()
{
    for (auto& v : voices)
    {
        v->active = false;
        v->next = v->slideFrom = nullptr;
    }

    lead = nullptr;
    numHeld = 0;

    for (auto& t : triggers)
        t.technique = 0;
}

ElectricBassPlugin::PlayVoice* ElectricBassPlugin::allocate (std::initializer_list<const PlayVoice*> keep)
{
    auto fresh = [] (PlayVoice* v)
    {
        v->boost = v->boostCoefficient = 1.0;
        v->boostLeft = 0;
        return v;
    };

    for (auto& v : voices)
        if (! v->active)
            return fresh (v.get());

    // All busy: the oldest voice that is neither the one being played nor
    // one the caller is in the middle of using.
    PlayVoice* oldest = nullptr;

    for (auto& v : voices)
        if (v.get() != lead && std::find (keep.begin(), keep.end(), v.get()) == keep.end()
             && (oldest == nullptr || v->age < oldest->age))
            oldest = v.get();

    if (oldest == nullptr)
        oldest = voices.front().get();

    kill (*oldest);
    return fresh (oldest);
}

void ElectricBassPlugin::kill (PlayVoice& v)
{
    v.active = false;

    // Nothing may point at a voice that is about to be reused.
    for (auto& other : voices)
    {
        if (other->next == &v)
            other->next = nullptr;
        if (other->slideFrom == &v)
            other->slideFrom = nullptr;
    }

    if (v.next != nullptr)
    {
        auto* pending = v.next;
        v.next = nullptr;
        kill (*pending);
    }

    v.slideFrom = nullptr;

    if (lead == &v)
        lead = nullptr;
}

ElectricBassPlugin::PlayVoice* ElectricBassPlugin::sounding (PlayVoice* v)
{
    // Mid-slide, what can be heard is the bent voice, not the landing.
    if (v != nullptr && v->slideFrom != nullptr && v->slideFrom->active && (v->delay > 0 || v->rampLeft > 0))
        return v->slideFrom;

    return v;
}

void ElectricBassPlugin::crossfade (PlayVoice& from, PlayVoice& to, double seconds)
{
    if (from.next != nullptr && from.next != &to)
    {
        // A slide's landing that will not happen now.
        auto* pending = from.next;
        from.next = nullptr;
        kill (*pending);
    }

    const auto fs = sampleRate;
    from.rampTo (0.0, seconds, fs, true);
    to.gain = 0.0;
    to.rampTo (to.gv, seconds, fs, false);
}

void ElectricBassPlugin::release (PlayVoice& v, double seconds)
{
    if (v.slideFrom != nullptr && v.slideFrom->active && v.delay > 0)
    {
        // Let go before the slide landed: stop where it has got to.
        v.slideFrom->releasing = true;
        v.slideFrom->rampLeft = 0;
        v.slideFrom->releaseCoef = std::exp (-1.0 / (seconds * sampleRate));
    }

    if (v.delay > 0)
    {
        kill (v);
        return;
    }

    if (v.rampLeft > 0 && v.dieAfterRamp)
        return;   // already on its way out

    v.releasing = true;
    v.rampLeft = 0;
    v.releaseCoef = std::exp (-1.0 / (seconds * sampleRate));
}

ElectricBassPlugin::PlayVoice* ElectricBassPlugin::pluck (int note, float velocity)
{
    auto* kit = loadKit();

    if (kit == nullptr || voices.empty())
        return nullptr;

    if (isMono())
        for (auto& v : voices)
            if (v->active && ! v->tapOnly)
                release (*v, monoCutSeconds);

    bass::makeSpec (*kit, note, velocity, getTuneSemitones(), spec);

    auto* v = allocate();
    v->voice.start (*kit, spec, sampleRate, makeOptions());
    v->active = true;
    v->note = note;
    v->velocity = velocity;
    v->gv = v->gain = 1.0;
    v->rampLeft = 0;
    v->releasing = false;
    v->delay = 0;
    v->tapOnly = false;
    v->next = v->slideFrom = nullptr;
    v->age = ++age;
    return v;
}

ElectricBassPlugin::PlayVoice* ElectricBassPlugin::legato (PlayVoice& from, int note, Technique tech, double slideSeconds)
{
    auto* kit = loadKit();
    const double fs = sampleRate;
    const double tau = from.getTau();

    if (kit == nullptr || tau > from.voice.getLength() / fs - legatoTailSeconds)
        return pluck (note, from.velocity);

    bass::makeSpec (*kit, note, from.velocity, getTuneSemitones(), spec);

    auto matchGain = [] (const PlayVoice& a, double tauA, const PlayVoice& b, double tauB, double lossDb)
    {
        // What `a` sounds at now, its slide's hold included.
        const double la = a.voice.levelAt (tauA) * a.gv * a.boost, lb = b.voice.levelAt (tauB);
        return lb > 0.0 ? la / lb * std::pow (10.0, lossDb / 20.0) : 0.0;
    };

    if (tech == Technique::slide)
    {
        const double glide = slideSeconds > 0.0 ? juce::jlimit (0.01, 4.0, slideSeconds)
                                                : (double) slideTime->getCurrentValue();
        const auto& fromSpec = from.voice.getSpec();
        const double rNow = from.voice.bendAt (tau), rTo = spec.f0 / fromSpec.f0;

        // The ringing note carried on, bent: a voice that matches it sample
        // for sample, plus the bend.
        auto* cont = allocate ({ &from });
        auto contOptions = from.voice.getOptions();
        contOptions.start = tau;
        contOptions.addBend ({ tau, glide, rNow, rTo, ! isFretless() });
        cont->voice.start (*kit, fromSpec, fs, contOptions);
        cont->active = true;
        cont->note = note;
        cont->velocity = from.velocity;
        cont->gv = from.gv;
        cont->delay = 0;
        cont->tapOnly = false;
        cont->next = cont->slideFrom = nullptr;
        cont->age = ++age;
        crossfade (from, *cont, slideInFade);

        // A finger held down along the string keeps it ringing: the slide
        // gives back Slide Hold's share of what the note would lose on the
        // way, as a gain rising evenly (in dB) over the slide.
        double heldBoost = 1.0;

        if (const auto before = cont->voice.levelAt (tau), after = cont->voice.levelAt (tau + glide);
            before > 0.0 && after > 0.0 && after < before)
        {
            heldBoost = std::pow (before / after, (double) slideHold->getCurrentValue());
            cont->boostLeft = juce::jmax (1, juce::roundToInt (glide * fs));
            cont->boostCoefficient = std::pow (heldBoost, 1.0 / cont->boostLeft);
        }

        // Where it lands, as the landing note's own sound (no attack).
        auto* arrive = allocate ({ &from, cont });
        auto options = makeOptions();
        options.transient = false;
        options.start = tau + glide;
        arrive->voice.start (*kit, spec, fs, options);
        arrive->active = true;
        arrive->note = note;
        arrive->velocity = from.velocity;
        arrive->gv = matchGain (*cont, tau + glide, *arrive, tau + glide, 0.0) * heldBoost;
        arrive->gain = 0.0;
        arrive->rampLeft = 0;
        arrive->releasing = false;
        arrive->delay = juce::jmax (1, juce::roundToInt (glide * fs));
        arrive->arrivalFade = arriveFade;
        arrive->tapOnly = false;
        arrive->next = nullptr;
        arrive->slideFrom = cont;
        arrive->age = ++age;
        cont->next = arrive;
        return arrive;
    }

    // Hammer-on going up, pull-off going down.
    const bool up = note > from.note;

    auto* nv = allocate ({ &from });
    auto options = makeOptions();
    options.transient = false;
    options.start = tau;
    nv->voice.start (*kit, spec, fs, options);
    nv->active = true;
    nv->note = note;
    nv->velocity = from.velocity;
    nv->gv = matchGain (from, tau, *nv, tau, up ? -1.5 : -0.5);
    nv->delay = 0;
    nv->tapOnly = false;
    nv->next = nv->slideFrom = nullptr;
    nv->age = ++age;
    crossfade (from, *nv, hammerFade);

    // The finger landing on the fret (or plucking the string on a pull-off):
    // the new note's recorded attack, quietly.
    const double tapDb = tap->getCurrentValue() + (up ? 0.0 : 6.0);

    if (tapDb > -40.0)
    {
        auto* tv = allocate ({ &from, nv });
        auto tapOptions = makeOptions();
        tapOptions.harmonics = false;
        tapOptions.attack += tapDb;
        tapOptions.dur = spec.prim->ta + 0.02;
        tv->voice.start (*kit, spec, fs, tapOptions);
        tv->active = true;
        tv->note = -1;
        tv->velocity = from.velocity;
        tv->gv = tv->gain = nv->gv;
        tv->rampLeft = 0;
        tv->releasing = false;
        tv->delay = 0;
        tv->tapOnly = true;
        tv->next = tv->slideFrom = nullptr;
        tv->age = ++age;
    }

    return nv;
}

void ElectricBassPlugin::readTrigger (const juce::MidiMessage& m, int position)
{
    const auto* data = m.getSysExData();

    const auto size = m.getSysExDataSize();

    // 5 bytes before slides had lengths of their own.
    if ((size != 5 && size != 8) || data[0] != sysexId[0] || data[1] != sysexId[1] || data[2] != sysexId[2])
        return;

    const int kind = data[3] & 0x7f, note = data[4] & 0x7f;
    const int ms = size == 8 ? ((data[5] & 0x7f) << 14) | ((data[6] & 0x7f) << 7) | (data[7] & 0x7f) : 0;

    if (kind >= 1 && kind <= 3)
        triggers[(size_t) note] = { kind, position, ms / 1000.0 };
}

ElectricBassPlugin::Mark ElectricBassPlugin::takeTrigger (int note, int position)
{
    // The trigger for this note-on is the one beside it: within a few
    // milliseconds either way. Anything older belonged to another note.
    auto& t = triggers[(size_t) juce::jlimit (0, 127, note)];
    const int window = juce::roundToInt (0.005 * sampleRate);
    const bool mine = t.technique != 0 && std::abs (t.position - position) <= window;
    const Mark mark { mine ? t.technique : 0, mine ? t.slideSeconds : 0.0 };
    t.technique = 0;
    return mark;
}

void ElectricBassPlugin::slideOff (const juce::MidiMessage& m)
{
    const auto* data = m.getSysExData();

    if (m.getSysExDataSize() != 9 || data[0] != sysexId[0] || data[1] != sysexId[1] || data[2] != sysexId[2]
         || data[3] != 4)
        return;

    auto* kit = loadKit();
    const int note = juce::jlimit (lowestNote, highestNote, data[4] & 0x7f);
    const bool up = (data[5] & 0x7f) != 0;
    const double seconds = juce::jmax (0.01, (((data[6] & 0x7f) << 14) | ((data[7] & 0x7f) << 7) | (data[8] & 0x7f)) / 1000.0);

    // The voice playing the note: the one being played if it is this note,
    // else the newest voice on it still held.
    PlayVoice* from = nullptr;

    if (lead != nullptr && lead->active && lead->note == note)
        from = sounding (lead);
    else
        for (auto& v : voices)
            if (v->active && ! v->tapOnly && v->note == note && ! v->releasing && v->delay == 0
                 && (from == nullptr || v->age > from->age))
                from = v.get();

    if (kit == nullptr || from == nullptr)
        return;

    const double fs = sampleRate, tau = from->getTau();

    if (tau > from->voice.getLength() / fs - 0.05)
        return;

    // The string carried on from where it is, bent away by Out Frets...
    auto* cont = allocate ({ from });
    auto options = from->voice.getOptions();
    options.start = tau;
    const double rNow = from->voice.bendAt (tau);
    const double frets = std::round (outFrets->getCurrentValue());
    options.addBend ({ tau, seconds, rNow, rNow * std::pow (2.0, (up ? frets : -frets) / 12.0), ! isFretless() });

    cont->voice.start (*kit, from->voice.getSpec(), fs, options);
    cont->active = true;
    cont->note = from->note;
    cont->velocity = from->velocity;
    cont->gv = from->gv;
    cont->delay = 0;
    cont->tapOnly = false;
    cont->next = cont->slideFrom = nullptr;
    cont->age = ++age;
    cont->boost = from->boost;
    crossfade (*from, *cont, slideInFade);

    // ...and let go of as it travels: down 30 dB by the time it gets there,
    // which is where the note ends.
    cont->boostLeft = juce::jmax (1, juce::roundToInt (seconds * fs));
    cont->boostCoefficient = std::pow (juce::Decibels::decibelsToGain (-30.0), 1.0 / cont->boostLeft);

    if (lead == from || (lead != nullptr && lead->slideFrom == from))
        lead = cont;
}

void ElectricBassPlugin::noteOn (int note, float velocity, Mark mark)
{
    const int marked = mark.technique;
    note = juce::jlimit (lowestNote, highestNote, note);

    // Marked, the note says how to get to it; unmarked, overlapping notes
    // go the way the Left Hand setting says.
    const auto tech = marked == 1 ? Technique::pluck : marked == 2 ? Technique::hammer
                    : marked == 3 ? Technique::slide : getTechnique();

    // A marked hammer or slide moves from the last note even once its key is
    // up, as long as it still rings: the finger never left the string.
    const bool fromRinging = lead != nullptr && lead->active && ! lead->tapOnly;
    const bool canLegato = tech != Technique::pluck && fromRinging
                            && (marked != 0 || (! lead->releasing && numHeld > 0));

    if (canLegato && lead->releasing)
    {
        lead->releasing = false;
        lead->gain = lead->gv;
    }

    PlayVoice* v = canLegato ? legato (*sounding (lead), note, tech, mark.slideSeconds) : pluck (note, velocity);

    // Held, newest last.
    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i] == note)
        {
            std::move (held.begin() + i + 1, held.begin() + numHeld, held.begin() + i);
            --numHeld;
            break;
        }

    if (numHeld < (int) held.size())
        held[(size_t) numHeld++] = note;

    if (v != nullptr)
        lead = v;

    lastNote.store (note, std::memory_order_relaxed);
    lastVelocity.store (velocity, std::memory_order_relaxed);
    noteCount.fetch_add (1, std::memory_order_relaxed);
}

void ElectricBassPlugin::noteOff (int note)
{
    note = juce::jlimit (lowestNote, highestNote, note);

    for (int i = 0; i < numHeld; ++i)
        if (held[(size_t) i] == note)
        {
            std::move (held.begin() + i + 1, held.begin() + numHeld, held.begin() + i);
            --numHeld;
            break;
        }

    // Letting go of the sounding note with others still held goes back to
    // the most recent of them -- a trill.
    if (lead != nullptr && lead->active && lead->note == note && ! lead->releasing
         && getTechnique() != Technique::pluck && numHeld > 0)
    {
        if (auto* v = legato (*sounding (lead), held[(size_t) numHeld - 1], getTechnique()))
            lead = v;

        return;
    }

    for (auto& v : voices)
        if (v->active && ! v->tapOnly && v->note == note && ! v->releasing)
            release (*v, releaseSeconds);
}

void ElectricBassPlugin::render (float* out, int numSamples)
{
    juce::FloatVectorOperations::clear (out, numSamples);

    for (auto& vp : voices)
    {
        auto& v = *vp;

        if (! v.active)
            continue;

        int offset = 0;

        if (v.delay > 0)
        {
            if (v.delay >= numSamples)
            {
                v.delay -= numSamples;
                continue;
            }

            offset = v.delay;
            v.delay = 0;

            // Landed: take over from the bent voice.
            if (v.slideFrom != nullptr && v.slideFrom->active)
            {
                v.slideFrom->rampTo (0.0, v.arrivalFade, sampleRate, true);
                v.slideFrom->next = nullptr;
            }

            v.slideFrom = nullptr;
            v.gain = 0.0;
            v.rampTo (v.gv, v.arrivalFade, sampleRate, false);
        }

        const int count = numSamples - offset;
        const int written = v.voice.render (scratch.data(), count);

        for (int i = 0; i < written; ++i)
        {
            if (! v.stepGain())
            {
                v.active = false;
                break;
            }

            out[offset + i] += (float) (scratch[(size_t) i] * v.gain * v.boost);
        }

        if (written < count || v.voice.isDone())
            v.active = false;

        if (! v.active)
            kill (v);
    }
}

void ElectricBassPlugin::applyToBuffer (const te::PluginRenderContext& fc)
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

    auto* out = buffer.getWritePointer (0, fc.bufferStartSample);
    int pos = 0;

    // Triggers from the last block are a block further back now.
    for (auto& t : triggers)
        t.position = juce::jmax (-(1 << 24), t.position - fc.bufferNumSamples);

    auto positionOf = [&] (const juce::MidiMessage& m)
    {
        return juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));
    };

    // Every trigger in the block first, so a note-on finds its own whichever
    // of the two the list put first.
    if (fc.bufferForMidiMessages != nullptr)
        for (const auto& m : *fc.bufferForMidiMessages)
            if (m.isSysEx())
                readTrigger (m, positionOf (m));

    auto renderTo = [this, out] (int from, int to)
    {
        // In pieces the scratch buffer holds.
        while (from < to)
        {
            const int n = juce::jmin (to - from, (int) scratch.size());
            render (out + from, n);
            from += n;
        }
    };

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            if (! m.isNoteOnOrOff() && ! m.isSysEx())
                continue;

            const int at = positionOf (m);

            if (at > pos)
            {
                renderTo (pos, at);
                pos = at;
            }

            if (m.isSysEx())
                slideOff (m);
            else if (m.isNoteOn())
                noteOn (m.getNoteNumber(), m.getFloatVelocity(), takeTrigger (m.getNoteNumber(), at));
            else
                noteOff (m.getNoteNumber());
        }
    }

    if (pos < fc.bufferNumSamples)
        renderTo (pos, fc.bufferNumSamples);

    if (isAmpOn())
        amp.process (out, fc.bufferNumSamples, getAmpSettings());

    if (numChannels > 1)
        buffer.copyFrom (1, fc.bufferStartSample, buffer, 0, fc.bufferStartSample, fc.bufferNumSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
