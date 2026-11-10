// What the signal path does to a signal, measured the way an export would
// make it: each case is a small song built around a test file, rendered by
// carve-render's own render, read back and compared with the source.
//
// The source file's layout (seconds):
//
//   0.5          impulse, left only       latency, to the sample
//   1.0          impulse, right only
//   1.5 - 3.5    1kHz sine, -23dBFS RMS   gain, distortion
//   3.5 - 4.5    15kHz sine, -23dBFS RMS  the top of the band
//   4.5 - 6.0    pink noise, -18dBFS RMS  an ordinary track level
//   6.0 - 7.0    1kHz sine, -3dBFS RMS    loud enough to compress
//   7.0 - 8.0    3kHz sine, -23dBFS RMS   an EQ band
//
// Each case below records why it is here. Most of them are things that were
// wrong once: a fresh synth's filter, the resampler's wobble, the pitch
// shifter's latency, the pan law, an insert's jump in level.

#include "SignalPathChecks.h"

#include <cstdio>
#include <vector>

namespace carve::test
{
namespace
{

constexpr double signalLength = 10.0;   // the bursts run to about 8.5s
constexpr float quietAmplitude = 0.1f;     // -20dBFS peak, -23dBFS RMS
constexpr float loudAmplitude = 0.999f;

//==============================================================================
// Test files

bool writeWav (const juce::File& file, const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());

    if (out == nullptr)
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (out.get(), sampleRate, 2, 32, {}, 0));

    if (writer == nullptr)
        return false;

    out.release();
    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}

std::optional<juce::AudioBuffer<float>> readWav (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr)
        return std::nullopt;

    juce::AudioBuffer<float> buffer (2, (int) reader->lengthInSamples);
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
    return buffer;
}

juce::AudioBuffer<float> makeSource (double sr)
{
    juce::AudioBuffer<float> b (2, (int) (signalLength * sr));
    b.clear();

    auto at = [sr] (double seconds) { return (int) std::lround (seconds * sr); };

    b.setSample (0, at (0.5), 0.5f);
    b.setSample (1, at (1.0), 0.5f);

    auto sine = [&] (double from, double to, double freq, float amplitude)
    {
        for (int i = at (from); i < at (to); ++i)
            for (int ch = 0; ch < 2; ++ch)
                b.setSample (ch, i, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / sr));
    };

    sine (1.5, 3.5, 1000.0, quietAmplitude);
    sine (3.5, 4.5, 15000.0, quietAmplitude);
    sine (6.0, 7.0, 1000.0, loudAmplitude);
    sine (7.0, 8.0, 3000.0, quietAmplitude);

    // Pink noise at -18dBFS RMS, independent per side: closer to where a
    // mix keeps its energy than white noise is, which matters to anything
    // with a tone control.
    juce::Random random (42);

    for (int ch = 0; ch < 2; ++ch)
    {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, sum = 0;   // Paul Kellet's filter
        std::vector<double> pink;

        for (int i = at (4.5); i < at (6.0); ++i)
        {
            const double white = random.nextDouble() * 2.0 - 1.0;
            b0 = 0.99886 * b0 + white * 0.0555179;  b1 = 0.99332 * b1 + white * 0.0750759;
            b2 = 0.96900 * b2 + white * 0.1538520;  b3 = 0.86650 * b3 + white * 0.3104856;
            b4 = 0.55000 * b4 + white * 0.5329522;  b5 = -0.7616 * b5 - white * 0.0168980;
            pink.push_back (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362);
            b6 = white * 0.115926;
            sum += pink.back() * pink.back();
        }

        const auto scale = 0.1259 / std::sqrt (sum / (double) pink.size());

        for (size_t i = 0; i < pink.size(); ++i)
            b.setSample (ch, at (4.5) + (int) i, (float) (pink[i] * scale));
    }

    return b;
}

// Where the bursts start: from 1s, 0.3-0.5s apart. Uneven, because evenly
// spaced bursts meet a time stretcher's frame grid at the same phase every
// time, and the median timing comes out biased by that one phase.
constexpr int numBursts = 16;

int burstStart (int burst, double sr)
{
    juce::Random random (11);
    double t = 1.0;

    for (int b = 0; b < burst; ++b)
        t += 0.3 + 0.2 * random.nextDouble();

    return (int) std::lround (t * sr);
}

// 30ms noise bursts: something a pitch shifter changes the pitch of but not
// the timing of.
juce::AudioBuffer<float> makeBursts (double sr)
{
    juce::AudioBuffer<float> b (2, (int) (signalLength * sr));
    b.clear();
    juce::Random random (7);
    const int length = (int) (0.03 * sr);

    for (int burst = 0; burst < numBursts; ++burst)
    {
        const int start = burstStart (burst, sr);

        for (int i = 0; i < length; ++i)
        {
            const auto window = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (length - 1));
            const auto v = (random.nextFloat() * 2.0f - 1.0f) * 0.3f * window;
            b.setSample (0, start + i, v);
            b.setSample (1, start + i, v);
        }
    }

    return b;
}

//==============================================================================
// Measurements

struct SineFit
{
    double amplitude = 0.0;
    double residualDb = 0.0;   // everything that is not the sine, against the sine
};

// Least squares on sin and cos at a known frequency over [t0, t1).
SineFit fitSine (const juce::AudioBuffer<float>& b, int ch, double sr, double t0, double t1, double freq)
{
    const int from = (int) (t0 * sr), to = juce::jmin (b.getNumSamples(), (int) (t1 * sr));
    const auto w = juce::MathConstants<double>::twoPi * freq / sr;
    double ss = 0, sc = 0, cc = 0, xs = 0, xc = 0;

    for (int i = from; i < to; ++i)
    {
        const auto s = std::sin (w * i), c = std::cos (w * i), x = (double) b.getSample (ch, i);
        ss += s * s; sc += s * c; cc += c * c; xs += x * s; xc += x * c;
    }

    const auto det = ss * cc - sc * sc;

    if (to <= from || det == 0.0)
        return {};

    const auto a = (xs * cc - xc * sc) / det, bc = (xc * ss - xs * sc) / det;
    double residual = 0;

    for (int i = from; i < to; ++i)
    {
        const auto e = (double) b.getSample (ch, i) - a * std::sin (w * i) - bc * std::cos (w * i);
        residual += e * e;
    }

    SineFit fit;
    fit.amplitude = std::sqrt (a * a + bc * bc);
    const auto residualRms = std::sqrt (residual / (to - from));
    fit.residualDb = 20.0 * std::log10 (juce::jmax (1.0e-15, residualRms) / juce::jmax (1.0e-15, fit.amplitude / std::sqrt (2.0)));
    return fit;
}

double toDb (double ratio)  { return 20.0 * std::log10 (juce::jmax (1.0e-12, ratio)); }

double rms (const juce::AudioBuffer<float>& b, int ch, double sr, double t0, double t1)
{
    const int from = (int) (t0 * sr), to = juce::jmin (b.getNumSamples(), (int) (t1 * sr));
    double sum = 0;

    for (int i = from; i < to; ++i)
        sum += (double) b.getSample (ch, i) * b.getSample (ch, i);

    return to > from ? std::sqrt (sum / (to - from)) : 0.0;
}

// Where the loudest sample within 50ms of `expected` is, relative to it.
int impulseOffset (const juce::AudioBuffer<float>& b, int ch, double sr, double expected)
{
    const int centre = (int) std::lround (expected * sr), span = (int) (0.05 * sr);
    int best = centre;

    for (int i = juce::jmax (0, centre - span); i < juce::jmin (b.getNumSamples(), centre + span); ++i)
        if (std::abs (b.getSample (ch, i)) > std::abs (b.getSample (ch, best)))
            best = i;

    return best - centre;
}

// Median, over the bursts, of how far the output's energy envelope sits from
// the input's: a timing that survives a change of pitch.
int medianBurstLag (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out, double sr)
{
    auto envelope = [sr] (const juce::AudioBuffer<float>& x)
    {
        const int w = (int) (0.002 * sr);
        std::vector<double> e ((size_t) x.getNumSamples(), 0.0);
        double acc = 0;

        for (int i = 0; i < x.getNumSamples(); ++i)
        {
            acc += (double) x.getSample (0, i) * x.getSample (0, i);

            if (i >= w)
                acc -= (double) x.getSample (0, i - w) * x.getSample (0, i - w);

            e[(size_t) i] = acc;
        }

        return e;
    };

    const auto ein = envelope (in), eout = envelope (out);
    const int length = (int) (0.03 * sr), maxLag = (int) (0.1 * sr);
    std::vector<int> lags;

    for (int burst = 0; burst < numBursts; ++burst)
    {
        const int start = burstStart (burst, sr);
        int best = 0;
        double bestScore = -1;

        for (int lag = -maxLag; lag <= maxLag; ++lag)
        {
            double score = 0;

            for (int i = start; i < start + length; i += 2)
                if (i + lag >= 0 && i + lag < (int) eout.size())
                    score += ein[(size_t) i] * eout[(size_t) (i + lag)];

            if (score > bestScore)
                bestScore = score, best = lag;
        }

        lags.push_back (best);
    }

    std::sort (lags.begin(), lags.end());
    return lags[lags.size() / 2];
}

//==============================================================================
// Songs

juce::String audioGenerator (const juce::String& id, float volumeDb, const juce::String& attributes = {},
                             const juce::String& children = {})
{
    return "<GENERATOR id=\"" + id + "\" name=\"" + id + "\" type=\"audio\" volumeDb=\"" + juce::String (volumeDb)
           + "\"" + attributes + ">" + children + "</GENERATOR>";
}

juce::String audioClip (const juce::String& generatorId, const juce::File& file)
{
    return "<AUDIOCLIP id=\"clip-" + generatorId + "\" generatorId=\"" + generatorId + "\" file=\""
           + file.getFullPathName() + "\" start=\"0.0\" length=\"" + juce::String (signalLength) + "\" offset=\"0.0\"/>";
}

juce::String effect (const juce::String& type, const juce::String& state = {})
{
    return "<EFFECTS><EFFECT id=\"fx-" + type + "\" type=\"" + type + "\">"
           + (state.isEmpty() ? juce::String() : "<PLUGIN type=\"" + type + "\" " + state + "/>")
           + "</EFFECT></EFFECTS>";
}

// One audio generator per entry, each playing `file`, plus whatever buses
// the case needs. Master at 0dB, so a gain is the path's own.
struct Track
{
    juce::String id;
    float volumeDb = 0.0f;
    juce::String attributes, children;
};

std::optional<model::Song> makeSong (const std::vector<Track>& tracks, const juce::File& file,
                                     const juce::String& songAttributes = {}, float masterDb = 0.0f,
                                     const juce::String& buses = {}, const juce::String& masterChildren = {})
{
    juce::String generators, clips;

    for (const auto& t : tracks)
    {
        generators << audioGenerator (t.id, t.volumeDb, t.attributes, t.children);
        clips << audioClip (t.id, file);
    }

    return model::Song::fromXml ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SONG name=\"check\" tempo=\"120.0\""
                                 + songAttributes + ">" + buses
                                 + "<MASTER volumeDb=\"" + juce::String (masterDb) + "\">" + masterChildren + "</MASTER>"
                                 + "<GENERATORS>" + generators + "</GENERATORS><PLAYLIST>" + clips
                                 + "</PLAYLIST></SONG>");
}

//==============================================================================

struct Report
{
    int failures = 0;

    void check (const juce::String& name, double measured, double low, double high, const juce::String& unit,
                const juce::String& expectation)
    {
        const bool ok = measured >= low && measured <= high;
        failures += ok ? 0 : 1;
        std::printf ("  %-4s %-58s %+10.2f %-4s (%s)\n", ok ? "ok" : "FAIL", name.toRawUTF8(), measured,
                     unit.toRawUTF8(), expectation.toRawUTF8());
    }

    void fail (const juce::String& name, const juce::String& why)
    {
        ++failures;
        std::printf ("  FAIL %-58s %s\n", name.toRawUTF8(), why.toRawUTF8());
    }
};

} // namespace

int runSignalPathChecks (RenderSong render)
{
    Report report;
    const juce::TemporaryFile scratchDir;
    const auto dir = scratchDir.getFile();
    dir.createDirectory();

    const auto source44 = dir.getChildFile ("source44.wav");
    const auto source48 = dir.getChildFile ("source48.wav");
    const auto bursts44 = dir.getChildFile ("bursts44.wav");
    const auto ref44 = makeSource (44100.0);
    const auto ref48 = makeSource (48000.0);
    const auto refBursts = makeBursts (44100.0);

    if (! writeWav (source44, ref44, 44100.0) || ! writeWav (source48, ref48, 48000.0)
         || ! writeWav (bursts44, refBursts, 44100.0))
    {
        report.fail ("test files", "could not write to " + dir.getFullPathName());
        return report.failures;
    }

    int caseNumber = 0;

    // Renders and reads back; a failure to do either is a failure of the check.
    auto renderSong = [&] (const juce::String& name, const std::optional<model::Song>& song,
                           double rate = 44100.0) -> std::optional<juce::AudioBuffer<float>>
    {
        if (! song)
        {
            report.fail (name, "the test song did not parse");
            return std::nullopt;
        }

        const auto out = dir.getChildFile ("case" + juce::String (++caseNumber) + ".wav");

        if (! render (*song, out, rate))
        {
            report.fail (name, "render failed");
            return std::nullopt;
        }

        auto buffer = readWav (out);

        if (! buffer)
            report.fail (name, "could not read the render back");

        return buffer;
    };

    auto gain1k = [] (const juce::AudioBuffer<float>& b, int ch, double sr = 44100.0)
    {
        return toDb (fitSine (b, ch, sr, 1.6, 3.4, 1000.0).amplitude / quietAmplitude);
    };

    std::printf ("\nSignal path checks\n\n");

    //--------------------------------------------------------------------------
    // Mixer: unity, faders, buses, sums
    if (auto out = renderSong ("unity", makeSong ({ { "a", 0.0f } }, source44)))
    {
        float maxDiff = 0.0f;

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < ref44.getNumSamples(); ++i)
                maxDiff = juce::jmax (maxDiff, std::abs (out->getSample (ch, i) - ref44.getSample (ch, i)));

        report.check ("unity: largest difference from the source, in 24-bit steps", maxDiff * 8388608.0, 0.0, 1.0,
                      "lsb", "the file as it went in");
        report.check ("unity: impulse offset, left", impulseOffset (*out, 0, 44100.0, 0.5), 0, 0, "smp", "no delay");
        report.check ("unity: impulse offset, right", impulseOffset (*out, 1, 44100.0, 1.0), 0, 0, "smp", "no delay");
        report.check ("unity: right impulse leaking into the left", toDb (std::abs (out->getSample (0, 44100)) / 0.5),
                      -300.0, -140.0, "dB", "none");
    }

    struct GainCase { juce::String name; std::vector<Track> tracks; float masterDb; juce::String buses; double expected; };

    const std::vector<GainCase> gainCases {
        { "generator fader -12dB",                { { "a", -12.0f } },                    0.0f, {}, -12.0 },
        { "generator fader +6dB",                 { { "a", 6.0f } },                      0.0f, {}, 6.0 },
        { "generator fader -1.5dB",               { { "a", -1.5f } },                     0.0f, {}, -1.5 },
        { "master fader -6dB",                    { { "a", 0.0f } },                      -6.0f, {}, -6.0 },
        { "group bus at -6dB",                    { { "a", 0.0f, " groupId=\"grp\"" } },  0.0f,
          "<GROUPS><GROUP id=\"grp\" name=\"Grp\" volumeDb=\"-6\"/></GROUPS>", -6.0 },
        { "send 0dB to a return at 0dB, plus the dry",
          { { "a", 0.0f, {}, "<SENDS><SEND returnId=\"ret\" gainDb=\"0\"/></SENDS>" } }, 0.0f,
          "<RETURNS><RETURN id=\"ret\" name=\"R\" busNumber=\"1\" volumeDb=\"0\"/></RETURNS>", 20.0 * std::log10 (2.0) },
        { "two tracks of the same file",          { { "a", 0.0f }, { "b", 0.0f } },       0.0f, {}, 20.0 * std::log10 (2.0) },
    };

    for (const auto& c : gainCases)
        if (auto out = renderSong (c.name, makeSong (c.tracks, source44, {}, c.masterDb, c.buses)))
            for (int ch = 0; ch < 2; ++ch)
                report.check (c.name + (ch == 0 ? ", left" : ", right"), gain1k (*out, ch),
                              c.expected - 0.02, c.expected + 0.02, "dB",
                              juce::String (c.expected, 2) + " dB");

    //--------------------------------------------------------------------------
    // Pan laws. A song saved before the choice existed keeps the linear law it
    // was mixed with; a new one pans at constant power, unity in the centre.
    struct PanCase { juce::String law; float pan; double left, right; };

    const std::vector<PanCase> panCases {
        { {},           -0.5f, toDb (1.5), toDb (0.5) },
        { {},           1.0f,  -300.0,     toDb (2.0) },
        { "equalPower", -0.5f, toDb (std::sqrt (2.0) * std::cos (0.25 * juce::MathConstants<double>::halfPi)),
                               toDb (std::sqrt (2.0) * std::sin (0.25 * juce::MathConstants<double>::halfPi)) },
        { "equalPower", 0.0f,  0.0,        0.0 },
        { "equalPower", 1.0f,  -300.0,     toDb (std::sqrt (2.0)) },
    };

    for (const auto& c : panCases)
    {
        const auto law = c.law.isEmpty() ? juce::String ("linear (older songs)") : c.law;
        const auto name = "pan " + juce::String (c.pan, 1) + ", " + law;
        const auto attributes = c.law.isEmpty() ? juce::String() : " panLaw=\"" + c.law + "\"";

        if (auto out = renderSong (name, makeSong ({ { "a", 0.0f, " pan=\"" + juce::String (c.pan) + "\"" } },
                                                   source44, attributes)))
        {
            const double expected[] { c.left, c.right };

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto low = expected[ch] < -200.0 ? -400.0 : expected[ch] - 0.02;
                const auto high = expected[ch] < -200.0 ? -120.0 : expected[ch] + 0.02;
                report.check (name + (ch == 0 ? ", left" : ", right"), gain1k (*out, ch), low, high, "dB",
                              expected[ch] < -200.0 ? juce::String ("silent") : juce::String (expected[ch], 2) + " dB");
            }

            if (c.law == "equalPower")
            {
                const auto power = (std::pow (10.0, gain1k (*out, 0) / 10.0) + std::pow (10.0, gain1k (*out, 1) / 10.0)) / 2.0;
                report.check (name + ", total power", 10.0 * std::log10 (power), -0.02, 0.02, "dB", "constant");
            }
        }
    }

    //--------------------------------------------------------------------------
    // Resampling: a file at another rate than the render, both ways. The
    // Lagrange path this replaced left sidebands at -37dB and the top octave
    // 2-5dB down.
    struct RateCase { juce::String name; juce::File file; double renderRate; };

    for (const auto& c : { RateCase { "48kHz file in a 44.1kHz render", source48, 44100.0 },
                           RateCase { "44.1kHz file in a 48kHz render", source44, 48000.0 } })
    {
        if (auto out = renderSong (c.name, makeSong ({ { "a", 0.0f } }, c.file), c.renderRate))
        {
            const auto sr = c.renderRate;
            report.check (c.name + ": 1kHz gain", toDb (fitSine (*out, 0, sr, 1.6, 3.4, 1000.0).amplitude / quietAmplitude),
                          -0.02, 0.02, "dB", "unity");
            report.check (c.name + ": 1kHz distortion and noise", fitSine (*out, 0, sr, 1.6, 3.4, 1000.0).residualDb,
                          -400.0, -100.0, "dB", "below -100dB");
            report.check (c.name + ": 15kHz gain", toDb (fitSine (*out, 0, sr, 3.6, 4.4, 15000.0).amplitude / quietAmplitude),
                          -0.1, 0.1, "dB", "unity");
            report.check (c.name + ": impulse offset", impulseOffset (*out, 0, sr, 0.5), -1, 1, "smp", "no delay");
        }
    }

    //--------------------------------------------------------------------------
    // Pitch shifter. tracktion's reported a fixed guess for its latency, so it
    // played up to 30ms early and combed against anything parallel to it.
    // Ours (plugins/PitchShifterPlugin) reports what the stretcher says, and
    // is asked for under the old type name, as a saved song would ask.
    if (auto out = renderSong ("pitch shifter at 0, beside the same file dry",
                               makeSong ({ { "a", 0.0f }, { "b", 0.0f, {}, effect ("pitchShifter") } }, source44)))
        report.check ("pitch shifter at 0, beside the same file dry: 1kHz sum", gain1k (*out, 0),
                      20.0 * std::log10 (2.0) - 0.1, 20.0 * std::log10 (2.0) + 0.1, "dB", "+6.02 dB, in phase");

    for (const auto semis : { -24, -19, -12, -5, -1, 3, 7, 12, 19, 24 })
    {
        const auto name = "pitch shifter at " + juce::String (semis) + ": timing of noise bursts";

        if (auto out = renderSong (name, makeSong ({ { "a", 0.0f, {}, effect ("pitchShifter", "semitonesUp=\""
                                                                                           + juce::String (semis) + "\"") } },
                                                   bursts44)))
            report.check (name, medianBurstLag (refBursts, *out, 44100.0) / 44.1, -6.0, 6.0, "ms",
                          "within the stretcher's own jitter");
    }

    //--------------------------------------------------------------------------
    // Inserts. A fresh Saturation or Distortion used to make an ordinary track
    // ~10dB louder the moment it went in.
    for (const auto& type : { juce::String ("carveSaturation"), juce::String ("carveDistortion") })
    {
        const auto name = type.fromFirstOccurrenceOf ("carve", false, false) + " as inserted, on a -18dBFS RMS track";

        if (auto out = renderSong (name, makeSong ({ { "a", 0.0f, {}, effect (type) } }, source44)))
            report.check (name, toDb (rms (*out, 0, 44100.0, 4.6, 5.9) / rms (ref44, 0, 44100.0, 4.6, 5.9)),
                          -1.5, 1.5, "dB", "about unity");
    }

    if (auto out = renderSong ("compressor", makeSong ({ { "a", 0.0f, {}, effect ("compressor", "threshold=\"0.1\" ratio=\"0\"") } },
                                                       source44)))
    {
        report.check ("compressor -20dB, inf:1: a -3dBFS RMS sine comes out at",
                      toDb (rms (*out, 0, 44100.0, 6.3, 6.9)), -40.0, -15.0, "dBFS", "held near -20");
        report.check ("compressor -20dB, inf:1: a -23dBFS RMS sine (under it)", gain1k (*out, 0), -0.5, 0.5, "dB",
                      "untouched");
    }

    if (auto out = renderSong ("4-band EQ", makeSong ({ { "a", 0.0f, {}, effect ("4bandEq", "midGain1=\"6\" midFreq1=\"3000\"") } },
                                                      source44)))
        report.check ("4-band EQ +6dB at 3kHz: 3kHz gain",
                      toDb (fitSine (*out, 0, 44100.0, 7.1, 7.9, 3000.0).amplitude / quietAmplitude), 5.5, 6.5, "dB",
                      "+6 dB");

    //--------------------------------------------------------------------------
    // A fresh synth: a square wave that starts at once. Its first defaults
    // once never reached the voices, leaving it behind a 440Hz low pass with
    // a 100ms attack.
    {
        const auto song = model::Song::fromXml (
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SONG name=\"synth\" tempo=\"120.0\"><MASTER volumeDb=\"0\"/>"
            "<GENERATORS><GENERATOR id=\"s\" name=\"S\" type=\"internal-synth\" volumeDb=\"0\"><PATTERNS>"
            "<PATTERN id=\"p\" name=\"P\" lengthBeats=\"4.0\"><NOTE start=\"0.0\" length=\"2.0\" pitch=\"57\" velocity=\"127\"/>"
            "</PATTERN></PATTERNS></GENERATOR></GENERATORS>"
            "<PLAYLIST><CLIP id=\"c\" generatorId=\"s\" patternId=\"p\" start=\"0.0\"/></PLAYLIST></SONG>");

        if (auto out = renderSong ("fresh synth", song))
        {
            const auto fundamental = fitSine (*out, 0, 44100.0, 0.2, 0.8, 220.0).amplitude;
            report.check ("fresh synth, A3: 7th harmonic against the fundamental",
                          toDb (fitSine (*out, 0, 44100.0, 0.2, 0.8, 1540.0).amplitude / fundamental), -19.0, -15.0,
                          "dB", "a square's -16.9 dB");
            report.check ("fresh synth, A3: level 2-7ms in against the sustain",
                          toDb (rms (*out, 0, 44100.0, 0.002, 0.007) / rms (*out, 0, 44100.0, 0.3, 0.5)), -3.0, 3.0,
                          "dB", "already there");
        }
    }

    std::printf ("\n%d check%s failed\n", report.failures, report.failures == 1 ? "" : "s");
    dir.deleteRecursively();
    return report.failures;
}

} // namespace carve::test
