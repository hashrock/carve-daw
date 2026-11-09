#include "GabberKickDsp.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace carve::plugins::gabber
{

namespace
{
    constexpr double pi = 3.14159265358979323846;
    constexpr int oversampling = 4;
    constexpr int lutHalfSize = 2048;
    constexpr int lutSize = lutHalfSize * 2 + 1;

    // Non-finite input lands on the bottom of the range: a NaN must not get
    // past any clamp, or one bad sample would poison the whole render.
    double clamp (double v, double lo, double hi)
    {
        return std::max (lo, std::min (hi, std::isfinite (v) ? v : lo));
    }

    double db (double v)  { return std::pow (10.0, v / 20.0); }

    double sign (double x)  { return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0; }

    // Where a driven sample lands on the curve's -1..1 span. A hard clamp here
    // would pin everything loud to the curve's end points.
    double applyDomain (double x, bool fold)
    {
        if (fold)
        {
            const auto u = std::fmod (std::fmod (x + 1.0, 4.0) + 4.0, 4.0);
            return u <= 2.0 ? u - 1.0 : 3.0 - u;
        }

        // Unity around zero, then approaching the edge without reaching it.
        return std::abs (x) <= 0.75 ? x
                                    : sign (x) * (0.75 + 0.25 * std::tanh ((std::abs (x) - 0.75) * 4.0));
    }

    std::array<double, 5> getPeakingCoefficients (double freq, double gain, double q, double rate)
    {
        const auto w = 2.0 * pi * clamp (freq, 20.0, std::min (18000.0, rate * 0.45)) / rate;
        const auto a = db (clamp (gain, -24.0, 24.0) / 2.0);
        const auto alpha = std::sin (w) / (2.0 * clamp (q, 0.2, 10.0));
        const auto c = std::cos (w);
        const auto a0 = 1.0 + alpha / a;

        return { (1.0 + alpha * a) / a0, -2.0 * c / a0, (1.0 - alpha * a) / a0,
                 -2.0 * c / a0, (1.0 - alpha / a) / a0 };
    }

    // A 257-tap Blackman-windowed sinc with its cutoff at 0.45 of the output
    // rate, whatever that rate is.
    const std::vector<double>& getDecimationFilter()
    {
        static const auto filter = []
        {
            constexpr int length = 257;
            constexpr double cutoff = 0.1125;

            std::vector<double> h ((size_t) length);
            const auto mid = (length - 1) / 2;
            double sum = 0.0;

            for (int i = 0; i < length; ++i)
            {
                const auto x = (double) (i - mid);
                const auto phase = 2.0 * pi * i / (length - 1);

                h[(size_t) i] = (x == 0.0 ? 2.0 * cutoff : std::sin (2.0 * pi * cutoff * x) / (pi * x))
                                * (0.42 - 0.5 * std::cos (phase) + 0.08 * std::cos (2.0 * phase));
                sum += h[(size_t) i];
            }

            for (auto& tap : h)
                tap /= sum;

            return h;
        }();

        return filter;
    }

    std::vector<float> decimate (const std::vector<double>& input)
    {
        const auto& filter = getDecimationFilter();
        const auto mid = (int) (filter.size() - 1) / 2;
        const auto inputSize = (int) input.size();

        std::vector<float> out ((size_t) ((inputSize + oversampling - 1) / oversampling));

        for (int i = 0; i < (int) out.size(); ++i)
        {
            double sum = 0.0;

            for (int j = 0; j < (int) filter.size(); ++j)
                if (const auto k = i * oversampling + j - mid; k >= 0 && k < inputSize)
                    sum += input[(size_t) k] * filter[(size_t) j];

            out[(size_t) i] = (float) sum;
        }

        return out;
    }

    void synthesiseSource (const Patch& p, std::vector<double>& data, double sr, double duration)
    {
        const auto base = clamp (p.base, 25.0, 120.0);
        const auto sweep = clamp (p.sweep, 0.0, 700.0);
        const auto pitchDecay = clamp (p.pitchDecay, 0.002, 0.1);
        const auto attack = std::max (1.0 / sr, clamp (p.attack, 0.0, 0.01));
        const auto clickLevel = clamp (p.click, 0.0, 1.0);
        const auto noiseLevel = clamp (p.noise, 0.0, 1.0);

        double phase = 0.0, previousNoise = 0.0;
        std::uint32_t seed = 0x12345678u;

        for (size_t i = 0; i < data.size(); ++i)
        {
            const auto t = (double) i / sr;

            // Integrated, so the sweep has no phase jumps in it.
            phase += 2.0 * pi * (base + sweep * std::exp (-t / pitchDecay)) / sr;

            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const auto noise = (double) seed / 2147483648.0 - 1.0;
            const auto high = noise - previousNoise;
            previousNoise = noise;

            const auto end = std::min (1.0, (duration - t) / 0.015);
            const auto env = std::sin (std::min (1.0, t / attack) * pi / 2.0)
                              * std::exp (-5.0 * t / duration) * end * end;
            const auto click = std::sin (2.0 * pi * 2400.0 * t) * std::exp (-t / 0.0018) * std::min (1.0, t / 0.0002);

            data[i] = std::sin (phase) * env
                       + clickLevel * click
                       + noiseLevel * high * 2.0 * std::exp (-t / 0.022) * std::min (1.0, t / 0.0004) * end;
        }
    }

    void applyStage (const Stage& stage, std::vector<double>& data, double sr)
    {
        const auto drive = db (clamp (stage.drive, -12.0, 36.0));
        const auto mix = clamp (stage.mix, 0.0, 1.0);
        const auto gain = db (clamp (stage.output, -24.0, 12.0));
        const auto fold = stage.shape == Shape::wavefold;

        const auto logStart = std::log (clamp (stage.startFreq, 20.0, 18000.0));
        const auto logEnd = std::log (clamp (stage.endFreq, 20.0, 18000.0));
        const auto gainStart = clamp (stage.startGain, -24.0, 24.0);
        const auto gainEnd = clamp (stage.endGain, -24.0, 24.0);

        const auto points = stage.points.size() >= 2 ? stage.points : getShapePoints (Shape::linear);

        std::vector<double> lut ((size_t) lutSize);

        for (int i = 0; i < lutSize; ++i)
            lut[(size_t) i] = evaluateCurve (points, (double) i / lutHalfSize - 1.0);

        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        const auto last = std::max<size_t> (1, data.size() - 1);

        for (size_t i = 0; i < data.size(); ++i)
        {
            const auto u = (double) i / (double) last;
            const auto s = u * u * (3.0 - 2.0 * u);
            const auto c = getPeakingCoefficients (std::exp (logStart * (1.0 - s) + logEnd * s),
                                                   gainStart * (1.0 - s) + gainEnd * s, stage.q, sr);
            const auto x = data[i];
            auto y = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;

            // A filter swept this hard can be pushed unstable, and a non-finite
            // state would stay non-finite for the rest of the kick.
            if (! std::isfinite (y))
                y = 0.0;

            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;

            auto position = (applyDomain (y * drive, fold) + 1.0) * lutHalfSize;

            if (! std::isfinite (position))
                position = (double) lutHalfSize;

            const auto k = std::clamp ((int) std::floor (position), 0, lutSize - 2);
            const auto wet = lut[(size_t) k] + (lut[(size_t) k + 1] - lut[(size_t) k]) * (position - k);

            data[i] = clamp ((y * (1.0 - mix) + wet * mix) * gain, -32.0, 32.0);
        }
    }

    void removeDcAndFadeEdges (std::vector<double>& data, double sr)
    {
        const auto r = std::exp (-2.0 * pi * 15.0 / sr);
        const auto n = (double) data.size();
        double lastX = 0.0, lastY = 0.0;

        for (size_t i = 0; i < data.size(); ++i)
        {
            const auto x = data[i];
            const auto y = x - lastX + r * lastY;
            lastX = x;
            lastY = y;
            data[i] = y * std::min (1.0, (double) i / (sr * 0.0005))
                        * std::min (1.0, (n - 1.0 - (double) i) / (sr * 0.008));
        }
    }
} // namespace

//==============================================================================
const char* getShapeName (Shape shape)
{
    switch (shape)
    {
        case Shape::linear:     return "Linear";
        case Shape::softClip:   return "Soft Clip";
        case Shape::hardClip:   return "Hard Clip";
        case Shape::asymmetric: return "Asymmetric";
        case Shape::wavefold:   return "Wavefold";
        case Shape::rectifier:  return "Rectifier";
    }

    return "Linear";
}

std::vector<Point> getShapePoints (Shape shape)
{
    std::vector<Point> points (9);

    for (int i = 0; i < 9; ++i)
    {
        const auto x = (i - 4) / 4.0;
        double y = x;

        switch (shape)
        {
            case Shape::linear:     break;
            case Shape::softClip:   y = std::tanh (x * 2.5); break;
            case Shape::hardClip:   y = clamp (x * 4.0, -1.0, 1.0); break;
            case Shape::asymmetric: y = x < 0.0 ? std::tanh (x * 1.2) * 0.65 : std::tanh (x * 4.0); break;
            case Shape::wavefold:   y = std::sin (x * pi * 2.5) * 0.9; break;
            case Shape::rectifier:  y = std::abs (x) * 1.8 - 0.8; break;
        }

        points[(size_t) i] = { x, y };
    }

    return points;
}

Stage makeStage (Shape shape)
{
    Stage stage;
    stage.shape = shape;
    stage.points = getShapePoints (shape);
    return stage;
}

const char* getFactoryPresetName (int index)
{
    static const char* names[numFactoryPresets] { "Oldschool Gabber", "Industrial", "Modern Hardcore", "Raw / Folded" };
    return names[juce::jlimit (0, numFactoryPresets - 1, index)];
}

Patch getFactoryPreset (int index)
{
    Patch p;
    p.stages = { makeStage(), makeStage (Shape::hardClip) };

    auto& low = p.stages[1];
    low.startFreq = 180.0;
    low.endFreq = 320.0;
    low.startGain = 7.0;
    low.endGain = 3.0;
    low.drive = 7.0;
    low.mix = 0.75;

    if (index == 1)
    {
        p.noise = 0.4;
        p.duration = 0.85;
        p.stages = { makeStage (Shape::asymmetric), makeStage (Shape::wavefold) };
        p.stages[1].mix = 0.55;
    }
    else if (index == 2)
    {
        p.click = 0.75;
        p.duration = 0.4;
        p.sweep = 300.0;
        auto third = makeStage();
        third.drive = 5.0;
        third.mix = 0.65;
        p.stages.push_back (third);
    }
    else if (index == 3)
    {
        p.click = 0.5;
        auto second = makeStage (Shape::asymmetric);
        second.drive = 5.0;
        second.mix = 0.55;
        p.stages = { makeStage (Shape::wavefold), second };
        p.stages[0].mix = 0.65;
    }

    return p;
}

//==============================================================================
// Cubic Hermite with flat tangents at local extrema, so a non-monotone curve
// goes through its points without overshooting them.
double evaluateCurve (const std::vector<Point>& points, double x)
{
    if (points.size() < 2)
        return clamp (x, -1.0, 1.0);

    size_t i = 0;

    while (i < points.size() - 2 && x > points[i + 1].x)
        ++i;

    const auto& a = points[i];
    const auto& b = points[i + 1];
    const auto width = std::max (1.0e-6, b.x - a.x);
    const auto t = clamp ((x - a.x) / width, 0.0, 1.0);

    auto slope = [&points] (size_t j)
    {
        return (points[j + 1].y - points[j].y) / std::max (1.0e-6, points[j + 1].x - points[j].x);
    };

    auto tangent = [&points, &slope] (size_t j)
    {
        if (j == 0)
            return slope (0);

        if (j == points.size() - 1)
            return slope (j - 1);

        const auto l = slope (j - 1), r = slope (j);
        return l * r <= 0.0 ? 0.0 : 2.0 * l * r / (l + r);
    };

    const auto t2 = t * t, t3 = t2 * t;

    return clamp ((2.0 * t3 - 3.0 * t2 + 1.0) * a.y
                    + (t3 - 2.0 * t2 + t) * width * tangent (i)
                    + (-2.0 * t3 + 3.0 * t2) * b.y
                    + (t3 - t2) * width * tangent (i + 1),
                  -1.0, 1.0);
}

double getEqResponseDb (double frequency, double centre, double gainDb, double q)
{
    constexpr double rate = 176400.0;   // a display rate; the shape barely depends on it

    const auto c = getPeakingCoefficients (centre, gainDb, q, rate);
    const auto w = 2.0 * pi * frequency / rate;

    auto magnitude = [w] (double a, double b, double d)
    {
        const auto re = a + b * std::cos (w) + d * std::cos (2.0 * w);
        const auto im = b * std::sin (w) + d * std::sin (2.0 * w);
        return re * re + im * im;
    };

    return 10.0 * std::log10 (magnitude (c[0], c[1], c[2]) / magnitude (1.0, c[3], c[4]));
}

Render render (const Patch& p, double outputRate)
{
    const auto sr = outputRate * oversampling;
    const auto duration = clamp (p.duration, 0.2, 1.2);

    std::vector<double> data ((size_t) std::ceil (duration * sr));

    synthesiseSource (p, data, sr, duration);

    for (const auto& stage : p.stages)
        if (stage.enabled)
            applyStage (stage, data, sr);

    removeDcAndFadeEdges (data, sr);

    Render result;
    result.samples = decimate (data);

    double peak = 0.0;

    for (auto sample : result.samples)
        peak = std::max (peak, (double) std::abs (sample));

    const auto gain = (p.normalize && peak > 1.0e-10 ? db (-1.0) / peak : 1.0) * db (clamp (p.gain, -24.0, 12.0));
    const auto requestedPeak = peak * gain;
    const auto ceiling = db (-0.1);
    const auto safety = requestedPeak > ceiling ? ceiling / requestedPeak : 1.0;

    for (auto& sample : result.samples)
        sample = (float) clamp (sample * gain * safety, -1.0, 1.0);

    result.peak = requestedPeak;
    result.limited = safety < 1.0;
    return result;
}

//==============================================================================
juce::String stagesToJson (const std::vector<Stage>& stages)
{
    juce::Array<juce::var> list;

    for (const auto& stage : stages)
    {
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty ("enabled", stage.enabled);
        object->setProperty ("startFreq", stage.startFreq);
        object->setProperty ("endFreq", stage.endFreq);
        object->setProperty ("startGain", stage.startGain);
        object->setProperty ("endGain", stage.endGain);
        object->setProperty ("q", stage.q);
        object->setProperty ("drive", stage.drive);
        object->setProperty ("mix", stage.mix);
        object->setProperty ("output", stage.output);
        object->setProperty ("shape", getShapeName (stage.shape));

        juce::Array<juce::var> points;

        for (const auto& point : stage.points)
            points.add (juce::Array<juce::var> { point.x, point.y });

        object->setProperty ("points", points);
        list.add (object.release());
    }

    return juce::JSON::toString (list, true);
}

std::vector<Stage> stagesFromJson (const juce::String& json)
{
    std::vector<Stage> stages;
    const auto parsed = juce::JSON::parse (json);

    if (const auto* list = parsed.getArray())
    {
        for (const auto& item : *list)
        {
            if (item.getDynamicObject() == nullptr)
                continue;

            Stage stage;

            auto number = [&item] (const char* name, double fallback)
            {
                const auto value = item.getProperty (name, fallback);
                return value.isDouble() || value.isInt() || value.isInt64() ? (double) value : fallback;
            };

            stage.enabled = item.getProperty ("enabled", true);
            stage.startFreq = number ("startFreq", stage.startFreq);
            stage.endFreq = number ("endFreq", stage.endFreq);
            stage.startGain = number ("startGain", stage.startGain);
            stage.endGain = number ("endGain", stage.endGain);
            stage.q = number ("q", stage.q);
            stage.drive = number ("drive", stage.drive);
            stage.mix = number ("mix", stage.mix);
            stage.output = number ("output", stage.output);

            const auto shapeName = item.getProperty ("shape", {}).toString();

            for (int i = 0; i < numShapes; ++i)
                if (shapeName == getShapeName ((Shape) i))
                    stage.shape = (Shape) i;

            stage.points = getShapePoints (stage.shape);

            if (const auto* points = item.getProperty ("points", {}).getArray(); points != nullptr && points->size() >= 2)
            {
                std::vector<Point> read;

                for (const auto& entry : *points)
                    if (const auto* pair = entry.getArray(); pair != nullptr && pair->size() == 2)
                        read.push_back ({ (double) (*pair)[0], (double) (*pair)[1] });

                if (read.size() >= 2)
                    stage.points = std::move (read);
            }

            stages.push_back (std::move (stage));
        }
    }

    return stages;
}

} // namespace carve::plugins::gabber
