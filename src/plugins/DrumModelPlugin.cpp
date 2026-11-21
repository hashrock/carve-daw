#include "DrumModelPlugin.h"

#include <cmath>

namespace carve::plugins
{

const char* DrumModelPlugin::xmlTypeName = "carveDrumModel";

namespace
{
    using P = DrumModelPlugin;

    // live.html's sliders: tuning in semitones, mute and stick as log2 of a
    // factor (shown as the factor), glide as a factor, levels as factors.
    const DrumModelPlugin::ParamInfo paramTable[]
    {
        { "kickTune",   "Kick Tuning",        -7.0f, 7.0f, 0.0f },
        { "kickMute",   "Kick Muffling",       0.0f, 3.5f, 2.321928f },   // x5: the model has no blanket
        { "kickGlide",  "Kick Pitch Glide",    0.0f, 3.0f, 1.0f },
        { "kickBeater", "Kick Beater",        -2.0f, 2.0f, 0.0f },
        { "kickLevel",  "Kick Level",          0.0f, 2.0f, 1.0f },
        { "snareTune",  "Snare Tuning",       -7.0f, 7.0f, 0.0f },
        { "snareMute",  "Snare Muffling",     -1.0f, 2.0f, 0.0f },
        { "snareWires", "Snare Wires",         0.0f, 3.0f, 1.0f },
        { "snareStick", "Snare Stick",        -2.0f, 2.0f, 0.0f },
        { "snareLevel", "Snare Level",         0.0f, 2.0f, 1.0f },
        { "snareRim",   "Snare Rimshots",      0.0f, 1.0f, 0.0f },
        { "tom1Tune",   "Tom 10\" Tuning",    -7.0f, 7.0f, 0.0f },
        { "tom1Mute",   "Tom 10\" Muffling",  -1.0f, 2.0f, 0.0f },
        { "tom1Glide",  "Tom 10\" Pitch Glide", 0.0f, 3.0f, 1.0f },
        { "tom1Stick",  "Tom 10\" Stick",     -2.0f, 2.0f, 0.0f },
        { "tom2Tune",   "Tom 12\" Tuning",    -7.0f, 7.0f, 0.0f },
        { "tom2Mute",   "Tom 12\" Muffling",  -1.0f, 2.0f, 0.0f },
        { "tom2Glide",  "Tom 12\" Pitch Glide", 0.0f, 3.0f, 1.0f },
        { "tom2Stick",  "Tom 12\" Stick",     -2.0f, 2.0f, 0.0f },
        { "tom3Tune",   "Tom 16\" Tuning",    -7.0f, 7.0f, 0.0f },
        { "tom3Mute",   "Tom 16\" Muffling",  -1.0f, 2.0f, 0.0f },
        { "tom3Glide",  "Tom 16\" Pitch Glide", 0.0f, 3.0f, 1.0f },
        { "tom3Stick",  "Tom 16\" Stick",     -2.0f, 2.0f, 0.0f },
        { "hatTune",    "Hi-Hat Tuning",     -24.0f, 7.0f, 0.0f },
        { "hatLevel",   "Hi-Hat Level",        0.0f, 1.5f, 1.0f },
        { "crashTune",  "Crash Tuning",      -24.0f, 7.0f, 0.0f },
        { "crashLevel", "Crash Level",         0.0f, 1.5f, 1.0f },
        { "rideTune",   "Ride Tuning",       -24.0f, 7.0f, 0.0f },
        { "rideLevel",  "Ride Level",          0.0f, 1.5f, 0.5f },
        { "close",      "Close Mics",          0.0f, 2.0f, 1.0f },
        { "overhead",   "Overheads",           0.0f, 2.0f, 1.0f },
        { "room",       "Room",                0.0f, 1.0f, 0.2f },
        { "volume",     "Volume",              0.0f, 1.5f, 0.8f },
    };

    static_assert (std::size (paramTable) == DrumModelPlugin::numParams);

    enum class Shown { semitones, log2Factor, factor, percent, wires, onOff };

    Shown shownAs (int p)
    {
        switch (p)
        {
            case P::kickTune: case P::snareTune: case P::tom1Tune: case P::tom2Tune: case P::tom3Tune:
            case P::hatTune: case P::crashTune: case P::rideTune:
                return Shown::semitones;
            case P::kickMute: case P::kickBeater: case P::snareMute: case P::snareStick:
            case P::tom1Mute: case P::tom1Stick: case P::tom2Mute: case P::tom2Stick: case P::tom3Mute: case P::tom3Stick:
                return Shown::log2Factor;
            case P::snareWires:
                return Shown::wires;
            case P::snareRim:
                return Shown::onOff;
            case P::hatLevel: case P::crashLevel: case P::rideLevel: case P::room: case P::volume:
                return Shown::percent;
            default:
                return Shown::factor;
        }
    }

    const juce::String times = juce::String::charToString (0x00d7);

    juce::String format (Shown s, float v)
    {
        switch (s)
        {
            case Shown::semitones:  return (v > 0 ? "+" : "") + juce::String (v, 1) + " st";
            case Shown::log2Factor: return times + juce::String (std::pow (2.0f, v), 2);
            case Shown::factor:     return times + juce::String (v, 2);
            case Shown::percent:    return juce::String (juce::roundToInt (v * 100.0f)) + "%";
            case Shown::wires:      return v < 0.03f ? juce::String ("off") : times + juce::String (v, 2);
            case Shown::onOff:      return v >= 0.5f ? juce::String ("on") : juce::String ("off");
        }
        return {};
    }

    float parse (Shown s, const juce::String& text)
    {
        const auto t = text.trim();
        const float number = t.retainCharacters ("0123456789.-").getFloatValue();

        switch (s)
        {
            case Shown::semitones:  return number;
            case Shown::log2Factor: return number > 0 ? std::log2 (number) : -2.0f;
            case Shown::factor:     return number;
            case Shown::percent:    return number * 0.01f;
            case Shown::wires:      return t.equalsIgnoreCase ("off") ? 0.0f : number;
            case Shown::onOff:      return t.equalsIgnoreCase ("on") || number >= 0.5f ? 1.0f : 0.0f;
        }
        return number;
    }
}

const DrumModelPlugin::ParamInfo& DrumModelPlugin::getParamInfo (int index)
{
    return paramTable[juce::jlimit (0, (int) numParams - 1, index)];
}

const std::array<DrumModelPlugin::Note, 13>& DrumModelPlugin::getNotes()
{
    using drummodel::Piece;
    static const std::array<Note, 13> notes
    { {
        { 36, Piece::kick }, { 38, Piece::snare }, { 40, Piece::rimshot },
        { 50, Piece::tomHigh }, { 48, Piece::tomMid }, { 45, Piece::tomFloor },
        { 42, Piece::hatClosed }, { 46, Piece::hatOpen }, { 44, Piece::hatPedal },
        { 49, Piece::crash }, { 52, Piece::crashChoke }, { 51, Piece::ride }, { 53, Piece::rideBell },
    } };
    return notes;
}

DrumModelPlugin::DrumModelPlugin (te::PluginCreationInfo info)
    : te::Plugin (info), kit (std::make_unique<drummodel::Kit>())
{
    auto um = getUndoManager();

    for (int i = 0; i < numParams; ++i)
    {
        const auto& p = paramTable[i];
        const auto shown = shownAs (i);
        const bool stepped = shown == Shown::onOff;
        values[(size_t) i].referTo (state, p.id, um, p.defaultValue);
        params[(size_t) i] = addParam (p.id, TRANS (p.name),
                                       stepped ? juce::NormalisableRange<float> (p.min, p.max, 1.0f)
                                               : juce::NormalisableRange<float> (p.min, p.max),
                                       [shown] (float v) { return format (shown, v); },
                                       [shown] (const juce::String& s) { return parse (shown, s); });
        params[(size_t) i]->attachToCurrentValue (values[(size_t) i]);
    }

    // Unpacking the models and settling the snare's wires takes a moment:
    // not on the message thread.
    loader = std::thread ([this]
    {
        loadState.store (kit->load() ? 1 : 2);
    });

    startTimer (500);
}

DrumModelPlugin::~DrumModelPlugin()
{
    stopTimer();
    notifyListenersOfDeletion();

    if (loader.joinable())
        loader.join();

    for (auto parameter : getAutomatableParameters())
        parameter->detachFromCurrentValue();
}

void DrumModelPlugin::waitForLoad()
{
    if (loader.joinable())
        loader.join();
}

void DrumModelPlugin::timerCallback()
{
    if (! isLoaded())
        return;

    drummodel::Kit::Strokes strokes;
    const bool allRimshots = params[(size_t) snareRim]->getCurrentValue() >= 0.5f;

    if (auto* track = dynamic_cast<te::AudioTrack*> (getOwnerTrack()))
        for (auto* clip : track->getClips())
            if (auto* midi = dynamic_cast<te::MidiClip*> (clip); midi != nullptr && ! midi->isMuted())
                for (auto* note : midi->getSequence().getNotes())
                    if (! note->isMute())
                    {
                        const auto piece = drummodel::pieceForNote (note->getNoteNumber());
                        const int index = drummodel::Kit::strokeIndex (piece, note->getVelocity() / 127.0, allRimshots);
                        if (index >= 0)
                            strokes.set ((size_t) index);
                    }

    if (strokes != keptStrokes)
    {
        keptStrokes = strokes;
        kit->keepStrokes (strokes);
    }
}

void DrumModelPlugin::restorePluginStateFromValueTree (const juce::ValueTree& v)
{
    for (auto& value : values)
    {
        const auto id = value.getPropertyID();
        if (v.hasProperty (id))
            value = (float) v.getProperty (id);
    }

    for (auto parameter : getAutomatableParameters())
        parameter->updateFromAttachedValue();
}

drummodel::Settings DrumModelPlugin::readSettings() const
{
    auto v = [this] (Param p) { return (double) params[(size_t) p]->getCurrentValue(); };
    drummodel::Settings s;
    s.kick = { v (kickTune), v (kickMute), v (kickGlide), v (kickBeater) };
    s.kickLevel = v (kickLevel);
    s.snare = { v (snareTune), v (snareMute), 1.0, v (snareStick) };
    s.snareWires = v (snareWires);
    s.snareLevel = v (snareLevel);
    s.allRimshots = v (snareRim) >= 0.5;
    s.toms[0] = { v (tom1Tune), v (tom1Mute), v (tom1Glide), v (tom1Stick) };
    s.toms[1] = { v (tom2Tune), v (tom2Mute), v (tom2Glide), v (tom2Stick) };
    s.toms[2] = { v (tom3Tune), v (tom3Mute), v (tom3Glide), v (tom3Stick) };
    s.hihat = { v (hatTune), v (hatLevel) };
    s.crash = { v (crashTune), v (crashLevel) };
    s.ride = { v (rideTune), v (rideLevel) };
    s.close = v (close);
    s.overhead = v (overhead);
    s.room = v (room);
    s.volume = v (volume);
    return s;
}

void DrumModelPlugin::initialise (const te::PluginInitialisationInfo& info)
{
    waitForLoad();

    if (isLoaded())
    {
        kit->prepare (info.sampleRate, juce::jmax (64, info.blockSizeSamples));
        kit->setSettings (readSettings());
        kit->allNotesOff();
    }
}

void DrumModelPlugin::applyToBuffer (const te::PluginRenderContext& fc)
{
    if (fc.destBuffer == nullptr)
        return;

    SCOPED_REALTIME_CHECK

    auto& buffer = *fc.destBuffer;
    const int numChannels = buffer.getNumChannels();

    if (numChannels == 0)
        return;

    buffer.clear (fc.bufferStartSample, fc.bufferNumSamples);

    if (! isLoaded())
        return;

    if (fc.bufferForMidiMessages != nullptr && fc.bufferForMidiMessages->isAllNotesOff)
        kit->allNotesOff();

    kit->setSettings (readSettings());

    auto* left = buffer.getWritePointer (0, fc.bufferStartSample);
    auto* right = numChannels > 1 ? buffer.getWritePointer (1, fc.bufferStartSample) : nullptr;
    float scratch[512];
    int pos = 0;

    auto renderTo = [&] (int to)
    {
        while (pos < to)
        {
            const int n = juce::jmin (to - pos, (int) std::size (scratch));
            kit->process (left + pos, right != nullptr ? right + pos : scratch, n);
            pos += n;
        }
    };

    if (fc.bufferForMidiMessages != nullptr)
    {
        for (const auto& m : *fc.bufferForMidiMessages)
        {
            // Seconds from the start of the block, as tracktion hands every
            // instrument its MIDI.
            renderTo (juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * sampleRate)));

            if (m.isNoteOn())
                kit->hit (drummodel::pieceForNote (m.getNoteNumber()), m.getFloatVelocity());
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                kit->allNotesOff();
        }
    }

    renderTo (fc.bufferNumSamples);

    for (int ch = 2; ch < numChannels; ++ch)
        buffer.clear (ch, fc.bufferStartSample, fc.bufferNumSamples);

    const auto a = kit->getActivity();
    activity.kick.store (a.kick, std::memory_order_relaxed);
    activity.snare.store (a.snare, std::memory_order_relaxed);
    for (int i = 0; i < 3; ++i)
        activity.toms[i].store (a.toms[i], std::memory_order_relaxed);
    activity.cymbals.store (a.cymbals, std::memory_order_relaxed);
}

} // namespace carve::plugins
