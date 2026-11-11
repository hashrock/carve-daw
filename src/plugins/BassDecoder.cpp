#include "BassDecoder.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include <juce_core/juce_core.h>

namespace carve::plugins::bass
{

namespace
{
    constexpr double silentDb = -300.0;
    constexpr double ln10over20 = 2.302585092994046 / 20.0;
    constexpr double pi = 3.14159265358979323846;
    constexpr double twoPi = 2.0 * pi;

    double jsRound (double x)  { return std::floor (x + 0.5); }   // Math.round

    // A run of RBJ peaking filters over the transient, an octave wide each.
    // Minimum phase, so nothing smears ahead of the attack.
    void applyAttackEq (float* y, int count, const std::vector<double>& gains,
                        const std::vector<double>& fc, double q, double amount, double fs)
    {
        for (size_t c = 0; c < gains.size() && c < fc.size(); ++c)
        {
            const auto g = gains[c] * amount;

            if (std::abs (g) < 0.05 || fc[c] >= 0.49 * fs)
                continue;

            const auto A = std::pow (10.0, g / 40.0), w0 = twoPi * fc[c] / fs;
            const auto al = std::sin (w0) / (2.0 * q), cw = std::cos (w0);
            const auto a0 = 1.0 + al / A;
            const auto b0 = (1.0 + al * A) / a0, b1 = -2.0 * cw / a0, b2 = (1.0 - al * A) / a0;
            const auto a1 = -2.0 * cw / a0, a2 = (1.0 - al / A) / a0;

            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

            for (int i = 0; i < count; ++i)
            {
                const double x = y[i];
                const double o = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = o;
                y[i] = (float) o;
            }
        }
    }
} // namespace

//==============================================================================
std::unique_ptr<Kit> Kit::parse (const void* bytes, std::size_t size)
{
    if (size < 8 || std::memcmp (bytes, "BAS1", 4) != 0)
        return nullptr;

    std::uint32_t hlen;
    std::memcpy (&hlen, static_cast<const char*> (bytes) + 4, 4);

    if (8 + (std::size_t) hlen > size)
        return nullptr;

    auto kit = std::make_unique<Kit>();
    kit->data.assign (static_cast<const std::uint8_t*> (bytes), static_cast<const std::uint8_t*> (bytes) + size);

    const auto header = juce::JSON::parse (juce::String::fromUTF8 ((const char*) kit->data.data() + 8, (int) hlen));

    if (! header.isObject())
        return nullptr;

    const auto base = (std::size_t) 8 + hlen;
    const auto* u8 = kit->data.data();

    auto num = [] (const juce::var& v, double fallback) { return v.isVoid() ? fallback : (double) v; };

    kit->fs = num (header["fs"], 44100.0);
    kit->tEnd = num (header["tEnd"], 6.0);
    kit->J = (int) num (header["J"], 24);
    kit->gamma = num (header["gamma"], 2.2);
    kit->P = (int) num (header["P"], 12);
    kit->envStep = num (header["envStep"], 0.4);
    kit->centStep = num (header["centStep"], 0.5);
    kit->block = (int) num (header["block"], 64);
    kit->devDiv = num (header["devDiv"], 256.0);
    kit->merge = num (header["merge"], 0.12);

    if (auto* teq = header["teq"].getDynamicObject())
    {
        if (auto* fc = teq->getProperty ("fc").getArray())
            for (const auto& f : *fc)
                kit->teqFc.push_back ((double) f);

        kit->teqQ = num (teq->getProperty ("q"), 1.41);
    }

    if (auto* m = header["midi"].getArray())
        for (const auto& v : *m)
            kit->midi.push_back ((int) v);

    if (auto* layers = header["layers"].getArray())
        kit->numLayers = layers->size();

    const int J = kit->J, P = kit->P;

    if (kit->midi.empty() || kit->numLayers < 1 || J < 2 || P < 2)
        return nullptr;

    // Transients: one int16 stream, little-endian.
    {
        const auto tpcm = header["tpcm"];
        const auto off = (std::size_t) (int) tpcm["off"], count = (std::size_t) (int) tpcm["n"];

        if (base + off + count * 2 > size)
            return nullptr;

        kit->tstream.resize (count);

        for (std::size_t i = 0; i < count; ++i)
        {
            std::int16_t v;
            std::memcpy (&v, u8 + base + off + 2 * i, 2);
            kit->tstream[i] = (float) v;
        }
    }

    kit->grid.resize ((size_t) J);
    for (int j = 0; j < J; ++j)
        kit->grid[(size_t) j] = kit->tEnd * std::pow ((double) j / (J - 1), kit->gamma);

    kit->pgrid.resize ((size_t) P);
    for (int j = 0; j < P; ++j)
        kit->pgrid[(size_t) j] = kit->tEnd * std::pow ((double) j / (P - 1), 2.5);

    // Which time points the buzz rows were stored at.
    std::vector<int> buzzJ;

    if (auto* bj = header["buzzJ"].getArray())
        for (const auto& v : *bj)
            buzzJ.push_back ((int) v);
    else
        for (int j = 0; j < J; ++j)
            buzzJ.push_back (j);

    auto* notes = header["notes"].getArray();

    if (notes == nullptr)
        return nullptr;

    kit->notes.resize ((size_t) notes->size());

    for (int i = 0; i < notes->size(); ++i)
    {
        const auto& v = notes->getReference (i);
        auto& n = kit->notes[(size_t) i];

        n.pitchIndex = (int) v["pi"];
        n.layerIndex = (int) v["li"];
        n.ref = (int) num (v["ref"], i);
        n.midi = (double) v["midi"];
        n.f0 = (double) v["f0"];
        n.B = (double) v["B"];
        n.K = (int) v["K"];
        n.top = (double) v["top"];
        n.ta = (double) v["ta"];
        n.tpos = (int) v["tpos"];
        n.tscale = (double) v["tscale"];
        n.Kb = (int) num (v["Kb"], 0);
        n.hb0 = (int) num (v["hb0"], 0);
        n.fbr = num (v["fbr"], 1.0);

        if (n.fbr == 0.0)
            n.fbr = 1.0;

        n.K = juce::jmin (n.K, maxStringPartials);

        const auto env = (std::size_t) (int) v["env"], ph = (std::size_t) (int) v["ph"],
                   dev = (std::size_t) (int) v["dev"], pitch = (std::size_t) (int) v["pitch"];

        if (base + env + (std::size_t) n.K * (std::size_t) J > size || base + ph + (std::size_t) n.K > size
             || base + dev + (std::size_t) n.K > size || base + pitch + (std::size_t) P > size)
            return nullptr;

        n.envQ = u8 + base + env;
        n.phQ = u8 + base + ph;
        n.devQ = reinterpret_cast<const std::int8_t*> (u8 + base + dev);
        n.pitchQ = reinterpret_cast<const std::int8_t*> (u8 + base + pitch);

        if (auto* teq = v["teq"].getArray())
            for (const auto& g : *teq)
                n.teq.push_back ((double) g);

        // The buzz: only the rows that exist are stored, at the buzzJ time
        // points. Spread them back out to Kb rows of J points (dB-linear in
        // between, silent if either side is).
        n.Kb = juce::jmin (n.Kb, maxBuzzPartials);
        n.envBQ.assign ((size_t) n.Kb * (size_t) J, 0);
        n.phBQ.assign ((size_t) n.Kb, 0);

        if (n.Kb > 0)
        {
            const auto bmask = (std::size_t) (int) v["bmask"], envB = (std::size_t) (int) v["envB"],
                       phB = (std::size_t) (int) v["phB"];
            const auto JB = buzzJ.size();

            for (int r = 0, c = 0; r < n.Kb; ++r)
            {
                if (base + bmask + (size_t) (r >> 3) >= size)
                    return nullptr;

                if (! ((u8[base + bmask + (size_t) (r >> 3)] >> (r & 7)) & 1))
                    continue;

                if (base + envB + (size_t) (c + 1) * JB > size || base + phB + (size_t) c >= size)
                    return nullptr;

                const auto* src = u8 + base + envB + (size_t) c * JB;

                for (int ii = 0, j = 0; j < J; ++j)
                {
                    while (buzzJ[(size_t) ii] < j)
                        ++ii;

                    int val;

                    if (buzzJ[(size_t) ii] == j)
                        val = src[ii];
                    else
                    {
                        const int a = src[ii - 1], b = src[ii];
                        val = a > 0 && b > 0 ? (int) jsRound (a + (b - a) * (double) (j - buzzJ[(size_t) ii - 1])
                                                                       / (buzzJ[(size_t) ii] - buzzJ[(size_t) ii - 1]))
                                             : 0;
                    }

                    n.envBQ[(size_t) r * (size_t) J + (size_t) j] = (std::uint8_t) val;
                }

                n.phBQ[(size_t) r] = u8[base + phB + (size_t) c];
                ++c;
            }
        }

        if ((std::size_t) n.tpos + (std::size_t) std::lround (n.ta * kit->fs) > kit->tstream.size())
            return nullptr;

        kit->maxTransientSamples = juce::jmax (kit->maxTransientSamples, (int) std::lround (n.ta * kit->fs) + 1);
    }

    kit->at.assign (kit->midi.size(), std::vector<const Note*> ((size_t) kit->numLayers, nullptr));

    for (const auto& n : kit->notes)
        if (juce::isPositiveAndBelow (n.pitchIndex, (int) kit->midi.size())
             && juce::isPositiveAndBelow (n.layerIndex, kit->numLayers))
            kit->at[(size_t) n.pitchIndex][(size_t) n.layerIndex] = &n;

    for (const auto& row : kit->at)
        for (auto* n : row)
            if (n == nullptr)
                return nullptr;

    return kit;
}

//==============================================================================
void Spec::reserve (const Kit& kit)
{
    lnEnv.resize ((size_t) maxStringPartials * (size_t) kit.J);
    dev.resize ((size_t) maxStringPartials);
    phase.resize ((size_t) maxStringPartials);
    pitchCents.resize ((size_t) kit.P);
    lnEnvB.resize ((size_t) maxBuzzPartials * (size_t) kit.J);
    phaseB.resize ((size_t) maxBuzzPartials);
}

void makeSpec (const Kit& kit, double midi, double velocity, double tune, Spec& s)
{
    const int J = kit.J, NL = kit.numLayers, numPitches = (int) kit.midi.size();

    // Two analysed pitches either side...
    int ia = 0, ib = 0;
    double wp = 0.0;

    if (midi >= kit.midi[(size_t) numPitches - 1])
        ia = ib = numPitches - 1;
    else if (midi > kit.midi[0])
    {
        for (int i = 0; i < numPitches - 1; ++i)
        {
            const int a = i, b = i + 1;

            if (midi >= kit.midi[(size_t) a] && midi <= kit.midi[(size_t) b])
            {
                ia = a; ib = b;
                wp = (midi - kit.midi[(size_t) a]) / (kit.midi[(size_t) b] - kit.midi[(size_t) a]);
                break;
            }
        }
    }

    // ...and two dynamics.
    const double lc = juce::jlimit (0.0, 1.0, velocity) * (NL - 1);
    const int la = juce::jmin (NL - 1, (int) std::floor (lc)), lb = juce::jmin (NL - 1, la + 1);
    const double wl = lc - la;

    struct Corner { const Note* note; double w; };
    std::array<Corner, 4> corners {};
    int numCorners = 0;

    for (const auto [pitchAt, w1] : { std::pair { ia, 1.0 - wp }, std::pair { ib, wp } })
        for (const auto [layerAt, w2] : { std::pair { la, 1.0 - wl }, std::pair { lb, wl } })
        {
            const double w = w1 * w2;

            if (w <= 1e-9)
                continue;

            const auto* note = kit.at[(size_t) pitchAt][(size_t) layerAt];
            bool merged = false;

            for (int c = 0; c < numCorners; ++c)
                if (corners[(size_t) c].note == note)
                {
                    corners[(size_t) c].w += w;
                    merged = true;
                    break;
                }

            if (! merged)
                corners[(size_t) numCorners++] = { note, w };
        }

    auto prim = corners[0];
    for (int c = 0; c < numCorners; ++c)
        if (corners[(size_t) c].w > prim.w + 1e-9)
            prim = corners[(size_t) c];

    const bool single = numCorners == 1;
    double cents = 0.0, B = 0.0, top = 0.0;

    for (int c = 0; c < numCorners; ++c)
    {
        const auto& [n, w] = corners[(size_t) c];
        cents += w * 1200.0 * std::log2 (n->f0 / midiHz (n->midi));
        B += w * n->B;
        top += w * n->top;
    }

    // Exactly the analysed note: its own measured f0, untouched.
    const double f0 = single && tune == 0.0 && std::equal_to<double>() (midi, prim.note->midi)
                        ? prim.note->f0
                        : midiHz (midi + tune) * std::pow (2.0, cents / 1200.0);

    auto srcF = [] (const Note& n, double h) { return h * n.f0 * std::sqrt (1.0 + n.B * h * h); };
    auto strF = [f0, B] (double h) { return h * f0 * std::sqrt (1.0 + B * h * h); };

    int K = 0;
    for (int c = 0; c < numCorners; ++c)
        K = juce::jmax (K, corners[(size_t) c].note->K);

    if (! single)
    {
        // No partials above anything the analysed notes have data for.
        double fTop = 0.0;
        for (int c = 0; c < numCorners; ++c)
            fTop = juce::jmax (fTop, srcF (*corners[(size_t) c].note, corners[(size_t) c].note->K));

        K = 0;
        while (K < 2000 && strF (K + 1) <= fTop)
            ++K;
    }

    K = juce::jmin (K, maxStringPartials);

    std::fill (s.lnEnv.begin(), s.lnEnv.begin() + K * J, 0.0);
    std::fill (s.dev.begin(), s.dev.begin() + K, 0.0);
    std::fill (s.phase.begin(), s.phase.begin() + K, 0.0);
    std::fill (s.pitchCents.begin(), s.pitchCents.end(), 0.0);

    for (int c = 0; c < numCorners; ++c)
    {
        const auto& n = *corners[(size_t) c].note;
        const auto w = corners[(size_t) c].w;

        // Interpolating, a partial one note lacks counts as very quiet.
        const double fill = single ? silentDb : n.top - 95.0;

        for (int k = 0; k < K; ++k)
        {
            for (int j = 0; j < J; ++j)
            {
                const int q = k < n.K ? n.envQ[k * J + j] : 0;
                s.lnEnv[(size_t) (k * J + j)] += w * (q > 0 ? n.top - (255 - q) * kit.envStep : fill);
            }

            if (k < n.K)
                s.dev[(size_t) k] += w * n.devQ[k] / kit.devDiv;
        }

        for (int j = 0; j < kit.P; ++j)
            s.pitchCents[(size_t) j] += w * n.pitchQ[j] * kit.centStep;
    }

    for (int i = 0; i < K * J; ++i)
        s.lnEnv[(size_t) i] *= ln10over20;

    // The buzz: exact multiples of fb = f0 * fbr, from hb0 up.
    double fbr = 0.0;
    for (int c = 0; c < numCorners; ++c)
        fbr += corners[(size_t) c].w * corners[(size_t) c].note->fbr;

    const double fb = f0 * fbr;
    int hb0 = 0, Kb = 0;

    if (single)
    {
        const auto& n = *prim.note;
        hb0 = n.hb0;
        Kb = n.Kb;

        for (int i = 0; i < Kb * J; ++i)
        {
            const int q = n.envBQ[(size_t) i];
            s.lnEnvB[(size_t) i] = (q > 0 ? n.top - (255 - q) * kit.envStep : silentDb) * ln10over20;
        }

        for (int i = 0; i < Kb; ++i)
            s.phaseB[(size_t) i] = n.phBQ[(size_t) i] / 256.0 * twoPi;
    }
    else
    {
        // Read each neighbour's buzz at the same frequency. Nothing above or
        // below what any of them has.
        double fLo = 1e300, fHi = -1e300;
        bool any = false;

        for (int c = 0; c < numCorners; ++c)
        {
            const auto& n = *corners[(size_t) c].note;

            if (n.Kb <= 0)
                continue;

            any = true;
            fLo = juce::jmin (fLo, n.hb0 * n.fbr * n.f0);
            fHi = juce::jmax (fHi, (n.hb0 + n.Kb - 1) * n.fbr * n.f0);
        }

        if (any)
        {
            hb0 = juce::jmax (1, (int) std::ceil (fLo / fb - 1e-9));
            Kb = juce::jlimit (0, maxBuzzPartials, (int) std::floor (fHi / fb + 1e-9) - hb0 + 1);

            std::fill (s.lnEnvB.begin(), s.lnEnvB.begin() + Kb * J, 0.0);

            for (int c = 0; c < numCorners; ++c)
            {
                const auto& n = *corners[(size_t) c].note;
                const auto w = corners[(size_t) c].w;
                const double fill = n.top - 95.0;
                const double fbs = n.fbr * n.f0;

                auto val = [&] (int r, int j)
                {
                    const int q = r >= 0 && r < n.Kb ? n.envBQ[(size_t) (r * J + j)] : 0;
                    return q > 0 ? n.top - (255 - q) * kit.envStep : fill;
                };

                for (int i = 0; i < Kb; ++i)
                {
                    const double x = juce::jmax (0.0, (hb0 + i) * fb / fbs - n.hb0);
                    const int r0 = (int) std::floor (x);
                    const double fr = x - r0;

                    for (int j = 0; j < J; ++j)
                        s.lnEnvB[(size_t) (i * J + j)] += w * (val (r0, j) * (1.0 - fr) + val (r0 + 1, j) * fr);
                }
            }

            for (int i = 0; i < Kb * J; ++i)
                s.lnEnvB[(size_t) i] *= ln10over20;

            // Silence buzz rows that land on a string partial: two that close beat.
            const double merge = kit.merge * f0;
            int k = 1;

            for (int i = 0; i < Kb; ++i)
            {
                const double F = (hb0 + i) * fb;

                while (k < K && strF (k + 1) < F)
                    ++k;

                const double d = juce::jmin (std::abs (F - strF (k)), std::abs (strF (k + 1) - F));

                if (d < merge)
                    for (int j = 0; j < J; ++j)
                        s.lnEnvB[(size_t) (i * J + j)] = silentDb * ln10over20;
            }

            const auto& pn = *prim.note;
            const double pfb = pn.fbr * pn.f0;

            for (int i = 0; i < Kb; ++i)
            {
                const int r = (int) jsRound ((hb0 + i) * fb / pfb) - pn.hb0;
                s.phaseB[(size_t) i] = r >= 0 && r < pn.Kb ? pn.phBQ[(size_t) r] / 256.0 * twoPi : 0.0;
            }
        }
    }

    for (int k = 0; k < juce::jmin (K, prim.note->K); ++k)
        s.phase[(size_t) k] = prim.note->phQ[k] / 256.0 * twoPi;

    s.midi = midi;
    s.f0 = f0;
    s.B = B;
    s.K = K;
    s.top = top;
    s.fbr = fbr;
    s.hb0 = hb0;
    s.Kb = Kb;
    s.prim = prim.note;
    s.ratio = f0 / prim.note->f0;
    s.transGain = prim.note->tscale * std::pow (10.0, (top - prim.note->top) / 20.0);
}

//==============================================================================
void Options::addBend (const Bend& b)
{
    if (numBends == maxBends)
    {
        // Drop the oldest: a later bend overrides it from its start anyway.
        std::move (bends.begin() + 1, bends.end(), bends.begin());
        --numBends;
    }

    bends[(size_t) numBends++] = b;
    std::sort (bends.begin(), bends.begin() + numBends, [] (const Bend& x, const Bend& y) { return x.at < y.at; });
}

//==============================================================================
void Voice::reserve (const Kit& k)
{
    spec.reserve (k);
    const auto cap = (size_t) (maxStringPartials + maxBuzzPartials);

    for (auto* v : { &fk, &gk, &mk, &phi, &lnPrev, &lnNext })
        v->resize (cap);

    for (auto* v : { &R0, &R1, &C1, &C2 })
        v->resize (cap + 4);

    on.resize (cap);
    lnEnv.resize (cap * (size_t) k.J);
    blk.resize ((size_t) k.block * 4);
    trans.resize ((size_t) k.maxTransientSamples + 1);
}

void Voice::start (const Kit& k, const Spec& source, double sampleRate, const Options& options)
{
    kit = &k;
    fs = sampleRate;
    opts = options;
    J = k.J;
    BLOCK = k.block;
    BIG = BLOCK * 4;

    // Copy the spec (only the part in use).
    spec.midi = source.midi; spec.f0 = source.f0; spec.B = source.B; spec.top = source.top;
    spec.fbr = source.fbr; spec.ratio = source.ratio; spec.transGain = source.transGain;
    spec.K = source.K; spec.hb0 = source.hb0; spec.Kb = source.Kb; spec.prim = source.prim;
    std::copy (source.lnEnv.begin(), source.lnEnv.begin() + source.K * J, spec.lnEnv.begin());
    std::copy (source.dev.begin(), source.dev.begin() + source.K, spec.dev.begin());
    std::copy (source.phase.begin(), source.phase.begin() + source.K, spec.phase.begin());
    std::copy (source.pitchCents.begin(), source.pitchCents.end(), spec.pitchCents.begin());
    std::copy (source.lnEnvB.begin(), source.lnEnvB.begin() + source.Kb * J, spec.lnEnvB.begin());
    std::copy (source.phaseB.begin(), source.phaseB.begin() + source.Kb, spec.phaseB.begin());

    KS = spec.K;
    const double buzzDb = opts.buzz;
    KB = buzzDb <= -60.0 ? 0 : spec.Kb;
    K = KS + KB;

    decay = opts.decay;
    const double bright = opts.bright, mute = opts.mute, pick = opts.pick, body = opts.body;
    const double gAll = opts.gain * ln10over20;

    step = spec.ratio * k.fs / fs;
    if (std::abs (step - 1.0) < 1e-9)
        step = 1.0;

    naS = (int) jsRound (spec.prim->ta * k.fs);
    na = step == 1.0 ? naS : (int) std::floor (naS / step);
    ta = na / fs;
    n = (int) jsRound ((ta + (k.tEnd - ta) * decay) * fs);
    if (decay == 1.0 && step == 1.0)
        n = (int) jsRound (k.tEnd * fs);
    n = juce::jmin (n, (int) jsRound (opts.dur * fs));

    auto tiltDb = [bright] (double f)
    {
        return bright != 0.0 ? juce::jlimit (-36.0, 36.0, bright * std::log2 (juce::jmax (f, 40.0) / 200.0)) : 0.0;
    };

    for (int c = 0; c < K; ++c)
    {
        const bool buzz = c >= KS;
        const int h = buzz ? spec.hb0 + (c - KS) : c + 1;
        const auto i = (size_t) c;

        fk[i] = buzz ? h * spec.f0 * spec.fbr
                     : h * spec.f0 * std::sqrt (1.0 + spec.B * h * h) + spec.dev[i] * spec.f0;
        on[i] = fk[i] < 0.45 * fs && fk[i] > 0.0 ? 1 : 0;

        double db = tiltDb (fk[i]) + (buzz ? buzzDb : 0.0);

        if (pick > 0.0 && ! buzz)
            db += 20.0 * std::log10 (juce::jmax (std::abs (std::sin (pi * h * pick)), 0.03)) + 3.0;

        if (mute > 0.0 && fk[i] > 0.0)
            db -= mute * 4.0 * juce::jmax (0.0, std::log2 (fk[i] / 150.0));

        gk[i] = db * ln10over20 + gAll;
        mk[i] = mute * (6.0 + fk[i] / 40.0);
        phi[i] = buzz ? spec.phaseB[(size_t) (c - KS)] : spec.phase[i];
    }

    std::copy (spec.lnEnv.begin(), spec.lnEnv.begin() + KS * J, lnEnv.begin());
    if (KB > 0)
        std::copy (spec.lnEnvB.begin(), spec.lnEnvB.begin() + KB * J, lnEnv.begin() + KS * J);

    // Body: the lowest string partials up or down, and when up, everything
    // down to keep the level at 0.15s where it was.
    bodyComp = 0.0;

    if (body != 0.0)
    {
        auto bodyW = [] (int h) { return h <= 2 ? 1.0 : juce::jmax (0.0, 1.0 - (h - 2) / 4.0); };

        int jr = 0;
        const double tr = juce::jmin (0.15, k.tEnd);
        while (jr < J - 2 && k.grid[(size_t) jr + 1] <= tr)
            ++jr;

        const double fr = (tr - k.grid[(size_t) jr]) / (k.grid[(size_t) jr + 1] - k.grid[(size_t) jr]);
        double e0 = 0.0, e1 = 0.0;

        for (int c = 0; c < K; ++c)
        {
            if (! on[(size_t) c])
                continue;

            const auto o = (size_t) (c * J + jr);
            const double l = lnEnv[o] + (lnEnv[o + 1] - lnEnv[o]) * fr + gk[(size_t) c];
            const double b = c < KS ? body * bodyW (c + 1) * ln10over20 : 0.0;
            e0 += std::exp (2.0 * l);
            e1 += std::exp (2.0 * (l + b));
        }

        if (body > 0.0 && e0 > 0.0)
            bodyComp = -0.5 * std::log (e1 / e0);

        for (int c = 0; c < K; ++c)
            gk[(size_t) c] += (c < KS ? body * bodyW (c + 1) * ln10over20 : 0.0) + bodyComp;
    }

    jE = 0;
    jP = 0;
    U = 0.0;

    // The transient, evened out towards its neighbours' colour if asked.
    if (opts.transient)
    {
        std::copy (k.tstream.begin() + spec.prim->tpos, k.tstream.begin() + spec.prim->tpos + naS, trans.begin());

        if (opts.attackEq > 0.0 && ! spec.prim->teq.empty())
            applyAttackEq (trans.data(), naS, spec.prim->teq, k.teqFc, k.teqQ, opts.attackEq, k.fs);
    }

    aT = spec.transGain * std::pow (10.0, (opts.attack + opts.gain) / 20.0) * std::exp (bodyComp);

    pos = 0;
    blen = BLOCK;
    fo = juce::jmin ((int) jsRound (0.03 * fs), n >> 2);
    rd = 0;
    outPos = 0;

    const int startS = juce::jmax (0, juce::jmin (n, (int) jsRound (opts.start * fs)));

    if (startS > 0)
    {
        // Advance the shared phase in exactly the blocks a voice started at 0
        // would have used, then take the envelope from there.
        for (int bl = blockLen (pos); pos + bl <= startS; bl = blockLen (pos))
        {
            if (opts.harmonics)
                U += bl * pitchAt (pos / fs) * bendAt (pos / fs) * twoPi / fs;

            pos += bl;
        }

        const int p0 = pos;

        if (opts.harmonics)
            envAt (pos / fs, lnPrev.data());

        genBlock();
        rd = startS - p0;
        outPos = startS;
    }
    else
    {
        if (opts.harmonics)
            envAt (0.0, lnPrev.data());

        blen = 0;
    }
}

void Voice::envAt (double t, double* dst)
{
    const auto& grid = kit->grid;
    const double tm = modelTime (t);

    while (jE < J - 2 && grid[(size_t) jE + 1] <= tm)
        ++jE;

    const double fr = juce::jlimit (0.0, 1.0, (tm - grid[(size_t) jE]) / (grid[(size_t) jE + 1] - grid[(size_t) jE]));

    for (int c = 0; c < K; ++c)
    {
        const auto o = (size_t) (c * J + jE);
        dst[c] = lnEnv[o] + (lnEnv[o + 1] - lnEnv[o]) * fr + gk[(size_t) c] - mk[(size_t) c] * t;
    }
}

double Voice::pitchAt (double t)
{
    const auto& pgrid = kit->pgrid;
    const double tm = modelTime (t);

    while (jP < kit->P - 2 && pgrid[(size_t) jP + 1] <= tm)
        ++jP;

    const double fr = juce::jlimit (0.0, 1.0, (tm - pgrid[(size_t) jP]) / (pgrid[(size_t) jP + 1] - pgrid[(size_t) jP]));
    const auto& pc = spec.pitchCents;
    return std::pow (2.0, (pc[(size_t) jP] + (pc[(size_t) jP + 1] - pc[(size_t) jP]) * fr) / 1200.0);
}

double Voice::bendAt (double t) const
{
    double r = 1.0;

    for (int i = 0; i < opts.numBends; ++i)
    {
        const auto& b = opts.bends[(size_t) i];

        if (t <= b.at)
            break;

        if (t >= b.at + b.dur)
        {
            r = b.ratio;
            continue;
        }

        const double u = (t - b.at) / b.dur;
        const double a = 12.0 * std::log2 (b.from), z = 12.0 * std::log2 (b.ratio);
        r = std::pow (2.0, (a + Bend::semitonesAt (u, z - a, b.fretted)) / 12.0);
    }

    return r;
}

double Bend::semitonesAt (double u, double total, bool fretted)
{
    u = juce::jlimit (0.0, 1.0, u);
    const double travelled = total * u * u * (3.0 - 2.0 * u);

    if (! fretted)
        return travelled;

    // The fret under the finger, counted from the one it left. Crossing a
    // fret wire takes the last few percent of the way to it: a slur of a
    // couple of milliseconds, not a click.
    constexpr double slur = 0.15;
    const double whole = std::floor (travelled), part = travelled - whole;
    const double x = juce::jlimit (0.0, 1.0, (part - 0.5 + slur / 2.0) / slur);
    return whole + x * x * (3.0 - 2.0 * x);
}

int Voice::blockLen (int p) const
{
    if (p < na + BLOCK)
        return BLOCK;

    const double t0 = p / fs, t1 = (p + BIG) / fs;

    for (int i = 0; i < opts.numBends; ++i)
    {
        const auto& b = opts.bends[(size_t) i];

        if (b.at < t1 + BLOCK / fs && b.at + b.dur > t0)
            return BLOCK;
    }

    return BIG;
}

void Voice::genBlock()
{
    const int BL = blen = blockLen (pos);
    std::fill (blk.begin(), blk.begin() + BL, 0.0);
    const double t0 = pos / fs;

    if (opts.harmonics)
    {
        const double pf = pitchAt (t0) * bendAt (t0) * twoPi / fs;
        envAt ((pos + BL) / fs, lnNext.data());

        // y[n] = 2 r cos(w) y[n-1] - r^2 y[n-2], the level ln-linear across
        // the block.
        int q = 0;
        double s0 = 0.0, s1 = 0.0;

        for (int c = 0; c < K; ++c)
        {
            const auto i = (size_t) c;
            const double w = fk[i] * pf, l0 = lnPrev[i], l1 = lnNext[i];

            if (! on[i] || w >= 0.9 * pi || (l0 <= -18.0 && l1 <= -18.0))
                continue;

            const double ph = std::fmod (phi[i] + fk[i] * U, twoPi);
            const double A0 = std::exp (l0), r = std::exp ((l1 - l0) / BL);
            const double y0 = A0 * std::cos (ph), y1 = A0 * r * std::cos (ph + w);
            R0[(size_t) q] = y0; R1[(size_t) q] = y1; C1[(size_t) q] = 2.0 * r * std::cos (w); C2[(size_t) q] = -r * r;
            ++q;
            s0 += y0;
            s1 += y1;
        }

        blk[0] += s0;
        blk[1] += s1;

        while (q & 3)
        {
            R0[(size_t) q] = R1[(size_t) q] = C1[(size_t) q] = C2[(size_t) q] = 0.0;
            ++q;
        }

        for (int g = 0; g < q; g += 4)
        {
            const auto G = (size_t) g;
            double a0 = R0[G], a1 = R1[G], b0 = R0[G + 1], b1 = R1[G + 1],
                   c0 = R0[G + 2], cc1 = R1[G + 2], d0 = R0[G + 3], d1 = R1[G + 3];
            const double ka = C1[G], la = C2[G], kb = C1[G + 1], lb = C2[G + 1],
                         kc = C1[G + 2], lc = C2[G + 2], kd = C1[G + 3], ld = C2[G + 3];

            for (int s = 2; s < BL; ++s)
            {
                const double ya = ka * a1 + la * a0, yb = kb * b1 + lb * b0,
                             yc = kc * cc1 + lc * c0, yd = kd * d1 + ld * d0;
                blk[(size_t) s] += (ya + yb) + (yc + yd);
                a0 = a1; a1 = ya; b0 = b1; b1 = yb; c0 = cc1; cc1 = yc; d0 = d1; d1 = yd;
            }
        }

        U += BL * pf;
        std::copy (lnNext.begin(), lnNext.begin() + K, lnPrev.begin());
    }

    if (opts.transient && pos < na)
    {
        const int m = juce::jmin (BL, na - pos);

        if (step == 1.0)
        {
            for (int s = 0; s < m; ++s)
                blk[(size_t) s] += trans[(size_t) (pos + s)] * aT;
        }
        else
        {
            // Re-read at the same ratio as the pitch moved (cubic Hermite).
            auto S = [this] (int i) { return i >= 0 && i < naS ? (double) trans[(size_t) i] : 0.0; };

            for (int s = 0; s < m; ++s)
            {
                const double x = (pos + s) * step;
                const int i0 = (int) std::floor (x);
                const double f = x - i0;
                const double y0 = S (i0 - 1), y1 = S (i0), y2 = S (i0 + 1), y3 = S (i0 + 2);
                const double c1 = 0.5 * (y2 - y0), c2 = y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3,
                             c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2);
                blk[(size_t) s] += (((c3 * f + c2) * f + c1) * f + y1) * aT;
            }
        }
    }

    if (pos + BL > n - fo)
    {
        for (int s = 0; s < BL; ++s)
        {
            const int d = n - 1 - (pos + s);

            if (d < fo)
                blk[(size_t) s] *= d < 0 ? 0.0 : 0.5 * (1.0 - std::cos (pi * d / fo));
        }
    }

    pos += BL;
}

int Voice::render (float* out, int count)
{
    const int m = juce::jmax (0, juce::jmin (count, n - outPos));

    for (int i = 0; i < m;)
    {
        if (rd == blen)
        {
            genBlock();
            rd = 0;
        }

        const int c = juce::jmin (blen - rd, m - i);

        for (int j = 0; j < c; ++j)
            out[i + j] = (float) blk[(size_t) (rd + j)];

        rd += c;
        i += c;
    }

    outPos += m;
    return m;
}

double Voice::getComponentLevelDb (int c, double t) const
{
    if (kit == nullptr || ! juce::isPositiveAndBelow (c, K))
        return silentDb;

    const auto& grid = kit->grid;
    const double tm = modelTime (t);
    int j = 0;

    while (j < J - 2 && grid[(size_t) j + 1] <= tm)
        ++j;

    const double fr = juce::jlimit (0.0, 1.0, (tm - grid[(size_t) j]) / (grid[(size_t) j + 1] - grid[(size_t) j]));
    const auto o = (size_t) (c * J + j);
    const double l = lnEnv[o] + (lnEnv[o + 1] - lnEnv[o]) * fr + gk[(size_t) c] - mk[(size_t) c] * t;
    return l / ln10over20;
}

double Voice::levelAt (double t) const
{
    if (kit == nullptr)
        return 0.0;

    const auto& grid = kit->grid;
    const double tm = modelTime (t);
    int j = 0;

    while (j < J - 2 && grid[(size_t) j + 1] <= tm)
        ++j;

    const double fr = juce::jlimit (0.0, 1.0, (tm - grid[(size_t) j]) / (grid[(size_t) j + 1] - grid[(size_t) j]));
    double e = 0.0;

    for (int c = 0; c < K; ++c)
    {
        if (! on[(size_t) c])
            continue;

        const auto o = (size_t) (c * J + j);
        const double l = lnEnv[o] + (lnEnv[o + 1] - lnEnv[o]) * fr + gk[(size_t) c] - mk[(size_t) c] * t;
        e += 0.5 * std::exp (2.0 * l);
    }

    return std::sqrt (e);
}

} // namespace carve::plugins::bass
