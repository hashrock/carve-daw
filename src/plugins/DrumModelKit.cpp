#include "DrumModelKit.h"
#include "InstrumentData.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <mutex>

// The comparisons with == and != on doubles here are exact on purpose, as in
// the PhysKit engines: flags and counts read from the model, a setting that
// changed, a target reached.
#if defined(__clang__)
 #pragma clang diagnostic ignored "-Wfloat-equal"
#elif defined(__GNUC__)
 #pragma GCC diagnostic ignored "-Wfloat-equal"
#endif

namespace carve::plugins::drummodel
{

namespace
{
    constexpr double pi = 3.14159265358979323846;

    // Gains of live.html: a v = 8 hit on the 12" tom peaks near -1dBFS, a v = 6
    // kick stroke near -1dBFS, a v = 9 snare stroke near -4dBFS.
    constexpr double tomGain = 0.0072, kickGain = 0.002, snareGain = 0.3;

    // Where the sticks land (m; x to the drummer's right, y away): the kit's
    // nominal hits (parts/toms.py, parts/snare.py, parts/kick.py).
    constexpr double tomX = -0.02, tomY = 0.01;
    constexpr double snareX = -0.03, snareY = 0.02;
    constexpr double kickX = 0.0, kickY = 0.04;

    const char* const tomModels[] { "tom_hi.bin.gz", "tom_mid.bin.gz", "tom_floor.bin.gz" };

    // The limiter's 1ms attack lets the first millisecond of a hard hit
    // through; above -1dBFS this rounds it off, to 0dBFS at most.
    float softClip (double x)
    {
        constexpr double knee = 0.891;   // -1dB
        const double a = std::abs (x);
        if (a <= knee)
            return (float) x;
        const double over = (a - knee) / (1 - knee);
        return (float) std::copysign (knee + (1 - knee) * std::tanh (over), x);
    }

    double expSpeed (double v0, double v1, double s)   { return v0 * std::pow (v1 / v0, juce::jlimit (0.0, 1.0, s)); }

    juce::MemoryBlock gunzip (const InstrumentData& d)
    {
        juce::MemoryBlock out;
        if (d.data == nullptr)
            return out;

        juce::GZIPDecompressorInputStream in (new juce::MemoryInputStream (d.data, d.size, false), true,
                                              juce::GZIPDecompressorInputStream::gzipFormat);
        juce::MemoryOutputStream mo (out, false);
        mo.writeFromInputStream (in, -1);
        return out;
    }

    // live.html's room (after kitpost.c): six early reflections and a 0.7s
    // diffuse tail, low-passed at 6kHz, one channel per side. Seeded, so a
    // render is the same every time.
    juce::AudioBuffer<float> roomImpulse (double fs)
    {
        const int n = (int) std::floor (1.2 * fs);
        juce::AudioBuffer<float> buf (2, n);
        buf.clear();
        juce::Random r (0x5eed);
        auto rnd = [&r] { return r.nextDouble() * 2 - 1; };
        const double er[][2] { { 0.0042, 0.55 }, { 0.0071, 0.35 }, { 0.0113, 0.3 }, { 0.0149, 0.25 }, { 0.0188, 0.22 }, { 0.0231, 0.18 } };

        for (int c = 0; c < 2; ++c)
        {
            auto* d = buf.getWritePointer (c);
            for (const auto& e : er)
            {
                const int i = (int) std::floor ((e[0] + rnd() * 0.0006) * fs);
                d[i] += (float) (e[1] * (r.nextBool() ? -1 : 1) * (0.8 + 0.4 * r.nextDouble()));
            }
            const double w = std::tan (pi * 6000 / fs), k1 = std::sqrt (2.0) * w, k2 = w * w, a0 = 1 + k1 + k2;
            const double b0 = k2 / a0, b1 = 2 * b0, a1 = 2 * (k2 - 1) / a0, a2 = (1 - k1 + k2) / a0;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (int i = 0; i < n; ++i)
            {
                const double t = i / fs;
                const double x = t > 0.02 ? rnd() * 1.7 * std::exp (-6.9 * t / 0.7) : 0;
                const double y = b0 * x + b1 * x1 + b0 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                d[i] += (float) (0.06 * y);
            }
        }
        return buf;
    }
}

//==============================================================================
Piece pieceForNote (int note)
{
    switch (note)
    {
        case 36: return Piece::kick;
        case 38: return Piece::snare;
        case 40: return Piece::rimshot;
        case 50: return Piece::tomHigh;
        case 48: return Piece::tomMid;
        case 45: return Piece::tomFloor;
        case 42: return Piece::hatClosed;
        case 46: return Piece::hatOpen;
        case 44: return Piece::hatPedal;
        case 49: return Piece::crash;
        case 52: return Piece::crashChoke;
        case 51: return Piece::ride;
        case 53: return Piece::rideBell;
        default: return Piece::none;
    }
}

const char* pieceName (Piece p)
{
    switch (p)
    {
        case Piece::kick:       return "Kick";
        case Piece::snare:      return "Snare";
        case Piece::rimshot:    return "Rimshot";
        case Piece::tomHigh:    return "Tom 10\"";
        case Piece::tomMid:     return "Tom 12\"";
        case Piece::tomFloor:   return "Floor Tom 16\"";
        case Piece::hatClosed:  return "Hi-Hat Closed";
        case Piece::hatOpen:    return "Hi-Hat Open";
        case Piece::hatPedal:   return "Hi-Hat Pedal";
        case Piece::crash:      return "Crash";
        case Piece::crashChoke: return "Crash Choke";
        case Piece::ride:       return "Ride";
        case Piece::rideBell:   return "Ride Bell";
        case Piece::none:       break;
    }
    return "";
}

double tomSpeed (double s)      { return expSpeed (0.4, 8.0, s); }
double snareSpeed (double s)    { return expSpeed (0.35, 9.0, s); }
double rimshotSpeed (double s)  { return expSpeed (2.5, 12.0, s); }
double kickSpeed (double s)     { return expSpeed (0.5, 6.0, s); }

//==============================================================================
// The cymbal samples (assets/instruments/modelled-drums/cymbals.txt): decoded
// once per process and shared by every kit, read-only.
struct Kit::CymbalSample
{
    juce::String articulation;
    int layer = 1, layers = 1, group = 0, offBy = 0;
    double rate = 48000;
    juce::AudioBuffer<float> audio;   // stereo
};

namespace
{
    std::shared_ptr<const std::vector<Kit::CymbalSample>> loadCymbalsOnce();
}

//==============================================================================
Kit::Kit() = default;
Kit::~Kit() = default;

bool Kit::load()
{
    loaded = false;

    for (int i = 0; i < 3; ++i)
    {
        const auto bytes = gunzip (findInstrumentData (tomModels[i]));
        if (! toms[(size_t) i].load (bytes.getData(), bytes.getSize(), tomGain))
            return false;
    }

    {
        const auto bytes = gunzip (findInstrumentData ("kick.bin.gz"));
        if (! kick.load (bytes.getData(), bytes.getSize(), kickGain))
            return false;
    }

    {
        const auto bytes = gunzip (findInstrumentData ("snare.bin.gz"));
        if (! snare.load (bytes.getData(), bytes.getSize()))
            return false;
    }

    busRate = toms[0].getModelRate();
    kickRate = kick.getModelRate();

    if (toms[1].getModelRate() != busRate || toms[2].getModelRate() != busRate || snare.getModelRate() != busRate)
        return false;

    cymbals = loadCymbalsOnce();
    if (cymbals == nullptr || cymbals->empty())
        return false;

    for (const auto& c : *cymbals)
        if (c.rate != busRate)
            return false;

    loaded = true;
    settingsChanged = true;
    applySettings (true);
    return true;
}

void Kit::prepare (double rate, int maxBlockSize)
{
    hostRate = rate;
    maxBlock = juce::jmax (16, maxBlockSize);

    busResampler.prepare (busRate, hostRate, maxBlock);
    kickResampler.prepare (kickRate, hostRate, maxBlock);

    const auto busMax = (size_t) busResampler.sourceNeeded (maxBlock) + 64;
    const auto kickMax = (size_t) kickResampler.sourceNeeded (maxBlock) + 64;
    for (auto* b : { &busL, &busR, &snareBuf }) b->assign (busMax, 0.0f);
    for (auto* b : { &kickL, &kickR }) b->assign (kickMax, 0.0f);
    for (auto* b : { &tmpL, &tmpR, &roomL, &roomR }) b->assign ((size_t) maxBlock, 0.0f);

    roomConvolution.prepare ({ hostRate, (juce::uint32) maxBlock, 2 });
    roomConvolution.loadImpulseResponse (roomImpulse (hostRate), hostRate,
                                         juce::dsp::Convolution::Stereo::yes, juce::dsp::Convolution::Trim::no,
                                         juce::dsp::Convolution::Normalise::no);
    roomConvolution.reset();

    // live.html's DynamicsCompressor on the snare and the kick, here on the
    // whole kit: -3dB, 20:1, 1ms attack, 100ms release.
    limAttack = 1 - std::exp (-1 / (0.001 * hostRate));
    limRelease = 1 - std::exp (-1 / (0.1 * hostRate));
    limiterEnv = 0;
}

void Kit::setSettings (const Settings& s)
{
    settings = s;
    settingsChanged = true;
}

void Kit::applySettings (bool force)
{
    if (! loaded || ! (settingsChanged || force))
        return;

    settingsChanged = false;
    const auto& s = settings;
    auto& a = applied;

    auto drum = [force] (ModalDrum& d, const Settings::Drum& now, Settings::Drum& was)
    {
        if (force || now.tune != was.tune)   d.setParam (0, std::pow (2.0, now.tune / 12));
        if (force || now.mute != was.mute)   d.setParam (1, std::pow (2.0, now.mute));
        if (force || now.glide != was.glide) d.setParam (2, now.glide);
        if (force || now.stick != was.stick) d.setParam (3, std::pow (2.0, now.stick));
        was = now;
    };

    const bool mics = force || s.close != a.close || s.overhead != a.overhead;

    for (size_t i = 0; i < 3; ++i)
    {
        drum (toms[i], s.toms[i], a.toms[i]);
        if (mics) { toms[i].setParam (4, s.close); toms[i].setParam (5, s.overhead); }
    }

    drum (kick, s.kick, a.kick);
    if (mics) { kick.setParam (4, s.close); kick.setParam (5, s.overhead); }

    if (force || s.snare.tune != a.snare.tune)   snare.setParam (0, std::pow (2.0, s.snare.tune / 12));
    if (force || s.snare.mute != a.snare.mute)   snare.setParam (1, std::pow (2.0, s.snare.mute));
    if (force || s.snare.stick != a.snare.stick) snare.setParam (3, std::pow (2.0, s.snare.stick));
    if (force || s.snareWires != a.snareWires)   snare.setParam (2, s.snareWires < 0.03 ? -20.0 : s.snareWires);
    if (mics) { snare.setParam (4, s.close); snare.setParam (5, s.overhead); }

    a = s;
}

//==============================================================================
void Kit::hit (Piece p, double velocity)
{
    if (! loaded)
        return;

    applySettings (false);
    velocity = juce::jlimit (0.0, 1.0, velocity);

    switch (p)
    {
        case Piece::kick:      kick.hit (kickX, kickY, kickSpeed (velocity)); break;
        case Piece::snare:
            if (settings.allRimshots) snare.hit (snareX, snareY, rimshotSpeed (velocity), true);
            else                      snare.hit (snareX, snareY, snareSpeed (velocity), false);
            break;
        case Piece::rimshot:   snare.hit (snareX, snareY, rimshotSpeed (velocity), true); break;
        case Piece::tomHigh:   toms[0].hit (tomX, tomY, tomSpeed (velocity)); break;
        case Piece::tomMid:    toms[1].hit (tomX, tomY, tomSpeed (velocity)); break;
        case Piece::tomFloor:  toms[2].hit (tomX, tomY, tomSpeed (velocity)); break;
        case Piece::hatClosed: case Piece::hatOpen: case Piece::hatPedal:
        case Piece::crash: case Piece::crashChoke: case Piece::ride: case Piece::rideBell:
            startCymbal (p, velocity);
            break;
        case Piece::none:
            break;
    }
}

void Kit::startCymbal (Piece p, double velocity)
{
    const char* art = p == Piece::hatClosed ? "hh_closed" : p == Piece::hatOpen ? "hh_open" : p == Piece::hatPedal ? "hh_pedal"
                    : p == Piece::crash ? "crash" : p == Piece::crashChoke ? "crash_choke" : p == Piece::ride ? "ride" : "ride_bell";
    const auto& set = p == Piece::hatClosed || p == Piece::hatOpen || p == Piece::hatPedal ? settings.hihat
                    : p == Piece::crash || p == Piece::crashChoke ? settings.crash : settings.ride;

    // The layer by strength, a round robin at random (live.html's playSample).
    int layers = 0;
    for (const auto& c : *cymbals)
        if (c.articulation == art) { layers = c.layers; break; }
    if (layers == 0)
        return;

    const int layer = juce::jlimit (1, layers, 1 + juce::roundToInt (velocity * (layers - 1)));
    const CymbalSample* choices[8] {};
    int numChoices = 0;
    for (const auto& c : *cymbals)
        if (c.articulation == art && c.layer == layer && numChoices < 8)
            choices[numChoices++] = &c;
    if (numChoices == 0)
        return;

    const auto* sample = choices[random.nextInt (numChoices)];

    // Chokes as the kit's SFZ off_by groups: a closed hat stops an open one,
    // the choke stops the crash.
    for (auto& v : voices)
        if (v.sample != nullptr && v.offBy != 0 && v.offBy == sample->group)
            v.choking = true;

    auto* slot = &voices[0];
    for (auto& v : voices)
    {
        if (v.sample == nullptr) { slot = &v; break; }
        if (v.gain * (float) (1.0 - v.pos / v.sample->audio.getNumSamples()) < slot->gain * (float) (1.0 - slot->pos / slot->sample->audio.getNumSamples()))
            slot = &v;
    }

    slot->sample = sample;
    slot->pos = 0;
    slot->rate = std::pow (2.0, set.tune / 12);
    slot->gain = 1;
    slot->choking = false;
    slot->group = sample->group;
    slot->offBy = sample->offBy;
    slot->bus = p == Piece::hatClosed || p == Piece::hatOpen || p == Piece::hatPedal ? 0
              : p == Piece::crash || p == Piece::crashChoke ? 1 : 2;
}

void Kit::allNotesOff()
{
    for (auto& t : toms) t.reset();
    kick.reset();
    snare.reset();
    for (auto& v : voices) v.sample = nullptr;
    busResampler.reset();
    kickResampler.reset();
    roomConvolution.reset();
    limiterEnv = 0;
}

Kit::Activity Kit::getActivity() const
{
    Activity a;
    a.kick = kick.getActiveModes();
    a.snare = snare.isAsleep() ? 0 : juce::jmax (1, snare.getActiveModes());
    for (int i = 0; i < 3; ++i) a.toms[i] = toms[(size_t) i].getActiveModes();
    for (const auto& v : voices) a.cymbals += v.sample != nullptr ? 1 : 0;
    return a;
}

//==============================================================================
void Kit::renderCymbals (float* l, float* r, int n)
{
    const double chokeStep = std::exp (-1 / (0.02 * busRate));   // setTargetAtTime (0, t, 0.02)
    const float levels[3] { (float) settings.hihat.level, (float) settings.crash.level, (float) settings.ride.level };

    for (auto& v : voices)
    {
        if (v.sample == nullptr)
            continue;

        const auto& a = v.sample->audio;
        const int len = a.getNumSamples();
        const float* s0 = a.getReadPointer (0);
        const float* s1 = a.getReadPointer (a.getNumChannels() > 1 ? 1 : 0);
        const float level = levels[v.bus];

        for (int i = 0; i < n; ++i)
        {
            const int i0 = (int) v.pos;
            if (i0 + 1 >= len || (v.choking && v.gain < 1e-4f))
            {
                v.sample = nullptr;
                break;
            }
            const float f = (float) (v.pos - i0);
            const float g = v.gain * level;
            l[i] += g * (s0[i0] + f * (s0[i0 + 1] - s0[i0]));
            r[i] += g * (s1[i0] + f * (s1[i0 + 1] - s1[i0]));
            v.pos += v.rate;
            if (v.choking)
                v.gain *= (float) chokeStep;
        }
    }
}

void Kit::renderModelBus (int m)
{
    std::fill (busL.begin(), busL.begin() + m, 0.0f);
    std::fill (busR.begin(), busR.begin() + m, 0.0f);
    std::fill (snareBuf.begin(), snareBuf.begin() + m, 0.0f);

    for (auto& t : toms)
        t.process (busL.data(), busR.data(), m);

    snare.process (snareBuf.data(), m);
    const auto g = (float) (snareGain * settings.snareLevel);
    for (int i = 0; i < m; ++i)
    {
        busL[(size_t) i] += g * snareBuf[(size_t) i];
        busR[(size_t) i] += g * snareBuf[(size_t) i];
    }

    renderCymbals (busL.data(), busR.data(), m);
}

void Kit::process (float* left, float* right, int n)
{
    if (! loaded)
    {
        std::fill (left, left + n, 0.0f);
        std::fill (right, right + n, 0.0f);
        return;
    }

    applySettings (false);

    for (int done = 0; done < n;)
    {
        const int c = juce::jmin (maxBlock, n - done);
        float* l = left + done;
        float* r = right + done;

        // the toms, the snare and the cymbals at their 48kHz
        const int m = busResampler.sourceNeeded (c);
        renderModelBus (m);
        busResampler.process (busL.data(), busR.data(), m, l, r, c);

        // the kick at its 16kHz
        const int mk = kickResampler.sourceNeeded (c);
        std::fill (kickL.begin(), kickL.begin() + mk, 0.0f);
        std::fill (kickR.begin(), kickR.begin() + mk, 0.0f);
        kick.process (kickL.data(), kickR.data(), mk);
        kickResampler.process (kickL.data(), kickR.data(), mk, tmpL.data(), tmpR.data(), c);
        const auto kg = (float) settings.kickLevel;
        for (int i = 0; i < c; ++i)
        {
            l[i] += kg * tmpL[(size_t) i];
            r[i] += kg * tmpR[(size_t) i];
        }

        // the room, and the master
        const auto room = (float) settings.room;
        if (room > 0)
        {
            for (int i = 0; i < c; ++i)
            {
                roomL[(size_t) i] = room * l[i];
                roomR[(size_t) i] = room * r[i];
            }
            float* ch[] { roomL.data(), roomR.data() };
            juce::dsp::AudioBlock<float> block (ch, 2, (size_t) c);
            roomConvolution.process (juce::dsp::ProcessContextReplacing<float> (block));
            for (int i = 0; i < c; ++i)
            {
                l[i] += roomL[(size_t) i];
                r[i] += roomR[(size_t) i];
            }
        }

        const auto vol = settings.volume;
        constexpr double threshold = -3.0, ratio = 20.0;
        for (int i = 0; i < c; ++i)
        {
            const double x = vol * l[i], y = vol * r[i];
            const double peak = juce::jmax (std::abs (x), std::abs (y));
            const double over = peak > 0 ? 20 * std::log10 (peak) - threshold : -100.0;
            const double target = over > 0 ? over * (1 - 1 / ratio) : 0;
            limiterEnv += (target > limiterEnv ? limAttack : limRelease) * (target - limiterEnv);
            const double g = limiterEnv > 1e-6 ? std::pow (10.0, -limiterEnv / 20) : 1.0;
            l[i] = softClip (x * g);
            r[i] = softClip (y * g);
        }

        done += c;
    }
}

//==============================================================================
namespace
{
    std::shared_ptr<const std::vector<Kit::CymbalSample>> loadCymbalsOnce()
    {
        static std::mutex lock;
        static std::weak_ptr<const std::vector<Kit::CymbalSample>> cache;
        const std::lock_guard<std::mutex> guard (lock);

        if (auto existing = cache.lock())
            return existing;

        const auto index = findInstrumentData ("cymbals.txt");
        if (index.data == nullptr)
            return nullptr;

        auto list = std::make_shared<std::vector<Kit::CymbalSample>>();
        juce::OggVorbisAudioFormat ogg;
        const auto lines = juce::StringArray::fromLines (juce::String::fromUTF8 (static_cast<const char*> (index.data), (int) index.size));

        for (const auto& line : lines)
        {
            const auto f = juce::StringArray::fromTokens (line, " ", "");
            if (f.size() < 7)
                continue;

            const auto file = findInstrumentData (f[6].toRawUTF8());
            if (file.data == nullptr)
                return nullptr;

            std::unique_ptr<juce::AudioFormatReader> reader (ogg.createReaderFor (new juce::MemoryInputStream (file.data, file.size, false), true));
            if (reader == nullptr || reader->lengthInSamples <= 1)
                return nullptr;

            Kit::CymbalSample s;
            s.articulation = f[0];
            s.layer = f[1].getIntValue();
            s.layers = f[2].getIntValue();
            s.group = f[4].getIntValue();
            s.offBy = f[5].getIntValue();
            s.rate = reader->sampleRate;
            s.audio.setSize (2, (int) reader->lengthInSamples + 1);
            s.audio.clear();
            reader->read (&s.audio, 0, (int) reader->lengthInSamples, 0, true, true);
            list->push_back (std::move (s));
        }

        cache = list;
        return list;
    }
}

//==============================================================================
// The resampler: a Kaiser-windowed sinc, 16 zero crossings either side (more
// when it decimates), the passband to 94% of the lower Nyquist.
void Resampler::prepare (double sourceRate, double targetRate, int maxOutputBlock)
{
    maxOut = juce::jmax (1, maxOutputBlock);
    ratio = sourceRate / targetRate;
    bypass = std::abs (ratio - 1.0) < 1e-12;
    const double scale = juce::jmin (1.0, 1.0 / ratio);
    halfWidth = bypass ? 0 : (int) std::ceil (16.0 / scale);

    if (! bypass)
    {
        const double fc = 0.47 * scale;   // cycles per source sample
        constexpr int perSample = 512;
        step = perSample;
        table.assign ((size_t) (halfWidth * perSample + 2), 0.0f);
        const double beta = 8.0;
        auto bessel0 = [] (double x)
        {
            double sum = 1, term = 1;
            for (int k = 1; k < 40; ++k)
            {
                term *= (x / (2 * k)) * (x / (2 * k));
                sum += term;
            }
            return sum;
        };
        const double i0b = bessel0 (beta);
        for (size_t j = 0; j < table.size(); ++j)
        {
            const double u = (double) j / perSample;
            const double t = u / halfWidth;
            const double win = t < 1 ? bessel0 (beta * std::sqrt (1 - t * t)) / i0b : 0;
            const double x = 2 * fc * u;
            const double sinc = x == 0 ? 1 : std::sin (pi * x) / (pi * x);
            table[j] = (float) (2 * fc * sinc * win);
        }
    }

    reset();
}

void Resampler::reset()
{
    // the history starts with halfWidth + 2 samples of silence before time 0
    histSize = halfWidth + 2;
    histL.assign ((size_t) (2 * halfWidth + 8 + (int) std::ceil (maxOut * ratio) + 64), 0.0f);
    histR.assign (histL.size(), 0.0f);
    pushed = 0;
    base = 0;
    frac = 0;
}

int Resampler::sourceNeeded (int n) const
{
    if (bypass)
        return n;

    // the last output's time, and the newest source sample it reads
    const double last = (double) base + frac + (double) (n - 1) * ratio;
    const auto needed = (std::int64_t) std::floor (last) + halfWidth + 1;
    return (int) juce::jmax<std::int64_t> (0, needed - pushed);
}

void Resampler::process (const float* inL, const float* inR, int numIn, float* outL, float* outR, int n)
{
    if (bypass)
    {
        std::copy (inL, inL + n, outL);
        std::copy (inR, inR + n, outR);
        return;
    }

    // hist[k] holds source sample (pushed - histSize + k)
    if ((size_t) (histSize + numIn) > histL.size())
    {
        // (only if a caller asks for more than prepare said: not realtime-safe, but correct)
        histL.resize ((size_t) (histSize + numIn + 64));
        histR.resize (histL.size());
    }
    std::copy (inL, inL + numIn, histL.begin() + histSize);
    std::copy (inR, inR + numIn, histR.begin() + histSize);
    histSize += numIn;
    pushed += numIn;

    const std::int64_t first = pushed - histSize;   // the source index of hist[0]
    const float* table_ = table.data();
    const int W = halfWidth;

    for (int j = 0; j < n; ++j)
    {
        const std::int64_t i0 = base;   // floor of the time
        const double f = frac;
        const auto* hl = histL.data() + (i0 - first);
        const auto* hr = histR.data() + (i0 - first);
        float sl = 0, sr = 0;

        // taps i0 - W + 1 .. i0 + W, at distances f + W - 1 .. -(W - f)
        for (int t = -W + 1; t <= W; ++t)
        {
            const double u = std::abs (f - t) * step;
            const auto iu = (size_t) u;
            const float fu = (float) (u - (double) iu);
            const float h = table_[iu] + fu * (table_[iu + 1] - table_[iu]);
            sl += h * hl[t];
            sr += h * hr[t];
        }
        outL[j] = sl;
        outR[j] = sr;

        frac += ratio;
        const double whole = std::floor (frac);
        base += (std::int64_t) whole;
        frac -= whole;
    }

    // keep what the next output still reads: from base - W + 1 on (and a margin)
    const std::int64_t keepFrom = juce::jmax<std::int64_t> (first, base - W - 1);
    const auto drop = (int) (keepFrom - first);
    if (drop > 0)
    {
        std::copy (histL.begin() + drop, histL.begin() + histSize, histL.begin());
        std::copy (histR.begin() + drop, histR.begin() + histSize, histR.begin());
        histSize -= drop;
    }
}


} // namespace carve::plugins::drummodel
