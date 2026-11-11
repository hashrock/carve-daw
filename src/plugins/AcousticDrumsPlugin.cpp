#include "AcousticDrumsPlugin.h"

#include <mutex>

#include "InstrumentData.h"

// The decoder is kept as the prototype wrote it (see the note at its top),
// warnings and all.
#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "CtdDecoder.h"
#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace carve::plugins
{

namespace
{
    constexpr int maxVoices = 32;
    constexpr double chokeSeconds = 0.04;   // an open hat closed by the next hat
    constexpr double stealSeconds = 0.01;   // a voice taken for a new hit
    constexpr double masterGain = 0.7;      // the prototype's master, which its balance assumes

    using Group = AcousticDrumsPlugin::Group;

    const std::array<AcousticDrumsPlugin::Pad, AcousticDrumsPlugin::numPads> pads {{
        { "kick",    "Kick",           "Kick",   { 36, 35 }, 1.0f,  Group::kickSnare },
        { "snare",   "Snare",          "Snare",  { 38, 0 },  0.8f,  Group::kickSnare },
        { "rim",     "Rimshot",        "Rim",    { 40, 0 },  0.8f,  Group::kickSnare },
        { "xstick",  "Cross Stick",    "X-Stk",  { 37, 0 },  0.5f,  Group::kickSnare },

        { "hhc",     "Closed Hat",     "CH",     { 42, 0 },  0.35f, Group::hats },
        { "hhp",     "Pedal Hat",      "Pedal",  { 44, 0 },  0.25f, Group::hats },
        { "hhh",     "Half-Open Hat",  "Half",   { 52, 0 },  0.4f,  Group::hats },
        { "hho",     "Open Hat",       "OH",     { 46, 0 },  0.45f, Group::hats },
        { "hhs",     "Hat Splash",     "Splash", { 55, 0 },  0.4f,  Group::hats },

        { "htom",    "High Tom",       "HT",     { 50, 0 },  0.8f,  Group::toms },
        { "mtom",    "Mid Tom",        "MT",     { 48, 47 }, 0.8f,  Group::toms },
        { "ltom",    "Low Tom",        "LT",     { 45, 0 },  0.85f, Group::toms },
        { "ftom",    "Floor Tom",      "FT",     { 43, 41 }, 0.9f,  Group::toms },

        { "crash",   "Crash",          "Crash",  { 49, 57 }, 0.6f,  Group::cymbals },
        { "ride",    "Ride",           "Ride",   { 51, 59 }, 0.4f,  Group::cymbals },
        { "bell",    "Ride Bell",      "Bell",   { 53, 0 },  0.45f, Group::cymbals },

        { "cowbell", "Cowbell",        "Cowbl",  { 56, 0 },  0.45f, Group::percussion },
        { "tamb",    "Tambourine",     "Tamb",   { 54, 0 },  0.4f,  Group::percussion },
        { "shaker",  "Shaker",         "Shakr",  { 70, 82 }, 0.35f, Group::percussion },
        { "claves",  "Claves",         "Clave",  { 75, 0 },  0.45f, Group::percussion },
        { "wbhi",    "Woodblock Hi",   "WB Hi",  { 76, 0 },  0.45f, Group::percussion },
        { "wblo",    "Woodblock Lo",   "WB Lo",  { 77, 0 },  0.45f, Group::percussion },
        { "bongoh",  "Bongo Hi",       "Bng Hi", { 60, 0 },  0.6f,  Group::percussion },
        { "bongol",  "Bongo Lo",       "Bng Lo", { 61, 0 },  0.6f,  Group::percussion },
        { "conga",   "Conga",          "Conga",  { 63, 62 }, 0.65f, Group::percussion },
        { "tumba",   "Tumba",          "Tumba",  { 64, 0 },  0.65f, Group::percussion },
        { "timbh",   "Timbale Hi",     "Tmb Hi", { 65, 0 },  0.6f,  Group::percussion },
        { "timbl",   "Timbale Lo",     "Tmb Lo", { 66, 0 },  0.6f,  Group::percussion },
    }};

    using P = AcousticDrumsPlugin::Param;

    const std::array<AcousticDrumsPlugin::ParamSpec, AcousticDrumsPlugin::numParams> paramSpecs {{
        { "level",  "Level",  -30.0f, 6.0f,  0.0f, "dB" },
        { "tune",   "Tune",   -12.0f, 12.0f, 0.0f, "st" },
        { "decay",  "Decay",  0.25f,  4.0f,  1.0f, "x" },
        { "attack", "Attack", -30.0f, 12.0f, 0.0f, "dB" },
        { "body",   "Body",   -30.0f, 12.0f, 0.0f, "dB" },
        { "noise",  "Noise",  -30.0f, 12.0f, 0.0f, "dB" },
        { "tone",   "Tone",   -6.0f,  6.0f,  0.0f, "dB/oct" },
    }};

    // The kit is parsed once per process and never freed: every instance
    // plays the same data, and it is the data, not a copy, that the voices
    // read from.
    const ctd::Kit* loadKit()
    {
        static std::once_flag once;
        static std::unique_ptr<ctd::Kit> kit;

        std::call_once (once, []
        {
            const auto blob = findInstrumentData ("acoustic-drums.ctd");

            if (blob.data == nullptr)
                return;

            try
            {
                const auto* bytes = static_cast<const std::uint8_t*> (blob.data);
                kit = std::make_unique<ctd::Kit> (ctd::Kit::parse (std::vector<std::uint8_t> (bytes, bytes + blob.size)));
            }
            catch (const std::exception& e)
            {
                juce::Logger::writeToLog (juce::String ("Acoustic Drums: the kit did not load: ") + e.what());
                kit.reset();
            }
        });

        return kit.get();
    }

    const ctd::Alias* findAlias (const ctd::Kit& kit, const char* id)
    {
        for (const auto& alias : kit.aliases)
            if (alias.id == id)
                return &alias;

        return nullptr;
    }

    const ctd::Instrument* findInstrument (const ctd::Kit& kit, const char* id)
    {
        if (auto* alias = findAlias (kit, id))
            return kit.find (alias->of);

        return kit.find (id);
    }

    std::array<std::int8_t, 128> makeNoteMap()
    {
        std::array<std::int8_t, 128> map;
        map.fill (-1);

        for (int i = 0; i < AcousticDrumsPlugin::numPads; ++i)
            for (auto note : pads[(size_t) i].notes)
                if (note > 0)
                    map[(size_t) note] = (std::int8_t) i;

        return map;
    }
} // namespace

//==============================================================================
// Runs the kit at its own rate and hands the host its rate: the voices fill a
// FIFO at 48kHz, a Lagrange interpolator drains it. Pass-through when the two
// agree, which is the usual case.
struct AcousticDrumsPlugin::Bridge
{
    static constexpr int chunkSize = 256;

    void prepare (double source, double host)
    {
        ratio = source / host;
        passThrough = std::abs (ratio - 1.0) < 1.0e-9;
        fifo.assign ((size_t) std::ceil (chunkSize * ratio) + 16, 0.0f);
        available = 0;
        interpolator.reset();
    }

    template <typename Generate>
    void process (float* out, int numOut, Generate&& generate)
    {
        if (passThrough)
        {
            generate (out, numOut);
            return;
        }

        // In pieces no larger than the FIFO was sized for.
        while (numOut > 0)
        {
            const int chunk = juce::jmin (numOut, chunkSize);
            const int needed = (int) std::ceil (chunk * ratio) + 2;

            if (available < needed)
            {
                generate (fifo.data() + available, needed - available);
                available = needed;
            }

            const int used = interpolator.process (ratio, fifo.data(), out, chunk);
            std::move (fifo.begin() + used, fifo.begin() + available, fifo.begin());
            available -= used;

            out += chunk;
            numOut -= chunk;
        }
    }

    double ratio = 1.0;
    bool passThrough = true;
    std::vector<float> fifo;
    int available = 0;
    juce::LagrangeInterpolator interpolator;
};

//==============================================================================
struct AcousticDrumsPlugin::Slot
{
    ctd::HitVoice voice;
    bool active = false;
    int pad = -1;
    bool choke = false;        // in the hats' choke group
    double gain = 1.0;
    double fade = 1.0, fadeStep = 0.0;   // fadeStep 0: not fading
    std::uint64_t age = 0;
    std::array<float, ctd::HitVoice::BLOCK> scratch {};
};

//==============================================================================
const char* AcousticDrumsPlugin::xmlTypeName = "carveAcousticDrums";

const std::array<AcousticDrumsPlugin::Pad, AcousticDrumsPlugin::numPads>& AcousticDrumsPlugin::getPads()
{
    return pads;
}

const char* AcousticDrumsPlugin::getGroupName (Group group)
{
    switch (group)
    {
        case Group::kickSnare:  return "Kick & Snare";
        case Group::hats:       return "Hi-Hat";
        case Group::toms:       return "Toms";
        case Group::cymbals:    return "Cymbals";
        case Group::percussion: return "Percussion";
    }

    return "";
}

int AcousticDrumsPlugin::getPadForNote (int note)
{
    static const auto map = makeNoteMap();
    return juce::isPositiveAndBelow (note, 128) ? map[(size_t) note] : -1;
}

const AcousticDrumsPlugin::ParamSpec& AcousticDrumsPlugin::getParamSpec (Param p)
{
    return paramSpecs[(size_t) p];
}

juce::String AcousticDrumsPlugin::formatValue (Param p, float v)
{
    auto signedText = [] (float x, int decimals)
    {
        return (x > 0.0f ? "+" : "") + juce::String (x, decimals);
    };

    switch (p)
    {
        case P::decay:  return juce::String::charToString (0x00d7) + juce::String (v, 2);
        case P::tune:   return signedText (v, 1) + " st";
        case P::tone:   return signedText (v, 1) + " dB/oct";
        case P::level:
        case P::attack:
        case P::body:
        case P::noise:  return v <= getParamSpec (p).minimum + 0.01f ? juce::String ("off") : signedText (v, 1) + " dB";
    }

    return {};
}

juce::Identifier AcousticDrumsPlugin::getPropertyId (int pad, Param p)
{
    // Built once: Identifiers are pooled strings, and the audio thread never
    // asks for one anyway.
    static const auto ids = []
    {
        std::vector<juce::Identifier> v;

        for (const auto& info : pads)
            for (const auto& spec : paramSpecs)
                v.emplace_back (juce::String (info.id) + "_" + spec.key);

        return v;
    }();

    return ids[(size_t) (pad * numParams + (int) p)];
}

const ctd::Kit* AcousticDrumsPlugin::getKit()
{
    return loadKit();
}

//==============================================================================
AcousticDrumsPlugin::AcousticDrumsPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    for (auto& count : hitCounts)
        count.store (0, std::memory_order_relaxed);

    {
        auto um = getUndoManager();
        compThresholdValue.referTo (state, "compThreshold", um, -18.0f);
        compRatioValue.referTo (state, "compRatio", um, 4.0f);
        compAttackValue.referTo (state, "compAttack", um, 10.0f);
        compReleaseValue.referTo (state, "compRelease", um, 120.0f);
        compMakeupValue.referTo (state, "compMakeup", um, 0.0f);
        compMixValue.referTo (state, "compMix", um, 1.0f);
        compOnValue.referTo (state, "compOn", um, false);

        auto plain = [] (const juce::String& s) { return s.getFloatValue(); };
        auto db = [] (float v) { return juce::String (v, 1) + " dB"; };
        auto ms = [] (float v) { return v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; };

        juce::NormalisableRange<float> ratioRange { 1.0f, 20.0f };
        ratioRange.setSkewForCentre (4.0f);
        juce::NormalisableRange<float> attackRange { 0.1f, 100.0f };
        attackRange.setSkewForCentre (10.0f);
        juce::NormalisableRange<float> releaseRange { 10.0f, 1000.0f };
        releaseRange.setSkewForCentre (120.0f);

        compThreshold = addParam ("compThreshold", TRANS ("Comp Threshold"), { -40.0f, 0.0f }, db, plain);
        compRatio = addParam ("compRatio", TRANS ("Comp Ratio"), ratioRange,
                              [] (float v) { return juce::String (v, v < 10.0f ? 1 : 0) + ":1"; }, plain);
        compAttack = addParam ("compAttack", TRANS ("Comp Attack"), attackRange, ms, plain);
        compRelease = addParam ("compRelease", TRANS ("Comp Release"), releaseRange, ms, plain);
        compMakeup = addParam ("compMakeup", TRANS ("Comp Makeup"), { 0.0f, 24.0f }, db, plain);
        compMix = addParam ("compMix", TRANS ("Comp Mix"), { 0.0f, 1.0f },
                            [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% wet"; },
                            [] (const juce::String& s) { return s.getFloatValue() * 0.01f; });

        compThreshold->attachToCurrentValue (compThresholdValue);
        compRatio->attachToCurrentValue (compRatioValue);
        compAttack->attachToCurrentValue (compAttackValue);
        compRelease->attachToCurrentValue (compReleaseValue);
        compMakeup->attachToCurrentValue (compMakeupValue);
        compMix->attachToCurrentValue (compMixValue);
    }

    readValuesFromState();
    state.addListener (&watcher);

    // Voices are made, and their working memory reserved, here rather than on
    // the audio thread: after reserve() a hit allocates nothing.
    if (auto* kit = loadKit())
    {
        for (int i = 0; i < maxVoices; ++i)
        {
            auto slot = std::make_unique<Slot>();
            slot->voice.reserve (*kit);
            slots.push_back (std::move (slot));
        }
    }

    bridge = std::make_unique<Bridge>();
}

AcousticDrumsPlugin::~AcousticDrumsPlugin()
{
    notifyListenersOfDeletion();
    state.removeListener (&watcher);

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void AcousticDrumsPlugin::setCompressorOn (bool on)
{
    compOnValue = on;
}

fx::BusCompressor::Settings AcousticDrumsPlugin::getCompressorSettings() const
{
    fx::BusCompressor::Settings s;
    s.thresholdDb = compThreshold->getCurrentValue();
    s.ratio = compRatio->getCurrentValue();
    s.attackMs = compAttack->getCurrentValue();
    s.releaseMs = compRelease->getCurrentValue();
    s.makeupDb = compMakeup->getCurrentValue();
    s.mix = compMix->getCurrentValue();
    return s;
}

void AcousticDrumsPlugin::readValuesFromState()
{
    for (int pad = 0; pad < numPads; ++pad)
        for (int p = 0; p < numParams; ++p)
        {
            const auto& spec = paramSpecs[(size_t) p];
            const auto v = (float) state.getProperty (getPropertyId (pad, (Param) p), spec.defaultValue);
            values[(size_t) pad][(size_t) p].store (juce::jlimit (spec.minimum, spec.maximum, v), std::memory_order_relaxed);
        }

    compOn.store ((bool) state.getProperty ("compOn", false), std::memory_order_relaxed);
}

float AcousticDrumsPlugin::getValue (int pad, Param p) const
{
    return values[(size_t) pad][(size_t) p].load (std::memory_order_relaxed);
}

void AcousticDrumsPlugin::setValue (int pad, Param p, float value)
{
    const auto& spec = paramSpecs[(size_t) p];
    value = juce::jlimit (spec.minimum, spec.maximum, value);
    const auto id = getPropertyId (pad, p);

    // A knob at its default is no property at all, so a kit nobody touched
    // stores nothing.
    if (std::abs (value - spec.defaultValue) < 1.0e-6f)
        state.removeProperty (id, nullptr);
    else
        state.setProperty (id, value, nullptr);
}

void AcousticDrumsPlugin::resetPad (int pad)
{
    for (int p = 0; p < numParams; ++p)
        state.removeProperty (getPropertyId (pad, (Param) p), nullptr);
}

void AcousticDrumsPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    for (int pad = 0; pad < numPads; ++pad)
        for (int p = 0; p < numParams; ++p)
        {
            const auto id = getPropertyId (pad, (Param) p);

            if (v.hasProperty (id))
                state.setProperty (id, v[id], nullptr);
            else
                state.removeProperty (id, nullptr);
        }

    te::copyPropertiesToCachedValues (v, compThresholdValue, compRatioValue, compAttackValue, compReleaseValue,
                                      compMakeupValue, compMixValue, compOnValue);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();

    readValuesFromState();
}

const ctd::Instrument* AcousticDrumsPlugin::getInstrument (int pad) const
{
    auto* kit = loadKit();
    return kit != nullptr ? findInstrument (*kit, pads[(size_t) pad].id) : nullptr;
}

ctd::RenderOptions AcousticDrumsPlugin::getRenderOptions (int pad) const
{
    ctd::RenderOptions opts;
    opts.tune = getValue (pad, P::tune);
    opts.decay = getValue (pad, P::decay);
    opts.attack = getValue (pad, P::attack);
    opts.modesGain = getValue (pad, P::body);
    opts.noiseGain = getValue (pad, P::noise);
    opts.tilt = getValue (pad, P::tone);

    // A knob at the bottom of its travel is that part switched off, which
    // also saves synthesising it.
    opts.transient = opts.attack > paramSpecs[(size_t) P::attack].minimum + 0.01;
    opts.modes = opts.modesGain > paramSpecs[(size_t) P::body].minimum + 0.01;
    opts.noise = opts.noiseGain > paramSpecs[(size_t) P::noise].minimum + 0.01;

    // The two toms the kit has no recordings of are its other two, retuned.
    if (auto* kit = loadKit())
        if (auto* alias = findAlias (*kit, pads[(size_t) pad].id))
            opts = ctd::withAlias (*alias, opts);

    return opts;
}

//==============================================================================
void AcousticDrumsPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    killAll();

    // From the top, so an export renders the same hits every time.
    roundRobin.fill (0);
    noteSeed = 0;

    const auto* kit = loadKit();
    bridge->prepare (kit != nullptr ? kit->fs : info.sampleRate, info.sampleRate);
    compressor.prepare (info.sampleRate);
}

void AcousticDrumsPlugin::killAll()
{
    for (auto& slot : slots)
        slot->active = false;
}

void AcousticDrumsPlugin::trigger (int pad, float velocity, bool fullQuality)
{
    const auto* kit = loadKit();
    const auto* inst = kit != nullptr ? findInstrument (*kit, pads[(size_t) pad].id) : nullptr;

    if (inst == nullptr || inst->layers.empty() || slots.empty())
        return;

    hitCounts[(size_t) pad].fetch_add (1, std::memory_order_relaxed);

    const float level = getValue (pad, P::level);

    if (level <= paramSpecs[(size_t) P::level].minimum + 0.01f)
        return;

    // Velocity picks a layer, and a gain that runs continuously across the
    // layers: each layer's average level at its centre, interpolated in
    // between, so a velocity ramp is a smooth crescendo rather than steps.
    const auto vel = juce::jlimit (1.0, 127.0, (double) velocity * 127.0);
    const int L = (int) inst->layers.size();
    const int li = juce::jmin (L - 1, (int) std::floor (vel / 128.0 * L));

    auto layerLevel = [inst] (int i)
    {
        double sum = 0.0;
        for (const auto& h : inst->layers[(size_t) i])
            sum += h.lvl;
        return sum / (double) inst->layers[(size_t) i].size();
    };

    const auto centre = [L] (int i) { return (i + 0.5) / L * 127.0; };
    const double posInLayers = vel / 127.0 * L - 0.5;
    double target;

    if (posInLayers <= 0.0)
        target = layerLevel (0) * juce::jmax (0.15, vel / centre (0));
    else if (posInLayers >= L - 1)
        target = layerLevel (L - 1) * (1.0 + 0.25 * (posInLayers - (L - 1)));
    else
    {
        const int i = (int) std::floor (posInLayers);
        const double f = posInLayers - i;
        target = std::exp (std::log (layerLevel (i)) * (1.0 - f) + std::log (layerLevel (i + 1)) * f);
    }

    const auto& hits = inst->layers[(size_t) li];
    const auto& hit = hits[(size_t) (roundRobin[(size_t) pad]++ % hits.size())];

    // The hats choke each other.
    const bool choke = ! inst->choke.empty();

    if (choke)
        for (auto& slot : slots)
            if (slot->active && slot->choke && slot->fadeStep == 0.0)
                slot->fadeStep = 1.0 / (chokeSeconds * kit->fs);

    // A free voice, or the oldest one, faded out quickly.
    Slot* free = nullptr;

    for (auto& slot : slots)
        if (! slot->active)
        {
            free = slot.get();
            break;
        }

    if (free == nullptr)
    {
        Slot* oldest = nullptr;

        for (auto& slot : slots)
            if (slot->fadeStep == 0.0 && (oldest == nullptr || slot->age < oldest->age))
                oldest = slot.get();

        if (oldest != nullptr)
            oldest->fadeStep = 1.0 / (stealSeconds * kit->fs);

        // Take whichever is closest to silent.
        free = slots.front().get();

        for (auto& slot : slots)
            if (slot->fade < free->fade)
                free = slot.get();
    }

    auto opts = getRenderOptions (pad);
    opts.quality = fullQuality ? ctd::Quality::Full : ctd::Quality::Mid;

    // A different noise every hit, but the same sequence on every export.
    opts.seed = 1u + (++noteSeed) * 2654435761u;

    free->voice.start (*kit, *inst, hit, opts);
    free->active = true;
    free->pad = pad;
    free->choke = choke;
    free->gain = target / hit.lvl * pads[(size_t) pad].mix * masterGain * juce::Decibels::decibelsToGain ((double) level);
    free->fade = 1.0;
    free->fadeStep = 0.0;
    free->age = ++age;
}

void AcousticDrumsPlugin::renderVoices (float* out, int numSamples)
{
    juce::FloatVectorOperations::clear (out, numSamples);

    for (auto& slotPtr : slots)
    {
        auto& slot = *slotPtr;

        if (! slot.active)
            continue;

        for (int done = 0; done < numSamples && slot.active;)
        {
            const int count = juce::jmin (numSamples - done, ctd::HitVoice::BLOCK);
            const int written = slot.voice.render (slot.scratch.data(), count);

            for (int i = 0; i < written; ++i)
            {
                out[done + i] += (float) (slot.scratch[(size_t) i] * slot.gain * slot.fade);

                if (slot.fadeStep > 0.0)
                {
                    slot.fade -= slot.fadeStep;

                    if (slot.fade <= 0.0)
                    {
                        slot.active = false;
                        break;
                    }
                }
            }

            done += written;

            if (written < count || slot.voice.done())
                slot.active = false;
        }
    }
}

void AcousticDrumsPlugin::applyToBuffer (const te::PluginRenderContext& fc)
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

    auto generate = [this] (float* dst, int count) { renderVoices (dst, count); };

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            if (! m.isNoteOn())
                continue;

            const int pad = getPadForNote (m.getNoteNumber());

            if (pad < 0)
                continue;

            // Seconds from the start of the block, as tracktion hands every
            // instrument its MIDI.
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * this->sampleRate));

            if (at > pos)
            {
                bridge->process (out + pos, at - pos, generate);
                pos = at;
            }

            trigger (pad, m.getFloatVelocity(), fc.isRendering);
        }
    }

    if (pos < fc.bufferNumSamples)
        bridge->process (out + pos, fc.bufferNumSamples - pos, generate);

    if (isCompressorOn())
        compressor.process (out, fc.bufferNumSamples, getCompressorSettings(), &compressorHistory);

    if (numChannels > 1)
        buffer.copyFrom (1, fc.bufferStartSample, buffer, 0, fc.bufferStartSample, fc.bufferNumSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
