#include "GabberKickPlugin.h"

#include <algorithm>
#include <cmath>

namespace carve::plugins
{

namespace
{
    const juce::Identifier baseId ("base"), sweepId ("sweep"), pitchDecayId ("pitchDecay"),
                           attackId ("attack"), durationId ("duration"), clickId ("click"),
                           noiseId ("noise"), gainId ("gain"), normalizeId ("normalize"),
                           stagesId ("stages");

    constexpr double cutFadeSeconds = 0.003;

    // A 4-point Hermite, for a kick played off its root note.
    inline float interpolate (const std::vector<float>& s, int i, float t) noexcept
    {
        const auto last = (int) s.size() - 1;
        const auto at = [&s, last] (int k) { return s[(size_t) juce::jlimit (0, last, k)]; };

        const auto a = at (i - 1), b = at (i), c = at (i + 1), d = at (i + 2);
        const auto c1 = 0.5f * (c - a);
        const auto c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const auto c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
} // namespace

const char* GabberKickPlugin::xmlTypeName = "carveGabberKick";

GabberKickPlugin::GabberKickPlugin (te::PluginCreationInfo info) : te::Plugin (info)
{
    latestPatch = readPatchFromState();
    state.addListener (&watcher);
    renderThread.startThread();
    renderThread.notify();
}

GabberKickPlugin::~GabberKickPlugin()
{
    notifyListenersOfDeletion();
    state.removeListener (&watcher);
    renderThread.stopThread (5000);
}

//==============================================================================
gabber::Patch GabberKickPlugin::readPatchFromState() const
{
    // Everything absent reads as the first factory preset, which is what a
    // new generator starts as -- nothing has to be written to make one.
    auto patch = gabber::getFactoryPreset (0);

    patch.base = state.getProperty (baseId, patch.base);
    patch.sweep = state.getProperty (sweepId, patch.sweep);
    patch.pitchDecay = state.getProperty (pitchDecayId, patch.pitchDecay);
    patch.attack = state.getProperty (attackId, patch.attack);
    patch.duration = state.getProperty (durationId, patch.duration);
    patch.click = state.getProperty (clickId, patch.click);
    patch.noise = state.getProperty (noiseId, patch.noise);
    patch.gain = state.getProperty (gainId, patch.gain);
    patch.normalize = state.getProperty (normalizeId, patch.normalize);

    if (state.hasProperty (stagesId))
        patch.stages = gabber::stagesFromJson (state[stagesId].toString());

    return patch;
}

gabber::Patch GabberKickPlugin::getPatch() const
{
    const juce::ScopedLock sl (patchLock);
    return latestPatch;
}

int GabberKickPlugin::getMatchingFactoryPreset() const
{
    // The stages compared as stored text: a curve that has been through the
    // tree once differs from the factory's in its last few bits.
    auto withoutStages = [] (gabber::Patch p) { p.stages.clear(); return p; };

    const auto patch = getPatch();

    for (int i = 0; i < gabber::numFactoryPresets; ++i)
        if (const auto factory = gabber::getFactoryPreset (i);
            withoutStages (patch) == withoutStages (factory)
             && gabber::stagesToJson (patch.stages) == gabber::stagesToJson (factory.stages))
            return i;

    return -1;
}

std::uint64_t GabberKickPlugin::getPatchRevision() const
{
    const juce::ScopedLock sl (patchLock);
    return latestRevision;
}

void GabberKickPlugin::setPatch (const gabber::Patch& patch)
{
    {
        // Not undoable, like every plugin parameter move in the app.
        const juce::ScopedValueSetter<bool> svs (writingPatch, true);

        state.setProperty (baseId, patch.base, nullptr);
        state.setProperty (sweepId, patch.sweep, nullptr);
        state.setProperty (pitchDecayId, patch.pitchDecay, nullptr);
        state.setProperty (attackId, patch.attack, nullptr);
        state.setProperty (durationId, patch.duration, nullptr);
        state.setProperty (clickId, patch.click, nullptr);
        state.setProperty (noiseId, patch.noise, nullptr);
        state.setProperty (gainId, patch.gain, nullptr);
        state.setProperty (normalizeId, patch.normalize, nullptr);
        state.setProperty (stagesId, gabber::stagesToJson (patch.stages), nullptr);
    }

    stateChanged();
}

void GabberKickPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    for (const auto& id : { baseId, sweepId, pitchDecayId, attackId, durationId, clickId,
                            noiseId, gainId, normalizeId, stagesId })
    {
        if (v.hasProperty (id))
            state.setProperty (id, v[id], nullptr);
        else
            state.removeProperty (id, nullptr);
    }
}

void GabberKickPlugin::stateChanged()
{
    if (writingPatch)
        return;

    auto patch = readPatchFromState();

    {
        const juce::ScopedLock sl (patchLock);

        // Most of what changes in a plugin's tree is the engine's own
        // bookkeeping, which has no business re-rendering the kick.
        if (patch == latestPatch)
            return;

        latestPatch = std::move (patch);
        ++latestRevision;
    }

    renderThread.notify();
}

//==============================================================================
void GabberKickPlugin::RenderThread::run()
{
    while (! threadShouldExit())
    {
        wait (-1);

        if (threadShouldExit())
            return;

        // A drag arrives as a stream of edits; let a few of them land.
        wait (15);
        owner.renderIfStale();
    }
}

void GabberKickPlugin::renderIfStale()
{
    const juce::ScopedLock renderGuard (renderLock);

    gabber::Patch patch;
    std::uint64_t revision;
    double rate;

    {
        const juce::ScopedLock sl (patchLock);
        patch = latestPatch;
        revision = latestRevision;
        rate = renderRate;
    }

    if (const auto current = getRenderedKick();
        current != nullptr && current->revision == revision && current->sampleRate == rate)
        return;

    auto result = gabber::render (patch, rate);

    auto kick = std::make_shared<RenderedKick>();
    kick->samples = std::move (result.samples);
    kick->sampleRate = rate;
    kick->peak = result.peak;
    kick->limited = result.limited;
    kick->revision = revision;

    {
        const juce::SpinLock::ScopedLockType sl (publishLock);
        published = kick;
    }

    retained.erase (std::remove_if (retained.begin(), retained.end(),
                                    [] (const auto& held) { return held.use_count() == 1; }),
                    retained.end());
    retained.push_back (std::move (kick));
}

std::shared_ptr<const GabberKickPlugin::RenderedKick> GabberKickPlugin::getRenderedKick() const
{
    const juce::SpinLock::ScopedLockType sl (publishLock);
    return published;
}

bool GabberKickPlugin::isRenderPending() const
{
    const auto kick = getRenderedKick();
    return kick == nullptr || kick->revision != getPatchRevision();
}

//==============================================================================
void GabberKickPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    sampleRate = info.sampleRate;

    for (auto& voice : voices)
        voice.kick.reset();

    {
        const juce::ScopedLock sl (patchLock);
        renderRate = info.sampleRate;
    }

    // Here and not on the thread: an export starts playing the moment this
    // returns, and its first bar has to have the kick in it.
    renderIfStale();
}

void GabberKickPlugin::trigger (int note, float velocity)
{
    if (live == nullptr || live->samples.empty())
        return;

    auto& current = voices[0];

    if (current.kick != nullptr)
    {
        current.fadeStep = 1.0 / juce::jmax (1.0, cutFadeSeconds * sampleRate);
        voices[1] = std::move (current);
    }

    current = {};
    current.kick = live;
    current.increment = (live->sampleRate / sampleRate) * std::pow (2.0, (note - rootNote) / 12.0);
    current.gain = 0.25 + 0.75 * (double) velocity;
}

void GabberKickPlugin::renderVoices (float* out, int numSamples)
{
    for (auto& voice : voices)
    {
        if (voice.kick == nullptr)
            continue;

        const auto& samples = voice.kick->samples;
        const auto size = (int) samples.size();
        const auto exactRate = voice.increment == 1.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto index = (int) voice.position;

            if (index >= size || voice.fade <= 0.0)
            {
                voice.kick.reset();
                break;
            }

            const auto sample = exactRate ? samples[(size_t) index]
                                          : interpolate (samples, index, (float) (voice.position - index));

            out[i] += (float) (sample * voice.gain * voice.fade);
            voice.position += voice.increment;
            voice.fade -= voice.fadeStep;
        }
    }
}

void GabberKickPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = buffer.getNumChannels();

    if (numChannels == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    {
        const juce::SpinLock::ScopedTryLockType sl (publishLock);

        if (sl.isLocked())
            live = published;
    }

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        for (auto& voice : voices)
            voice.kick.reset();

    auto* out = buffer.getWritePointer (0, fc.bufferStartSample);
    int pos = 0;

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            if (! m.isNoteOn())
                continue;

            // Seconds from the start of the block, as tracktion hands every
            // instrument its MIDI.
            const int at = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate));

            if (at > pos)
            {
                renderVoices (out + pos, at - pos);
                pos = at;
            }

            trigger (m.getNoteNumber(), m.getFloatVelocity());
        }
    }

    if (pos < fc.bufferNumSamples)
        renderVoices (out + pos, fc.bufferNumSamples - pos);

    if (numChannels > 1)
        buffer.copyFrom (1, fc.bufferStartSample, buffer, 0, fc.bufferStartSample, fc.bufferNumSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);
}

} // namespace carve::plugins
