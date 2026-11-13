#include "VocalModelPlugin.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins
{

namespace vocal = voice::vocal;

const char* VocalModelPlugin::xmlTypeName = "carveVocalModel";

namespace
{
    constexpr juce::uint8 triggerHeader[] = { 0x7d, 0x43, 0x56 };   // non-commercial id, "CV"

    // A part no longer in the song keeps its ids this long, so an undo straight
    // after an edit gets the same ones back and the clips need no rewrite.
    constexpr double keepUnwantedMs = 20000.0;

    // A replaced plan is freed only after this long: the audio thread reads a
    // plan for as long as one trigger takes to copy it.
    constexpr double retireGraceMs = 5000.0;

    double nowMs()  { return juce::Time::getMillisecondCounterHiRes(); }

    std::string lyricText (const juce::String& lyric)
    {
        return lyric.trim().toStdString();
    }

    // The order of Applied::values.
    constexpr vocal::Synth::Key optionKeys[] = {
        vocal::Synth::Key::tractLength, vocal::Synth::Key::fold, vocal::Synth::Key::resonance,
        vocal::Synth::Key::breath, vocal::Synth::Key::press, vocal::Synth::Key::registerMix,
        vocal::Synth::Key::throat, vocal::Synth::Key::twang, vocal::Synth::Key::nasal,
        vocal::Synth::Key::clarity, vocal::Synth::Key::vibrato, vocal::Synth::Key::vibratoRate,
        vocal::Synth::Key::drift, vocal::Synth::Key::brightness, vocal::Synth::Key::body,
        vocal::Synth::Key::ring, vocal::Synth::Key::air, vocal::Synth::Key::volume
    };

    std::array<double, 18> optionValues (const vocal::Options& o)
    {
        return { o.tractLength, o.fold, o.resonance, o.breath, o.press, o.registerMix, o.throat, o.twang, o.nasal,
                 o.clarity, o.vibrato, o.vibratoRate, o.drift, o.brightness, o.body, o.ring, o.air, o.volume };
    }
}

VocalModelPlugin::VocalModelPlugin (te::PluginCreationInfo info)
    : te::Plugin (info),
      table (new std::atomic<const Plan*>[(size_t) maxPlans])
{
    for (int i = 0; i < maxPlans; ++i)
        table[(size_t) i].store (nullptr, std::memory_order_relaxed);

    auto um = getUndoManager();
    const auto& c = vocal::getCharacter (vocal::defaultCharacter);

    // A new singer opens as the prototype's page does: the "default" character.
    tractLengthValue.referTo (state, "tractLength", um, (float) c.tractLength);
    foldValue.referTo (state, "fold", um, (float) c.fold);
    resonanceValue.referTo (state, "resonance", um, (float) c.resonance);
    breathValue.referTo (state, "breath", um, (float) c.breath);
    pressValue.referTo (state, "press", um, (float) c.press);
    registerValue.referTo (state, "register", um, (float) c.registerMix);
    throatValue.referTo (state, "throat", um, (float) c.throat);
    twangValue.referTo (state, "twang", um, (float) c.twang);
    nasalValue.referTo (state, "nasal", um, (float) c.nasal);
    clarityValue.referTo (state, "clarity", um, (float) c.clarity);
    vowelPresetValue.referTo (state, "vowelPreset", um, 0.0f);
    vibratoValue.referTo (state, "vibrato", um, (float) c.vibrato);
    vibratoRateValue.referTo (state, "vibratoRate", um, (float) c.vibratoRate);
    driftValue.referTo (state, "drift", um, (float) c.drift);
    brightnessValue.referTo (state, "brightness", um, (float) c.brightness);
    bodyValue.referTo (state, "body", um, (float) c.body);
    ringValue.referTo (state, "ring", um, (float) c.ring);
    airValue.referTo (state, "air", um, (float) c.air);
    reverbValue.referTo (state, "reverb", um, 0.16f);
    volumeValue.referTo (state, "volume", um, 0.8f);
    strengthValue.referTo (state, "strength", um, 1.0f);
    speedValue.referTo (state, "speed", um, 1.0f);
    liveLyric.referTo (state, "liveLyric", um, {});

    auto times = [] (float v) { return juce::String::charToString (0x00d7) + juce::String (v, 2); };
    auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
    auto percentValue = [] (const juce::String& s) { return s.getFloatValue() * 0.01f; };
    auto plain = [] (const juce::String& s) { return s.retainCharacters ("0123456789.-").getFloatValue(); };
    auto decibels = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; };
    auto signedValue = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (v, 2); };

    tractLength = addParam ("tractLength", TRANS ("Tract Length"), { 10.5f, 14.0f },
                            [] (float v) { return juce::String (v, 1) + " cm"; }, plain);
    fold = addParam ("fold", TRANS ("Fold Size"), { 0.6f, 0.85f }, times, plain);
    resonance = addParam ("resonance", TRANS ("Resonance"), { 0.5f, 2.5f }, times, plain);
    breath = addParam ("breath", TRANS ("Breathiness"), { 0.0f, 6.0f },
                       [] (float v) { return juce::String (v, 2); }, plain);
    press = addParam ("press", TRANS ("Press"), { -1.0f, 1.0f }, signedValue, plain);
    registerMix = addParam ("register", TRANS ("Register"), { 0.0f, 1.0f },
                            [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "% head"; }, percentValue);
    throat = addParam ("throat", TRANS ("Throat Width"), { 0.5f, 2.0f }, times, plain);
    twang = addParam ("twang", TRANS ("Twang"), { 0.0f, 1.0f }, percent, percentValue);
    nasal = addParam ("nasal", TRANS ("Nasality"), { 0.0f, 1.0f }, percent, percentValue);
    clarity = addParam ("clarity", TRANS ("Vowel Clarity"), { 0.6f, 1.3f }, times, plain);
    vowelPreset = addParam ("vowelPreset", TRANS ("Vowel Shapes"), { 0.0f, (float) (vocal::numVowelPresets - 1), 1.0f },
                            [] (float v) { return juce::String (vocal::getVowelPreset (juce::roundToInt (v)).name); },
                            [] (const juce::String& s) { return s.trim().equalsIgnoreCase ("standard") ? 1.0f : 0.0f; });
    vibrato = addParam ("vibrato", TRANS ("Vibrato"), { 0.0f, 2.5f }, times, plain);
    vibratoRate = addParam ("vibratoRate", TRANS ("Vibrato Rate"), { 3.0f, 8.0f },
                            [] (float v) { return juce::String (v, 1) + " Hz"; }, plain);
    drift = addParam ("drift", TRANS ("Drift"), { 0.0f, 3.0f }, times, plain);
    brightness = addParam ("brightness", TRANS ("Brightness"), { -0.5f, 2.0f }, percent, percentValue);
    body = addParam ("body", TRANS ("Body"), { -12.0f, 12.0f }, decibels, plain);
    ring = addParam ("ring", TRANS ("Ring"), { -12.0f, 12.0f }, decibels, plain);
    air = addParam ("air", TRANS ("Air"), { -6.0f, 12.0f }, decibels, plain);
    reverb = addParam ("reverb", TRANS ("Room"), { 0.0f, 0.6f }, percent, percentValue);
    volume = addParam ("volume", TRANS ("Volume"), { 0.0f, 1.5f },
                       [] (float v) { return v <= 0.0f ? juce::String ("-inf dB")
                                                       : juce::String (juce::Decibels::gainToDecibels (v / 0.8f), 1) + " dB"; },
                       [] (const juce::String& s) { return 0.8f * juce::Decibels::decibelsToGain (s.getFloatValue()); });
    strength = addParam ("strength", TRANS ("Expression"), { 0.0f, 2.0f }, times, plain);
    // How quickly it moves into each note -- consonants, slurs, scoops: up
    // for a fast song. Skewed so that 1 is in the middle of the knob.
    speed = addParam ("speed", TRANS ("Speed"), { 0.5f, 2.0f, 0.0f, 0.6309f }, times, plain);   // log 0.5 / log (0.5 / 1.5)

    tractLength->attachToCurrentValue (tractLengthValue);
    fold->attachToCurrentValue (foldValue);
    resonance->attachToCurrentValue (resonanceValue);
    breath->attachToCurrentValue (breathValue);
    press->attachToCurrentValue (pressValue);
    registerMix->attachToCurrentValue (registerValue);
    throat->attachToCurrentValue (throatValue);
    twang->attachToCurrentValue (twangValue);
    nasal->attachToCurrentValue (nasalValue);
    clarity->attachToCurrentValue (clarityValue);
    vowelPreset->attachToCurrentValue (vowelPresetValue);
    vibrato->attachToCurrentValue (vibratoValue);
    vibratoRate->attachToCurrentValue (vibratoRateValue);
    drift->attachToCurrentValue (driftValue);
    brightness->attachToCurrentValue (brightnessValue);
    body->attachToCurrentValue (bodyValue);
    ring->attachToCurrentValue (ringValue);
    air->attachToCurrentValue (airValue);
    reverb->attachToCurrentValue (reverbValue);
    volume->attachToCurrentValue (volumeValue);
    strength->attachToCurrentValue (strengthValue);
    speed->attachToCurrentValue (speedValue);

    setLiveLyric (liveLyric.get());
    plannedCenter = currentCenter();
    plannedStrength = strength->getCurrentValue();
    plannedSpeed = speed->getCurrentValue();

    // The breath and the expression are planned with the folds' size and the
    // expression knob as they were when a note was prepared; this notices
    // either moving, and plans the song again.
    startTimer (250);
}

VocalModelPlugin::~VocalModelPlugin()
{
    notifyListenersOfDeletion();
    stopTimer();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void VocalModelPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    te::copyPropertiesToCachedValues (v, tractLengthValue, foldValue, resonanceValue, breathValue, pressValue,
                                      registerValue, throatValue, twangValue, nasalValue, clarityValue);
    te::copyPropertiesToCachedValues (v, vowelPresetValue, vibratoValue, vibratoRateValue, driftValue,
                                      brightnessValue, bodyValue, ringValue, airValue, reverbValue, volumeValue);
    te::copyPropertiesToCachedValues (v, strengthValue, speedValue, liveLyric);

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();

    setLiveLyric (liveLyric.get());
}

//==============================================================================
// Characters

void VocalModelPlugin::applyCharacter (int index)
{
    JUCE_ASSERT_MESSAGE_THREAD

    const auto& c = vocal::getCharacter (index);

    if (auto um = getUndoManager())
        um->beginNewTransaction ("Character");

    auto setTo = [] (te::AutomatableParameter& parameter, double value)
    {
        parameter.parameterChangeGestureBegin();
        parameter.setParameter ((float) value, juce::sendNotification);
        parameter.parameterChangeGestureEnd();
    };

    setTo (*tractLength, c.tractLength);
    setTo (*fold, c.fold);
    setTo (*resonance, c.resonance);
    setTo (*breath, c.breath);
    setTo (*press, c.press);
    setTo (*registerMix, c.registerMix);
    setTo (*throat, c.throat);
    setTo (*twang, c.twang);
    setTo (*nasal, c.nasal);
    setTo (*clarity, c.clarity);
    setTo (*vibrato, c.vibrato);
    setTo (*vibratoRate, c.vibratoRate);
    setTo (*drift, c.drift);
    setTo (*brightness, c.brightness);
    setTo (*body, c.body);
    setTo (*ring, c.ring);
    setTo (*air, c.air);
}

int VocalModelPlugin::findCurrentCharacter() const
{
    for (int i = 0; i < vocal::numCharacters; ++i)
    {
        const auto& c = vocal::getCharacter (i);
        const std::pair<te::AutomatableParameter*, double> pairs[] = {
            { tractLength.get(), c.tractLength }, { fold.get(), c.fold }, { resonance.get(), c.resonance },
            { breath.get(), c.breath }, { press.get(), c.press }, { registerMix.get(), c.registerMix },
            { throat.get(), c.throat }, { twang.get(), c.twang }, { nasal.get(), c.nasal }, { clarity.get(), c.clarity },
            { vibrato.get(), c.vibrato }, { vibratoRate.get(), c.vibratoRate }, { drift.get(), c.drift },
            { brightness.get(), c.brightness }, { body.get(), c.body }, { ring.get(), c.ring }, { air.get(), c.air }
        };

        if (std::all_of (std::begin (pairs), std::end (pairs), [] (const auto& p)
                         { return std::abs ((double) p.first->getCurrentValue() - p.second) < 1.0e-3; }))
            return i;
    }

    return -1;
}

//==============================================================================
// Preparing (message thread)

double VocalModelPlugin::currentCenter() const
{
    return vocal::centerFor ((double) fold->getCurrentValue());
}

void VocalModelPlugin::retire (std::shared_ptr<const Plan> plan)
{
    if (plan != nullptr)
        retired.emplace_back (std::move (plan), nowMs());
}

void VocalModelPlugin::beginPreparing()
{
    ++generation;
}

std::vector<VocalModelPlugin::Prepared> VocalModelPlugin::preparePart (const juce::String& partKey,
                                                                        const std::vector<vocal::PartNote>& notes)
{
    JUCE_ASSERT_MESSAGE_THREAD

    plannedCenter = currentCenter();
    plannedStrength = strength->getCurrentValue();
    plannedSpeed = speed->getCurrentValue();

    const auto planned = vocal::planPart (notes, plannedCenter, plannedStrength, plannedSpeed);
    auto& part = parts[partKey];
    const auto previousIds = part.ids;
    part.notes = notes;
    part.ids.assign (notes.size(), -1);
    part.generation = generation;

    std::vector<Prepared> result (notes.size());

    for (size_t i = 0; i < notes.size(); ++i)
    {
        if (planned[i].empty())
            continue;

        auto plan = std::make_shared<Plan>();
        plan->events = planned[i];

        // Relative to the note, and the trigger as far ahead as its first move.
        double earliest = 0.0;

        for (auto& e : plan->events)
        {
            e.t -= notes[i].start;
            earliest = std::min (earliest, e.t);
        }

        plan->leadSec = -earliest;

        const auto key = partKey + ":" + juce::String ((int) i);
        int id = -1;

        if (const auto it = idsByKey.find (key); it != idsByKey.end())
        {
            id = it->second;
        }
        else
        {
            for (int tries = 0; tries < maxPlans; ++tries)
            {
                const auto candidate = nextId;
                nextId = (nextId + 1) % maxPlans;

                if (slots.find (candidate) == slots.end())
                {
                    id = candidate;
                    break;
                }
            }

            if (id < 0)
                continue;

            idsByKey[key] = id;
        }

        auto& slot = slots[id];
        slot.key = key;
        slot.unwantedSinceMs = 0.0;
        retire (slot.plan);
        slot.plan = plan;
        table[(size_t) id].store (plan.get());

        part.ids[i] = id;
        result[i] = { id, plan->leadSec };
    }

    // Notes the part had and no longer has.
    const auto now = nowMs();

    for (auto id : previousIds)
        if (id >= 0 && std::find (part.ids.begin(), part.ids.end(), id) == part.ids.end())
            markUnwanted (id, now);

    return result;
}

void VocalModelPlugin::markUnwanted (int id, double now)
{
    if (const auto it = slots.find (id); it != slots.end() && juce::exactlyEqual (it->second.unwantedSinceMs, 0.0))
        it->second.unwantedSinceMs = now;
}

void VocalModelPlugin::endPreparing()
{
    const auto now = nowMs();

    for (auto& [key, part] : parts)
        if (part.generation != generation)
            for (auto id : part.ids)
                markUnwanted (id, now);
}

// The folds' size or the expression has moved since the notes were planned:
// plan every part again, keeping each note's id and where its trigger is.
void VocalModelPlugin::replanAll()
{
    plannedCenter = currentCenter();
    plannedStrength = strength->getCurrentValue();
    plannedSpeed = speed->getCurrentValue();

    for (auto& [key, part] : parts)
    {
        const auto planned = vocal::planPart (part.notes, plannedCenter, plannedStrength, plannedSpeed);

        for (size_t i = 0; i < part.ids.size() && i < planned.size(); ++i)
        {
            const auto it = slots.find (part.ids[i]);

            if (it == slots.end() || it->second.plan == nullptr)
                continue;

            auto plan = std::make_shared<Plan>();
            plan->events = planned[i];
            plan->leadSec = it->second.plan->leadSec;   // the clip's trigger stays where it is

            for (auto& e : plan->events)
                e.t -= part.notes[i].start;

            retire (it->second.plan);
            it->second.plan = plan;
            table[(size_t) it->first].store (plan.get());
        }
    }
}

void VocalModelPlugin::timerCallback()
{
    if (std::abs (currentCenter() - plannedCenter) > 1.0e-6
         || std::abs (strength->getCurrentValue() - plannedStrength) > 1.0e-6
         || std::abs (speed->getCurrentValue() - plannedSpeed) > 1.0e-6)
        replanAll();

    if (liveLyric.get() != parsedLiveLyric)
        setLiveLyric (liveLyric.get());

    const auto now = nowMs();

    // Notes gone from the song for a while: their ids are free again.
    for (auto it = slots.begin(); it != slots.end();)
    {
        if (it->second.unwantedSinceMs > 0.0 && now - it->second.unwantedSinceMs > keepUnwantedMs)
        {
            table[(size_t) it->first].store (nullptr);
            retire (it->second.plan);
            idsByKey.erase (it->second.key);

            // The id may be handed out again: nothing else may refer to it.
            for (auto& [key, part] : parts)
                std::replace (part.ids.begin(), part.ids.end(), it->first, -1);

            it = slots.erase (it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = parts.begin(); it != parts.end();)
    {
        const bool gone = it->second.generation != generation
                           && std::none_of (it->second.ids.begin(), it->second.ids.end(),
                                            [] (int id) { return id >= 0; });
        it = gone ? parts.erase (it) : std::next (it);
    }

    retired.erase (std::remove_if (retired.begin(), retired.end(), [now] (const auto& r)
    {
        return now - r.second > retireGraceMs;
    }), retired.end());
}

//==============================================================================
juce::MidiMessage VocalModelPlugin::createTrigger (int id, double skipSec)
{
    const auto skip = juce::jlimit (0, (1 << 21) - 1, (int) std::lround (skipSec * 10000.0));   // 0.1 ms units

    const juce::uint8 data[] = { triggerHeader[0], triggerHeader[1], triggerHeader[2],
                                 (juce::uint8) ((id >> 14) & 0x7f), (juce::uint8) ((id >> 7) & 0x7f), (juce::uint8) (id & 0x7f),
                                 (juce::uint8) ((skip >> 14) & 0x7f), (juce::uint8) ((skip >> 7) & 0x7f), (juce::uint8) (skip & 0x7f) };

    return juce::MidiMessage::createSysExMessage (data, (int) sizeof (data));
}

bool VocalModelPlugin::parseTrigger (const juce::MidiMessage& m, int& id, double& skipSec)
{
    if (! m.isSysEx() || m.getSysExDataSize() != 9)
        return false;

    const auto* d = m.getSysExData();

    if (d[0] != triggerHeader[0] || d[1] != triggerHeader[1] || d[2] != triggerHeader[2])
        return false;

    id = (d[3] << 14) | (d[4] << 7) | d[5];
    skipSec = ((d[6] << 14) | (d[7] << 7) | d[8]) / 10000.0;
    return true;
}

//==============================================================================
// Live keys

void VocalModelPlugin::setLiveLyric (const juce::String& lyric)
{
    parsedLiveLyric = lyric;
    auto moras = vocal::parseLyrics (lyricText (lyric));

    const juce::SpinLock::ScopedLockType sl (liveLock);
    numLiveMoras = (int) std::min (moras.size(), liveMoras.size());
    std::copy_n (moras.begin(), numLiveMoras, liveMoras.begin());
    liveIndex = 0;
}

void VocalModelPlugin::liveNoteOn (int note)
{
    vocal::Mora mora;   // あ

    {
        const juce::SpinLock::ScopedTryLockType sl (liveLock);

        if (sl.isLocked() && numLiveMoras > 0)
        {
            mora = liveMoras[(size_t) (liveIndex % numLiveMoras)];
            liveIndex = (liveIndex + 1) % numLiveMoras;
        }
    }

    // As the prototype's worklet: a key pressed while the last is held (or
    // just let go) joins it; otherwise it starts afresh, and the consonant
    // delays the vowel, there being no way to start it early.
    auto& s = *synth;
    const double now = s.time();
    const bool joined = live.active && (live.held || now - live.off < 0.05);
    const double lead = vocal::leadTime (mora);
    const double t0 = now + 0.015 + (joined ? std::max (0.05, std::min (lead, std::max (0.06, (now - live.t) * 0.55))) : lead + 0.05);

    vocal::SungNote previous;

    if (joined)
    {
        previous.t = live.t;
        previous.dur = std::max (0.05, t0 - live.t);
        previous.midi = live.midi;
        previous.mora = live.mora;
    }

    if (! joined)
    {
        s.clear();
        liveState = {};
    }
    else if (! live.held)
    {
        s.clear();   // let go a moment ago: cancel the fade and carry on
    }

    vocal::SungNote n;
    n.t = t0;
    n.dur = 1;
    n.midi = note;
    n.mora = mora;
    n.open = true;
    n.speed = speed->getCurrentValue();

    scratchEvents.size = 0;
    vocal::planNote (n, joined ? &previous : nullptr, nullptr, liveState, s.getCenter(), scratchEvents);
    s.schedule (scratchEvents.events, scratchEvents.size);

    live = { true, true, note, mora, t0, 0.0 };
}

void VocalModelPlugin::liveNoteOff (int note)
{
    if (! live.active || live.midi != note || ! live.held)
        return;

    auto& s = *synth;
    const double now = s.time();
    live.held = false;
    live.off = now;

    scratchEvents.size = 0;
    vocal::planRelease (std::max (now + 0.12, live.t + 0.15), 0.6, 9, liveState, scratchEvents);
    s.schedule (scratchEvents.events, scratchEvents.size);
}

void VocalModelPlugin::stopAll()
{
    synth->silence();
    live = {};
    liveState = {};

    const juce::SpinLock::ScopedTryLockType sl (liveLock);

    if (sl.isLocked())
        liveIndex = 0;
}

//==============================================================================
// Audio

vocal::Options VocalModelPlugin::readOptions() const
{
    vocal::Options o;
    o.tractLength = tractLength->getCurrentValue() / 100.0;
    o.fold = fold->getCurrentValue();
    o.resonance = resonance->getCurrentValue();
    o.breath = breath->getCurrentValue();
    o.press = press->getCurrentValue();
    o.registerMix = registerMix->getCurrentValue();
    o.throat = throat->getCurrentValue();
    o.twang = twang->getCurrentValue();
    o.nasal = nasal->getCurrentValue();
    o.clarity = clarity->getCurrentValue();
    o.vowelPreset = juce::roundToInt (vowelPreset->getCurrentValue());
    o.vibrato = vibrato->getCurrentValue();
    o.vibratoRate = vibratoRate->getCurrentValue();
    o.drift = drift->getCurrentValue();
    o.brightness = brightness->getCurrentValue();
    o.body = body->getCurrentValue();
    o.ring = ring->getCurrentValue();
    o.air = air->getCurrentValue();
    o.volume = volume->getCurrentValue();
    return o;
}

void VocalModelPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    // A graph rebuild (every note edit) initialises the plugin again; the
    // voice carries on through it unless the rate has changed.
    if (synth == nullptr || ! juce::exactlyEqual (synthRate, info.sampleRate))
    {
        const auto o = readOptions();
        synth = std::make_unique<vocal::Synth> (info.sampleRate, o);
        synthRate = info.sampleRate;
        applied.values = optionValues (o);
        applied.vowelPreset = o.vowelPreset;
        room.prepare (info.sampleRate);
        live = {};
        liveState = {};
    }
}

void VocalModelPlugin::updateSynthOptions()
{
    const auto o = readOptions();
    const auto values = optionValues (o);

    for (size_t i = 0; i < values.size(); ++i)
        if (! juce::exactlyEqual (values[i], applied.values[i]))
        {
            synth->setOption (optionKeys[i], values[i]);
            applied.values[i] = values[i];
        }

    if (o.vowelPreset != applied.vowelPreset)
    {
        synth->setVowelPreset (o.vowelPreset);
        applied.vowelPreset = o.vowelPreset;
    }
}

void VocalModelPlugin::render (float* left, float* right, int numSamples)
{
    const auto wet = reverb->getCurrentValue() * 0.35f;

    while (numSamples > 0)
    {
        const int n = juce::jmin (numSamples, (int) mono.size());
        synth->process (mono.data(), n);

        for (int i = 0; i < n; ++i)
        {
            left[i] = mono[(size_t) i];

            if (right != nullptr)
                right[i] = mono[(size_t) i];
        }

        room.process (mono.data(), left, right, n, wet);

        left += n;

        if (right != nullptr)
            right += n;

        numSamples -= n;
    }
}

void VocalModelPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr || synth == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = buffer.getNumChannels();

    if (numChannels == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);
    updateSynthOptions();

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        stopAll();

    auto* left = buffer.getWritePointer (0, fc.bufferStartSample);
    auto* right = numChannels > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr;
    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                render (left + pos, right != nullptr ? right + pos : nullptr, at - pos);
                pos = at;
            }

            int id = -1;
            double skip = 0.0;

            if (parseTrigger (m, id, skip))
            {
                if (id >= 0 && id < maxPlans)
                    if (const auto* plan = table[(size_t) id].load())
                        synth->schedule (plan->events.data(), (int) plan->events.size(), synth->time() + plan->leadSec - skip);
            }
            else if (m.isNoteOn())
            {
                liveNoteOn (m.getNoteNumber());
            }
            else if (m.isNoteOff())
            {
                liveNoteOff (m.getNoteNumber());
            }
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                stopAll();
            }
        }
    }

    if (pos < fc.bufferNumSamples)
        render (left + pos, right != nullptr ? right + pos : nullptr, fc.bufferNumSamples - pos);

    voicedNow.store (synth->isVoiced(), std::memory_order_relaxed);
    sungHz.store (synth->isVoiced() ? (float) synth->getMeasuredHz() : 0.0f, std::memory_order_relaxed);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

//==============================================================================
// The room

void VocalModelPlugin::Room::prepare (double sampleRate)
{
    // Mutually prime-ish lengths, 30-75ms; decaying 60dB in about 2.3s, as
    // the prototype's impulse response (a 0.33s time constant) does.
    constexpr double lengthsMs[numLines] = { 29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.1, 73.3 };
    constexpr double rt60 = 2.3;

    for (int i = 0; i < numLines; ++i)
    {
        const auto length = juce::jmax (1, (int) std::lround (lengthsMs[i] * 0.001 * sampleRate));
        lines[(size_t) i].assign ((size_t) length, 0.0f);
        positions[(size_t) i] = 0;
        feedback[(size_t) i] = (float) std::pow (10.0, -3.0 * (double) length / (rt60 * sampleRate));
        damp[(size_t) i] = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 6000.0 / sampleRate));
        state[(size_t) i] = 0.0f;
    }
}

void VocalModelPlugin::Room::process (const float* in, float* left, float* right, int n, float wet)
{
    if (lines[0].empty())
        return;

    // The energy the loop adds up to, taken back out: an impulse leaves the
    // room about as loud as it went in.
    float g2 = 0.0f;

    for (auto g : feedback)
        g2 += g * g;

    const float norm = std::sqrt (juce::jmax (0.0f, 1.0f - g2 / (float) numLines)) * 1.4142f;
    const float inGain = 1.0f / std::sqrt ((float) numLines);

    for (int i = 0; i < n; ++i)
    {
        std::array<float, numLines> out {};
        float sum = 0.0f;

        for (size_t k = 0; k < (size_t) numLines; ++k)
        {
            out[k] = lines[k][(size_t) positions[k]];
            sum += out[k];
        }

        float l = 0.0f, r = 0.0f;

        for (size_t k = 0; k < (size_t) numLines; ++k)
        {
            (k % 2 == 0 ? l : r) += out[k];

            // Householder mixing, a loss for the decay, a low-pass for the air.
            const auto mixed = (out[k] - 2.0f / (float) numLines * sum) * feedback[k];
            state[k] += (mixed - state[k]) * damp[k];
            lines[k][(size_t) positions[k]] = state[k] + in[i] * inGain;

            if (++positions[k] >= (int) lines[k].size())
                positions[k] = 0;
        }

        if (wet > 0.0f)
        {
            left[i] += l * norm * wet;

            if (right != nullptr)
                right[i] += r * norm * wet;
            else
                left[i] += r * norm * wet;
        }
    }
}

} // namespace carve::plugins
