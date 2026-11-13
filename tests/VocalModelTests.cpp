// The modelled singer (src/voice/VocalModel, VocalScore): the main checks of
// the web-voice prototype's tools/selftest.mjs, on the port. The folds have to
// start by themselves and land on the pitch, a voiceless stop has to close to
// silence, the voice has to stop when the song does, and nothing may blow up.
// Offline and deterministic (the noise is a seeded xorshift), so here rather
// than through the engine.

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "voice/VocalModel.h"
#include "voice/VocalScore.h"

using namespace carve::voice::vocal;

namespace
{
    constexpr double fs = 48000.0;
    constexpr double start = 0.8;   // where the score starts, as in the selftest

    double power (const std::vector<float>& x, double t0, double t1)
    {
        const auto a = (size_t) std::lround (t0 * fs), b = (size_t) std::lround (t1 * fs);
        double s = 0;

        for (size_t i = a; i < b && i < x.size(); ++i)
            s += (double) x[i] * (double) x[i];

        return s / (double) (b - a);
    }

    double dB (double p)  { return 10.0 * std::log10 (p + 1.0e-30); }

    int noteNumber (const std::string& s)
    {
        static const int semi[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        size_t i = 1;
        int accidental = 0;

        if (s[i] == '#') { accidental = 1; ++i; }
        else if (s[i] == 'b') { accidental = -1; ++i; }

        return semi[s[0] - 'A'] + accidental + 12 * (std::stoi (s.substr (i)) + 1);
    }

    // The prototype's score text: "lyric:note:beats", r:beats for a rest.
    std::vector<SungNote> parseScore (const std::string& text, double bpm, double& lengthSec)
    {
        std::vector<SungNote> notes;
        std::istringstream in (text);
        std::string token;
        double beat = 0;
        const double beatSec = 60 / bpm;

        while (in >> token)
        {
            std::vector<std::string> parts;
            std::istringstream ts (token);

            for (std::string p; std::getline (ts, p, ':');)
                parts.push_back (p);

            if (parts[0] == "r")
            {
                beat += std::stod (parts[1]);
                continue;
            }

            const auto beats = std::stod (parts[2]);
            const auto moras = parseLyrics (parts[0]);
            const double len = beats * beatSec;

            for (size_t i = 0; i < moras.size(); ++i)
            {
                SungNote n;
                n.t = beat * beatSec + (double) i * len / (double) moras.size();
                n.dur = len / (double) moras.size();
                n.midi = noteNumber (parts[1]);
                n.mora = moras[i];
                notes.push_back (n);
            }

            beat += beats;
        }

        lengthSec = beat * beatSec;
        return notes;
    }

    struct Sung
    {
        std::vector<float> out;
        std::vector<bool> voiced;           // every 5ms
        std::vector<double> fricative;      // every 5ms
    };

    Sung sing (const std::string& score, double bpm = 60, Options options = {})
    {
        options.vibrato = 0;
        options.drift = 0;
        Synth synth (fs, options);
        double length = 0;
        const auto notes = parseScore (score, bpm, length);
        const auto events = planScore (notes, start, synth.getCenter());
        synth.schedule (events.data(), (int) events.size());

        Sung result;
        result.out.resize ((size_t) std::lround ((length + start + 0.9) * fs));
        const size_t hop = (size_t) std::lround (fs * 0.005);

        for (size_t i = 0; i < result.out.size(); i += hop)
        {
            synth.process (result.out.data() + i, (int) std::min (hop, result.out.size() - i));
            result.voiced.push_back (synth.isVoiced());
            result.fricative.push_back (synth.getFricationAmp());
        }

        return result;
    }

    // A vowel held at a pitch with the pressure the singer would use; the
    // pitch error in cents, or 999 if the folds never started.
    double heldVowelCents (Options options, Vowel v, int midi, double* levelDb = nullptr)
    {
        options.drift = 0;
        options.vibrato = 0;
        Synth synth (fs, options);
        synth.setVowel (v, 0.01, midi);
        synth.set (pitch, midi);
        static const char names[] = { 'a', 'i', 'u', 'e', 'o' };
        synth.ramp (ps, pressureFor (midi, names[v], synth.getCenter()), 0.07);
        std::vector<float> out ((size_t) (fs * 0.9));
        synth.process (out.data(), (int) out.size());

        if (levelDb != nullptr)
            *levelDb = dB (power (out, 0.6, 0.9));

        return synth.isVoiced() ? std::abs (1200.0 * std::log2 (synth.getMeasuredHz() / synth.getTargetHz())) : 999.0;
    }

    std::string read (const std::string& lyric)
    {
        static const char* names[] = { "", "k", "g", "t", "d", "p", "b", "s", "z", "sh", "hy", "f", "h", "ch", "ts",
                                       "j", "m", "n", "ny", "r", "y", "w" };
        std::string out;

        for (const auto& m : parseLyrics (lyric))
        {
            if (! out.empty())
                out += ' ';

            out += names[(int) m.c];
            out += m.pal ? "y" : "";
            out += m.v;
        }

        return out;
    }
}

TEST_CASE ("kana, katakana and romaji read as the prototype reads them", "[voice]")
{
    CHECK (read ("\xe3\x81\x8d\xe3\x82\x83\xe3\x81\x97\xe3\x82\x85\xe3\x81\xa1\xe3\x82\x87") == "kya shu cho");   // きゃしゅちょ
    CHECK (read ("\xe3\x81\x8c\xe3\x81\xa3\xe3\x81\x93\xe3\x81\x86") == "ga Q ko u");                           // がっこう
    CHECK (read ("\xe3\x81\x97\xe3\x82\x93\xe3\x81\xb6\xe3\x82\x93") == "shi N bu N");                          // しんぶん
    CHECK (read ("\xe3\x81\xb5\xe3\x81\x81\xe3\x81\xa6\xe3\x81\x83") == "fa ti");                               // ふぁてぃ
    CHECK (read ("\xe3\x82\xb5\xe3\x82\xaf\xe3\x83\xa9") == "sa ku ra");                                        // サクラ
    CHECK (read ("\xe3\x81\xb2\xe3\x83\xbc\xe3\x82\x8d\xe3\x83\xbc") == "hyi - ro -");                          // ひーろー
    CHECK (read ("\xe3\x81\x98\xe3\x82\x83 \xe3\x81\xab\xe3\x82\x85") == "ja nyu");                              // じゃ にゅ
    CHECK (read ("ka sa") == "ka sa");
    CHECK (read ("\xef\xbd\x8b\xef\xbd\x81") == "ka");                                                          // full-width ｋａ
    CHECK (read ("?!") == "");
}

TEST_CASE ("the five vowels start by themselves and sing in tune", "[voice]")
{
    for (int preset = 0; preset < numVowelPresets; ++preset)
    {
        Options options;
        options.vowelPreset = preset;
        double worst = 0;

        for (int v = 0; v < 5; ++v)
            for (int midi = 55; midi <= 77; midi += 3)   // G3 to E5, across the comfortable range
            {
                double level = 0;
                const auto cents = heldVowelCents (options, (Vowel) v, midi, &level);
                INFO ("preset " << preset << " vowel " << v << " midi " << midi << ": " << cents << " cents, " << level << " dB");
                CHECK (cents < 10.0);
                CHECK (level > -32.0);
                worst = std::max (worst, cents);
            }

        INFO ("worst " << worst << " cents");
        CHECK (worst < 10.0);
    }
}

TEST_CASE ("every character sings its range", "[voice]")
{
    for (int c = 0; c < numCharacters; ++c)
    {
        const auto& character = getCharacter (c);
        const int bottom = std::string (character.key) == "anime" ? 58 : 55;
        const int top = std::string (character.key) == "mature" ? 76 : 77;

        for (int v = 0; v < 5; ++v)
            for (int midi : { bottom, (bottom + top) / 2, top })
            {
                INFO (character.name << " vowel " << v << " midi " << midi);
                CHECK (heldVowelCents (character.toOptions(), (Vowel) v, midi) < 15.0);
            }
    }
}

TEST_CASE ("a song sings through without blowing up, and stops when it ends", "[voice]")
{
    const std::string sakura = "\xe3\x81\x95:A4:1 \xe3\x81\x8f:A4:1 \xe3\x82\x89:B4:2 "      // さくら
                               "\xe3\x82\x84:A4:1 \xe3\x82\x88:B4:1 \xe3\x81\x84:C5:1 \xe3\x81\xae:B4:1 "   // やよいの
                               "\xe3\x81\x9d:A4:1 \xe3\x82\x89:B4:0.5 \xe3\x83\xbc:A4:0.5 \xe3\x81\xaf:F4:2";   // そらーは

    // The synth's neutral defaults (what the selftest sings the demos with),
    // and some characters. The output's soft clipper ends at 1; the neutral
    // voice stays clear of it, a bright character (Bell) can touch it.
    for (int c = -1; c < numCharacters; c += 3)
    {
        const auto options = c < 0 ? Options {} : getCharacter (c).toOptions();
        const auto sung = sing (sakura, 76, options);
        double peak = 0;
        bool finite = true;

        for (auto s : sung.out)
        {
            finite = finite && std::isfinite (s);
            peak = std::max (peak, (double) std::abs (s));
        }

        const auto end = (double) sung.out.size() / fs;
        INFO ((c < 0 ? "neutral" : getCharacter (c).name) << ": peak " << peak << ", tail " << dB (power (sung.out, end - 0.1, end - 0.01)) << " dB");
        CHECK (finite);
        CHECK (peak < (c < 0 ? 0.98 : 1.0));
        CHECK (peak > 0.05);
        CHECK (dB (power (sung.out, end - 0.1, end - 0.01)) < -50.0);

        // Voiced through most of it: every note's vowel sounds.
        size_t voiced = 0;
        const auto first = (size_t) (start / 0.005), last = (size_t) ((end - 0.9) / 0.005);

        for (size_t i = first; i < last && i < sung.voiced.size(); ++i)
            voiced += sung.voiced[i] ? 1 : 0;

        CHECK ((double) voiced / (double) (last - first) > 0.6);
    }
}

TEST_CASE ("a voiceless stop closes to silence, a nasal hums, a fricative hisses", "[voice]")
{
    // "あ, then the consonant": the stretch before the second vowel.
    auto consonant = [] (const std::string& kana, double& low, double& middle)
    {
        const auto sung = sing ("\xe3\x81\x82:A4:1 " + kana + ":A4:1");
        const auto vowel = power (sung.out, start + 0.4, start + 0.7);
        const double t = start + 1;
        low = 1.0e30;

        for (double s = t - 0.16; s < t - 0.005; s += 0.005)
            low = std::min (low, power (sung.out, s, s + 0.01));

        middle = dB (power (sung.out, t - 0.08, t - 0.06) / vowel);
        low = dB (low / vowel);
        return sung;
    };

    double low = 0, middle = 0;

    for (const auto* kana : { "\xe3\x81\x8b", "\xe3\x81\x9f", "\xe3\x81\xb1" })   // か た ぱ
    {
        consonant (kana, low, middle);
        INFO (kana << ": closure " << low << " dB");
        CHECK (low < -25.0);
    }

    consonant ("\xe3\x81\x82", low, middle);   // あ あ: no gap
    CHECK (low > -16.0);

    for (const auto* kana : { "\xe3\x81\xaa", "\xe3\x81\xbe" })   // な ま
    {
        consonant (kana, low, middle);
        INFO (kana << ": " << low << " dB at its quietest");
        CHECK (low > -30.0);
        CHECK (low < -2.0);
    }

    const auto sa = consonant ("\xe3\x81\x95", low, middle);   // さ
    double hiss = 0;

    for (size_t i = (size_t) ((start + 0.8) / 0.005); i < (size_t) ((start + 1.0) / 0.005); ++i)
        hiss = std::max (hiss, sa.fricative[i]);

    CHECK (hiss > 2.0);
}

TEST_CASE ("a small tsu lengthens the fricative after it", "[voice]")
{
    auto fricativeMs = [] (const std::string& score)
    {
        const auto sung = sing (score, 120);
        int ms = 0;
        bool started = false;

        for (size_t i = 0; i < sung.fricative.size(); ++i)
        {
            const double t = (double) (i + 1) * 0.005;
            started = started || sung.voiced[i];

            if (started && t <= start + 1.6 && sung.fricative[i] > 2)
                ms += 5;
        }

        return ms;
    };

    const auto issho = fricativeMs ("\xe3\x81\x84:A4:1 \xe3\x81\xa3:A4:1 \xe3\x81\x97\xe3\x82\x87:A4:1");   // い っ しょ
    const auto isho = fricativeMs ("\xe3\x81\x84:A4:1 \xe3\x81\x97\xe3\x82\x87:A4:1");                      // い しょ
    INFO ("issho " << issho << "ms, isho " << isho << "ms");
    CHECK (issho > isho + 250);
}

TEST_CASE ("a held key sings, and letting go of it stops the voice", "[voice]")
{
    Synth synth (fs, getCharacter (defaultCharacter).toOptions());
    PlanState state;
    EventList events;

    SungNote key;
    key.t = 0.1;
    key.dur = 1;
    key.midi = 69;
    key.open = true;
    planNote (key, nullptr, nullptr, state, synth.getCenter(), events);
    synth.schedule (events.events, events.size);

    std::vector<float> held ((size_t) (fs * 1.0));
    synth.process (held.data(), (int) held.size());
    REQUIRE (synth.isVoiced());
    CHECK (std::abs (1200.0 * std::log2 (synth.getMeasuredHz() / 440.0)) < 60.0);   // vibrato and drift on

    events.size = 0;
    planRelease (synth.time() + 0.12, 0.6, 9, state, events);
    synth.schedule (events.events, events.size);

    std::vector<float> after ((size_t) (fs * 1.5));
    synth.process (after.data(), (int) after.size());
    CHECK_FALSE (synth.isVoiced());
    CHECK (dB (power (after, 1.3, 1.5)) < -50.0);
}

TEST_CASE ("silence stops a song part way through", "[voice]")
{
    Synth synth (fs);
    double length = 0;
    const auto notes = parseScore ("\xe3\x81\x82:A4:4", 60, length);
    const auto events = planScore (notes, 0.2, synth.getCenter());
    synth.schedule (events.data(), (int) events.size());

    std::vector<float> out ((size_t) (fs * 1.5));
    synth.process (out.data(), (int) out.size());
    REQUIRE (synth.isVoiced());

    synth.silence();
    CHECK (synth.numQueued() == 0);
    synth.process (out.data(), (int) out.size());
    CHECK_FALSE (synth.isVoiced());
    CHECK (dB (power (out, 1.0, 1.5)) < -50.0);
}

TEST_CASE ("the voice carries through a voiced consonant into the next note", "[voice]")
{
    // あ then the consonant, as above: how quiet the stretch between the two
    // vowels gets. A voiced stop, じ and ら used to fall as silent as a
    // voiceless stop's closure -- every note of a line like だんだん cut off
    // from the next.
    auto quietest = [] (const std::string& kana)
    {
        const auto sung = sing ("\xe3\x81\x82:A4:1 " + kana + ":A4:1");
        const auto vowel = power (sung.out, start + 0.4, start + 0.7);
        const double t = start + 1;
        double low = 1.0e30;

        for (double s = t - 0.2; s < t - 0.005; s += 0.005)
            low = std::min (low, power (sung.out, s, s + 0.01));

        return dB (low / vowel);
    };

    for (const auto* kana : { "\xe3\x81\x8c", "\xe3\x81\xa0", "\xe3\x81\xb0",     // が だ ば
                              "\xe3\x81\x98", "\xe3\x82\x89" })                    // じ ら
    {
        const auto low = quietest (kana);
        INFO (kana << ": " << low << " dB at its quietest");
        CHECK (low > -25.0);
    }

    // ...while a voiceless one still stops it.
    CHECK (quietest ("\xe3\x81\x8b") < -25.0);   // か
}

TEST_CASE ("a part is planned a note at a time, unreadable lyrics silent", "[voice]")
{
    std::vector<PartNote> part (3);
    part[0] = { 1.0, 0.5, 69, 100, "\xe3\x81\x95" };   // さ
    part[1] = { 1.5, 0.5, 71, 100, "?" };
    part[2] = { 2.0, 1.0, 72, 100, "" };               // the default lyric

    const auto planned = planPart (part, centerFor (0.75), 1.0);
    REQUIRE (planned.size() == 3);
    CHECK_FALSE (planned[0].empty());
    CHECK (planned[1].empty());
    CHECK_FALSE (planned[2].empty());

    // Every movement of the first note's consonant comes before its beat; a
    // phrase opens with a breath in, well ahead of it.
    double earliest = 1.0e9;

    for (const auto& e : planned[0])
        earliest = std::min (earliest, e.t);

    CHECK (earliest < 1.0 - 0.4);
    CHECK (earliest > 1.0 - 0.8);
}

TEST_CASE ("speed quickens the move into a note and leaves the rest of it alone", "[voice]")
{
    // あ, then さ: the second note's consonant digs into the first.
    std::vector<PartNote> part (2);
    part[0] = { 1.0, 1.0, 69, 100, "\xe3\x81\x82" };
    part[1] = { 2.0, 1.5, 72, 100, "\xe3\x81\x95" };

    const auto center = centerFor (0.75);
    const auto normal = planPart (part, center, 1.0);
    const auto fast = planPart (part, center, 1.0, 2.0);
    const auto slow = planPart (part, center, 1.0, 0.5);

    REQUIRE (planPart (part, center, 1.0, 1.0).size() == normal.size());

    auto earliest = [] (const std::vector<Event>& events)
    {
        double t = 1.0e9;

        for (const auto& e : events)
            t = std::min (t, e.t);

        return t;
    };

    auto latest = [] (const std::vector<Event>& events)
    {
        double t = -1.0e9;

        for (const auto& e : events)
            t = std::max (t, e.t + e.duration);

        return t;
    };

    const double lead = 2.0 - earliest (normal[1]);
    CHECK (lead > 0.1);
    CHECK (2.0 - earliest (fast[1]) == lead / 2);    // twice as quick
    CHECK (2.0 - earliest (slow[1]) > lead * 1.5);   // slower, still within the note before
    CHECK (2.0 - earliest (slow[1]) <= 0.6 + 1.0e-9);

    // The swell, the vibrato and the end of the phrase are the note's own.
    CHECK (latest (fast[1]) == latest (normal[1]));
    CHECK (fast[1].size() == normal[1].size());
}
