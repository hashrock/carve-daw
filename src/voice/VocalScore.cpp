#include "VocalScore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

// voice-jp.js, expression by expression (see VocalModel.cpp on why the order
// of the arithmetic matters).
namespace carve::voice::vocal
{

namespace
{
    bool same (double a, double b)  { return ! (a < b) && ! (a > b); }

    //==========================================================================
    // Kana -> moras

    std::u32string decodeUtf8 (const std::string& s)
    {
        std::u32string out;
        size_t i = 0;

        while (i < s.size())
        {
            const auto c = (unsigned char) s[i];
            char32_t cp = 0;
            int extra = 0;

            if (c < 0x80)                { cp = c; extra = 0; }
            else if ((c & 0xe0) == 0xc0) { cp = c & 0x1fu; extra = 1; }
            else if ((c & 0xf0) == 0xe0) { cp = c & 0x0fu; extra = 2; }
            else if ((c & 0xf8) == 0xf0) { cp = c & 0x07u; extra = 3; }
            else                         { ++i; continue; }

            if (i + (size_t) extra >= s.size() + (extra > 0 ? 0 : 1))
                break;

            bool ok = true;

            for (int k = 1; k <= extra; ++k)
            {
                const auto cc = (unsigned char) s[i + (size_t) k];

                if ((cc & 0xc0) != 0x80)
                {
                    ok = false;
                    break;
                }

                cp = (cp << 6) | (cc & 0x3fu);
            }

            i += ok ? (size_t) extra + 1 : 1;

            if (ok)
                out.push_back (cp);
        }

        return out;
    }

    const std::map<char32_t, std::string>& kanaTable()
    {
        static const auto table = []
        {
            std::map<char32_t, std::string> t;

            auto add = [&t] (const char* kana, const char* romaji)
            {
                const auto chars = decodeUtf8 (kana);
                std::vector<std::string> words;
                std::string word;

                for (const char* p = romaji; ; ++p)
                {
                    if (*p == ' ' || *p == 0)
                    {
                        words.push_back (word);
                        word.clear();

                        if (*p == 0)
                            break;
                    }
                    else
                    {
                        word += *p;
                    }
                }

                for (size_t i = 0; i < chars.size() && i < words.size(); ++i)
                    t[chars[i]] = words[i];
            };

            add ("\xe3\x81\x82\xe3\x81\x84\xe3\x81\x86\xe3\x81\x88\xe3\x81\x8a", "a i u e o");
            add ("\xe3\x81\x8b\xe3\x81\x8d\xe3\x81\x8f\xe3\x81\x91\xe3\x81\x93", "ka ki ku ke ko");
            add ("\xe3\x81\x8c\xe3\x81\x8e\xe3\x81\x90\xe3\x81\x92\xe3\x81\x94", "ga gi gu ge go");
            add ("\xe3\x81\x95\xe3\x81\x97\xe3\x81\x99\xe3\x81\x9b\xe3\x81\x9d", "sa shi su se so");
            add ("\xe3\x81\x96\xe3\x81\x98\xe3\x81\x9a\xe3\x81\x9c\xe3\x81\x9e", "za ji zu ze zo");
            add ("\xe3\x81\x9f\xe3\x81\xa1\xe3\x81\xa4\xe3\x81\xa6\xe3\x81\xa8", "ta chi tsu te to");
            add ("\xe3\x81\xa0\xe3\x81\xa2\xe3\x81\xa5\xe3\x81\xa7\xe3\x81\xa9", "da ji zu de do");
            add ("\xe3\x81\xaa\xe3\x81\xab\xe3\x81\xac\xe3\x81\xad\xe3\x81\xae", "na ni nu ne no");
            add ("\xe3\x81\xaf\xe3\x81\xb2\xe3\x81\xb5\xe3\x81\xb8\xe3\x81\xbb", "ha hi fu he ho");
            add ("\xe3\x81\xb0\xe3\x81\xb3\xe3\x81\xb6\xe3\x81\xb9\xe3\x81\xbc", "ba bi bu be bo");
            add ("\xe3\x81\xb1\xe3\x81\xb4\xe3\x81\xb7\xe3\x81\xba\xe3\x81\xbd", "pa pi pu pe po");
            add ("\xe3\x81\xbe\xe3\x81\xbf\xe3\x82\x80\xe3\x82\x81\xe3\x82\x82", "ma mi mu me mo");
            add ("\xe3\x82\x84\xe3\x82\x86\xe3\x82\x88", "ya yu yo");
            add ("\xe3\x82\x89\xe3\x82\x8a\xe3\x82\x8b\xe3\x82\x8c\xe3\x82\x8d", "ra ri ru re ro");
            add ("\xe3\x82\x8f\xe3\x82\x92\xe3\x82\x93\xe3\x81\xa3\xe3\x83\xbc\xe3\x82\x94", "wa o N Q - bu");
            return t;
        }();

        return table;
    }

    char smallY (char32_t c)
    {
        switch (c)
        {
            case 0x3083: return 'a';   // ゃ
            case 0x3085: return 'u';   // ゅ
            case 0x3087: return 'o';   // ょ
            default:     return 0;
        }
    }

    char smallV (char32_t c)
    {
        switch (c)
        {
            case 0x3041: return 'a';   // ぁ
            case 0x3043: return 'i';   // ぃ
            case 0x3045: return 'u';   // ぅ
            case 0x3047: return 'e';   // ぇ
            case 0x3049: return 'o';   // ぉ
            default:     return 0;
        }
    }

    std::optional<Cons> consFromString (const std::string& c)
    {
        static const std::map<std::string, Cons> table {
            { "", Cons::none }, { "k", Cons::k }, { "g", Cons::g }, { "t", Cons::t }, { "d", Cons::d },
            { "p", Cons::p }, { "b", Cons::b }, { "s", Cons::s }, { "z", Cons::z }, { "sh", Cons::sh },
            { "hy", Cons::hy }, { "f", Cons::f }, { "h", Cons::h }, { "ch", Cons::ch }, { "ts", Cons::ts },
            { "j", Cons::j }, { "m", Cons::m }, { "n", Cons::n }, { "ny", Cons::ny }, { "r", Cons::r },
            { "y", Cons::y }, { "w", Cons::w }
        };

        if (const auto it = table.find (c); it != table.end())
            return it->second;

        return std::nullopt;
    }

    // One mora of romaji -> consonant, contracted or not, vowel.
    std::optional<Mora> fromRomaji (const std::string& r)
    {
        if (r == "N" || r == "n")  return Mora { Cons::none, false, 'N' };
        if (r == "Q" || r == "q")  return Mora { Cons::none, false, 'Q' };
        if (r == "-")              return Mora { Cons::none, false, '-' };

        if (r.empty())
            return std::nullopt;

        const char v = r.back();

        if (std::string ("aiueo").find (v) == std::string::npos)
            return std::nullopt;

        auto c = r.substr (0, r.size() - 1);
        bool pal = false;

        static const char* const consonants[] = { "ky", "gy", "sh", "ch", "ts", "ny", "hy", "my", "ry", "by", "py",
                                                  "j", "k", "g", "s", "z", "t", "d", "n", "h", "f", "b", "p", "m", "y", "r", "w" };

        if (! c.empty() && std::find_if (std::begin (consonants), std::end (consonants),
                                         [&c] (const char* k) { return c == k; }) == std::end (consonants))
            return std::nullopt;

        if (c.size() == 2 && c[1] == 'y' && c != "ny" && c != "hy")
        {
            c = c.substr (0, 1);
            pal = true;
        }

        if (c == "s" && v == 'i') c = "sh";
        if (c == "t" && v == 'i') c = "ch";
        if (c == "t" && v == 'u') c = "ts";
        if (c == "z" && v == 'i') c = "j";
        if (c == "h" && v == 'u') c = "f";
        if (c == "h" && v == 'i') c = "hy";   // ひ is a palatal fricative

        const auto cons = consFromString (c);

        if (! cons)
            return std::nullopt;

        return Mora { *cons, pal, v };
    }

    bool isAsciiLetter (char32_t c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

    //==========================================================================
    // Consonants: where, what kind, voiceless or not, the area it narrows to
    // (a fricative's; は's glottal opening; what a ら's flick leaves open) and
    // how long it takes against its kind's (lead). The areas and leads marked
    // were tuned by ear in the prototype's phoneme lab.

    enum class Kind : std::uint8_t { stop, fric, aff, h, nas, tap, gl, lat };

    struct ConsInfo
    {
        bool exists = false;
        int place = -1;      // -1: none (h, the glides)
        Kind kind = Kind::stop;
        bool voiceless = false;
        double area = 0;
        Vowel glide = va;
        double lead = 1;
        bool vz = false;     // an English voiced fricative: keeps voicing (the glottis stays shut, the narrowing a little wider)
    };

    ConsInfo consInfo (Cons c)
    {
        switch (c)
        {
            case Cons::k:  return { true, vel, Kind::stop, true, 0, va, 1.25 };
            case Cons::g:  return { true, vel, Kind::stop, false, 0, va };
            case Cons::t:  return { true, alv, Kind::stop, true, 0, va, 1.1 };
            case Cons::d:  return { true, alv, Kind::stop, false, 0, va };
            case Cons::p:  return { true, lab, Kind::stop, true, 0, va };
            case Cons::b:  return { true, lab, Kind::stop, false, 0, va };
            case Cons::s:  return { true, den, Kind::fric, true, 0.12e-4, va, 0.7 };
            case Cons::z:  return { true, den, Kind::fric, false, 0.12e-4, va };
            case Cons::sh: return { true, apl, Kind::fric, true, 0.295e-4, va, 0.95 };
            case Cons::hy: return { true, pal, Kind::fric, true, 0.22e-4, va, 0.65 };
            case Cons::f:  return { true, lab, Kind::fric, true, 0.16e-4, va };
            case Cons::h:  return { true, -1, Kind::h, false, 0.795e-4, va };
            case Cons::ch: return { true, apl, Kind::aff, true, 0.08e-4, va, 0.5 };
            case Cons::ts: return { true, den, Kind::aff, true, 0.095e-4, va, 0.5 };
            case Cons::j:  return { true, apl, Kind::aff, false, 0.13e-4, va };
            case Cons::m:  return { true, lab, Kind::nas, false, 0, va, 1.35 };
            case Cons::n:  return { true, alv, Kind::nas, false, 0, va, 1.35 };
            case Cons::ny: return { true, apl, Kind::nas, false, 0, va };
            case Cons::r:  return { true, alv, Kind::tap, false, 0, va, 1.45 };
            case Cons::y:  return { true, -1, Kind::gl, false, 0, vi };
            case Cons::w:  return { true, -1, Kind::gl, false, 0, vu };
            // English. The voiced fricatives keep their voicing (vz): opened a
            // little, as ざ is, the air behind the narrowing soon stops the
            // folds and v, dh, zh come out as f, th, sh.
            case Cons::v:  return { true, lab, Kind::fric, false, 0.25e-4, va, 1, true };
            case Cons::th: return { true, ide, Kind::fric, true, 0.2e-4, va };
            case Cons::dh: return { true, ide, Kind::fric, false, 0.3e-4, va, 1, true };
            case Cons::zh: return { true, apl, Kind::fric, false, 0.2e-4, va, 1, true };
            case Cons::ng: return { true, vel, Kind::nas, false, 0, va };
            case Cons::l:  return { true, lat, Kind::lat, false, 0.15e-4, va };
            case Cons::rr: return { true, -1, Kind::gl, false, 0, vER };
            case Cons::fl: return { true, alv, Kind::tap, false, 0.03e-4, va, 0.5 };   // a 20-30ms flick, not shut
            case Cons::ek: return { true, veE, Kind::stop, true, 0, va, 1.25 };
            case Cons::eg: return { true, veE, Kind::stop, false, 0, va };
            case Cons::none: break;
        }

        return {};
    }

    // How long each kind of consonant takes before the vowel [s]: a little
    // longer than in speech, to be heard.
    double leadOf (Kind k)
    {
        switch (k)
        {
            case Kind::stop: return 0.1;
            case Kind::fric: return 0.17;
            case Kind::aff:  return 0.17;
            case Kind::h:    return 0.12;
            case Kind::nas:  return 0.115;
            case Kind::tap:  return 0.055;
            case Kind::gl:   return 0.12;
            case Kind::lat:  return 0.08;
        }

        return 0.03;
    }

    constexpr double ADD = 0.05e-4, OPEN = 0.30e-4;   // the glottis [m^2]: voicing, a voiceless consonant
    constexpr double VELUM = 1.0e-4;                   // the velum, for a nasal
    constexpr double voicedLeak = 0.02;                // ...and the little of it a voiced closure lets through

    // How a phrase ends (voice -> breathy -> breath).
    constexpr double tailFo = 0.5, tailFo0 = 0.07, tailAbd = 0.085e-4, tailAir = 5, tailAirNarrow = 1, tailPsAt = 0.45, tailOver = 0.1;

    Vowel vowelOf (char v)
    {
        switch (v)
        {
            case 'a': return va;
            case 'i': return vi;
            case 'u': return vu;
            case 'e': return ve;
            case 'o': return vo;
            default:  return v >= 1 && v <= enVowelChar (vER) ? (Vowel) ((int) vIY + v - 1) : vx;
        }
    }

    // The narrow vowels: a phrase on one ends without a breathy tail.
    bool isNarrow (char v)
    {
        return v == 'i' || v == 'u' || v == enVowelChar (vIY) || v == enVowelChar (vUW) || v == enVowelChar (vIH)
                || v == enVowelChar (vUH) || v == enVowelChar (vER);
    }

    constexpr double clusterLead = 0.7;   // a consonant further from the vowel in a cluster (st, tw) is a little shorter

    // Live keys, where what comes next is unknown.
    Expression liveExpr (bool phraseStart, double up)
    {
        Expression x;
        x.scoop = phraseStart ? 1.0 : up >= 3 ? std::min (1.2, 0.35 + 0.13 * up) : 0;
        x.accent = phraseStart ? 0.6 : 0;
        x.end = Expression::End::none;
        x.endDepth = 1;
        x.vib = 0.8;
        x.g = 1;
        x.bounce = 1;
        x.over = std::abs (up) >= 3 ? std::min (0.8, 0.2 + 0.07 * std::abs (up)) * (up < 0 ? 0.6 : 1) : 0.08 * std::abs (up);
        return x;
    }

    Event rampEvent (double t, Param p, double v, double d)
    {
        Event e;
        e.t = t; e.kind = Event::ramp; e.target = p; e.value = v; e.duration = d;
        return e;
    }

    Event vowelEvent (double t, Vowel v, double d, double midi)
    {
        Event e;
        e.t = t; e.kind = Event::vowel; e.target = v; e.value = midi; e.duration = d;
        return e;
    }

    Event closeEvent (double t, int place, double area, double d)
    {
        Event e;
        e.t = t; e.kind = Event::close; e.target = (std::uint8_t) place; e.value = area; e.duration = d;
        return e;
    }

    Event openEvent (double t, int place, double d)
    {
        Event e;
        e.t = t; e.kind = Event::open; e.target = (std::uint8_t) place; e.duration = d;
        return e;
    }

    // One consonant's moves (the prototype's consGesture). t0 is when it
    // opens into what follows (the vowel, or the next consonant of a cluster),
    // sc its time scale against its kind's lead. `into` is a consonant
    // straight after it (st, ts): then it holds its narrowing until that one
    // closes, and between two voiceless ones the glottis stays open -- no
    // vowel slips in between. `coda` is an English syllable's end: no extra
    // push of breath, a gentle release, and the last one neither opens nor
    // voices again (this, love: opened, it falls back into a vowel); held is
    // a stop after a nasal at its place (nd, mb): the mouth is shut already,
    // so the velum only rises.
    enum class CodaMode : std::uint8_t { none, coda, held };

    struct GestureContext
    {
        bool phraseStart = false;
        double psNote = 0;
        bool narrowV = false;
        int m = 60;
        const ConsInfo* into = nullptr;
        CodaMode coda = CodaMode::none;
    };

    void consGesture (EventList& ev, const ConsInfo& c, double t0, double sc, const GestureContext& o)
    {
        auto R = [&ev] (double t, Param p, double v, double d)  { ev.push (rampEvent (t, p, v, d)); };
        auto T = [t0, sc] (double dt)  { return t0 - dt * sc; };
        auto D = [sc] (double d)       { return d * sc; };
        const int pl = c.place;
        const bool keepOpen = o.into != nullptr && o.into->voiceless;
        const bool last = o.coda != CodaMode::none && o.into == nullptr;   // the syllable's final consonant

        if (c.kind == Kind::stop)
        {
            if (o.coda == CodaMode::held)
            {
                R (T (0.1), velum, 0, D (0.03));
                return;
            }

            // Close, let the pressure build, and the release bursts. Pushed
            // harder while closed, for a clearer release.
            ev.push (closeEvent (T (0.1), pl, 0, D (0.028)));
            ev.push (openEvent (o.coda != CodaMode::none ? T (0.03) : T (0.016), pl, o.coda != CodaMode::none ? 0.06 : D (0.018)));

            if (c.voiceless)
            {
                R (T (0.105), abd, 0.26e-4, D (0.035));

                if (! keepOpen && ! last)
                    R (t0 + (o.narrowV ? -0.006 : 0.004), abd, ADD, 0.04);
            }

            if (! o.phraseStart && o.coda == CodaMode::none)
                R (T (0.085), ps, o.psNote * 1.3, D (0.04));

            // A voiced one keeps voicing through the closure, as a singer
            // does: closed off, the air behind the lips or tongue soon
            // pushes as hard as the lungs, the folds stop, and だ・ば・が
            // came out as a gap as silent as た・ぱ・か's -- every such note
            // cut off from the one before. A little air let out through the
            // nose keeps it flowing, a hum under the closure (and in が, the
            // [ŋ] Japanese sings it with between vowels). Shut as the
            // closure opens: shut any sooner, the pressure it traps pops.
            if (! c.voiceless && ! o.phraseStart)
            {
                R (T (0.1), velum, VELUM * voicedLeak, D (0.03));
                R (t0, velum, 0, D (0.015));
            }
        }
        else if (c.kind == Kind::fric)
        {
            // Narrow with the air flowing. Voiced (ざ) opens the glottis a
            // little; an English voiced one (vz) keeps it shut.
            ev.push (closeEvent (T (0.17), pl, c.area, 0.028));

            if (! last)
                ev.push (o.into != nullptr ? openEvent (t0 + 0.005, pl, 0.03) : openEvent (T (0.024), pl, D (0.035)));

            if (c.voiceless)
            {
                R (T (0.18), abd, OPEN, D (0.045));

                if (! keepOpen && ! last)
                    R (o.into != nullptr ? t0 : T (0.042), abd, ADD, 0.045);
            }
            else if (! c.vz)
            {
                R (T (0.17), abd, 0.11e-4, D (0.04));
                R (T (0.032), abd, ADD, 0.04);
            }
        }
        else if (c.kind == Kind::aff)
        {
            // Close, then open part way and hiss.
            ev.push (closeEvent (T (0.17), pl, 0, D (0.03)));
            R (T (0.1), cParam ((Place) pl), c.area, 0.012);
            ev.push (openEvent (T (0.02), pl, D (0.03)));

            if (c.voiceless)
            {
                R (T (0.175), abd, OPEN, D (0.04));

                if (! keepOpen)
                    R (T (0.038), abd, ADD, 0.045);
            }
            else
            {
                R (T (0.1), abd, 0.1e-4, 0.02);
                R (T (0.03), abd, ADD, 0.04);

                // じ: hum through the closure, as a voiced stop does, until
                // it opens into its hiss.
                if (! o.phraseStart)
                {
                    R (T (0.17), velum, VELUM * voicedLeak, D (0.03));
                    R (T (0.1), velum, 0, 0.02);
                }
            }
        }
        else if (c.kind == Kind::h)
        {
            // は: the mouth keeps the vowel's shape, the glottis opens, breath only.
            R (T (0.12), abd, c.area, D (0.045));
            R (T (0.028), abd, ADD, 0.05);
        }
        else if (c.kind == Kind::nas)
        {
            // な・ま: close the mouth and lower the velum.
            R (T (0.12), velum, VELUM, D (0.035));
            ev.push (closeEvent (T (0.11), pl, 0, D (0.03)));
            ev.push (openEvent (T (0.012), pl, 0.022));
            R (t0 - 0.005, velum, 0, 0.04);
        }
        else if (c.kind == Kind::tap)
        {
            // ら: the tongue tip flicks the ridge, shut (tuned by ear; the
            // voice carrying through it is checked by the song tests).
            ev.push (closeEvent (T (0.055), pl, c.area, D (0.018)));
            ev.push (openEvent (T (0.022), pl, 0.02));
        }
        else if (c.kind == Kind::gl)
        {
            // や・わ: glide from い or う into the vowel.
            ev.push (vowelEvent (T (0.12), c.glide, 0.05, o.m));
        }
        else if (c.kind == Kind::lat)
        {
            // l: the tongue tip on the ridge, air past its sides; the voice goes on.
            ev.push (closeEvent (T (0.08), pl, c.area, D (0.03)));
            ev.push (openEvent (o.into != nullptr ? t0 + 0.005 : T (0.025), pl, 0.04));
        }
    }

    void hold (PlanState& st, int place)
    {
        for (auto& h : st.hold)
            if (h == place)
                return;

        for (auto& h : st.hold)
            if (h < 0)
            {
                h = (std::int8_t) place;
                return;
            }
    }

    void openHeld (PlanState& st, double t, double d, EventList& ev)
    {
        for (auto& h : st.hold)
            if (h >= 0)
            {
                ev.push (openEvent (t, h, d));
                h = -1;
            }
    }
}

//==============================================================================
std::vector<Mora> parseLyrics (const std::string& utf8)
{
    std::vector<Mora> out;
    auto s = decodeUtf8 (utf8);

    for (auto& c : s)
    {
        if (c >= 0xff01 && c <= 0xff5e)       // full-width ASCII (NFKC)
            c -= 0xfee0;
        else if (c >= 0x30a1 && c <= 0x30f6)  // katakana -> hiragana
            c -= 0x60;
    }

    const auto& kana = kanaTable();

    for (size_t i = 0; i < s.size(); i++)
    {
        const auto ch = s[i];

        if (isAsciiLetter (ch))   // a run of letters is one mora
        {
            size_t j = i;
            std::string word;

            while (j < s.size() && isAsciiLetter (s[j]))
                word += (char) s[j++];

            std::string lower = word;

            for (auto& l : lower)
                if (l >= 'A' && l <= 'Z')
                    l = (char) (l - 'A' + 'a');

            if (auto m = fromRomaji (word == "N" ? std::string ("N") : lower))
                out.push_back (*m);

            i = j - 1;
            continue;
        }

        if (ch == '-')
        {
            out.push_back ({ Cons::none, false, '-' });
            continue;
        }

        const auto it = kana.find (ch);

        if (it == kana.end())
            continue;

        const auto& base = it->second;
        const char32_t next = i + 1 < s.size() ? s[i + 1] : 0;
        std::optional<Mora> m;

        if (smallY (next) != 0 && ! base.empty() && base.back() == 'i')
        {
            // A contracted sound: きゃ しゅ ちょ ...
            const auto c = base.substr (0, base.size() - 1);
            m = fromRomaji ((c == "sh" || c == "ch" || c == "j" ? c : c + "y") + smallY (next));
            i++;
        }
        else if (smallV (next) != 0)
        {
            // ふぁ てぃ うぃ しぇ ...: the consonant as it is, the vowel replaced.
            m = fromRomaji (base);

            if (m)
            {
                m->v = smallV (next);

                if (ch == 0x3066 || ch == 0x3067 || ch == 0x3068 || ch == 0x3069)   // て で と ど
                    if (auto c = consFromString (base.substr (0, 1)))
                        m->c = *c;

                if (ch == 0x3046)   // う
                    m->c = Cons::w;
            }

            i++;
        }
        else
        {
            m = fromRomaji (base);
        }

        if (m)
            out.push_back (*m);
    }

    return out;
}

//==============================================================================
// English (ARPAbet, and words through the pronouncing dictionary)

namespace
{
    struct EnPhone
    {
        bool vowel = false;
        Cons c = Cons::none;
        Vowel v = vAA, v2 = vx;
        std::uint8_t stress = 1;
    };

    std::optional<EnPhone> enPhone (std::string tok)
    {
        int stress = -1;

        if (! tok.empty() && tok.back() >= '0' && tok.back() <= '2')
        {
            stress = tok.back() - '0';
            tok.pop_back();
        }

        for (auto& ch : tok)
            if (ch >= 'A' && ch <= 'Z')
                ch = (char) (ch - 'A' + 'a');

        struct V { const char* k; Vowel a, b; };
        static const V vowels[] = {
            { "aa", vAA, vx }, { "ae", vAE, vx }, { "ah", vAH, vx }, { "ao", vAO, vx }, { "aw", vAA, vUH }, { "ay", vAA, vIH },
            { "eh", vEH, vx }, { "er", vER, vx }, { "ey", vEYs, vIY }, { "ih", vIH, vx }, { "iy", vIY, vx }, { "ow", vOWs, vUH },
            { "oy", vAO, vIY }, { "uh", vUH, vx }, { "uw", vUW, vx }, { "ax", vAX, vx },
        };

        for (const auto& e : vowels)
            if (tok == e.k)
            {
                EnPhone p;
                p.vowel = true;
                p.v = e.a == vAH && stress == 0 ? vAX : e.a;   // a weak AH is the reduced vowel
                p.v2 = e.b;
                p.stress = (std::uint8_t) (stress < 0 ? 1 : stress);
                return p;
            }

        struct K { const char* k; Cons c; };
        static const K cons[] = {
            { "p", Cons::p }, { "b", Cons::b }, { "t", Cons::t }, { "d", Cons::d }, { "k", Cons::ek }, { "g", Cons::eg },
            { "ch", Cons::ch }, { "jh", Cons::j }, { "f", Cons::f }, { "v", Cons::v }, { "th", Cons::th }, { "dh", Cons::dh },
            { "s", Cons::s }, { "z", Cons::z }, { "sh", Cons::sh }, { "zh", Cons::zh }, { "hh", Cons::h }, { "m", Cons::m },
            { "n", Cons::n }, { "ng", Cons::ng }, { "l", Cons::l }, { "r", Cons::rr }, { "w", Cons::w }, { "y", Cons::y },
        };

        if (stress < 0)
            for (const auto& e : cons)
                if (tok == e.k)
                {
                    EnPhone p;
                    p.c = e.c;
                    return p;
                }

        return std::nullopt;
    }

    std::vector<std::string> splitOn (const std::string& s, const char* seps)
    {
        std::vector<std::string> out;
        std::string cur;

        for (char ch : s)
        {
            if (std::strchr (seps, ch) != nullptr)
            {
                if (! cur.empty())
                    out.push_back (cur);

                cur.clear();
            }
            else
            {
                cur += ch;
            }
        }

        if (! cur.empty())
            out.push_back (cur);

        return out;
    }

    // ARPAbet -> syllables: each vowel one; a single consonant between two
    // vowels starts the second, of two or more the first ends the first.
    std::vector<Mora> parseArpabet (const std::string& text)
    {
        std::vector<EnPhone> ph;

        for (const auto& tok : splitOn (text, " .\t"))
        {
            const auto p = enPhone (tok);

            if (! p)
                return {};

            ph.push_back (*p);
        }

        std::vector<size_t> vi;

        for (size_t i = 0; i < ph.size(); ++i)
            if (ph[i].vowel)
                vi.push_back (i);

        std::vector<Mora> out;

        for (size_t k = 0; k < vi.size(); ++k)
        {
            const size_t i = vi[k];
            const size_t prevV = k > 0 ? vi[k - 1] + 1 : 0, nextV = k + 1 < vi.size() ? vi[k + 1] : ph.size();
            std::vector<Cons> before, after;

            for (size_t j = prevV; j < i; ++j)
                before.push_back (ph[j].c);

            for (size_t j = i + 1; j < nextV; ++j)
                after.push_back (ph[j].c);

            if (k > 0 && before.size() > 1)
                before.erase (before.begin());   // went to the syllable before

            if (k + 1 < vi.size())
                after.resize (std::min<size_t> (after.size() > 1 ? 1 : 0, after.size()));

            Mora m;
            m.en = true;
            m.v = enVowelChar (ph[i].v);
            m.v2 = ph[i].v2 != vx ? enVowelChar (ph[i].v2) : 0;
            m.stress = ph[i].stress;

            if (! before.empty())
            {
                m.c = before.back();
                before.pop_back();
            }

            for (auto c : before)
                if (m.nPre < m.pre.size())
                    m.pre[m.nPre++] = c;

            for (auto c : after)
                if (m.nCoda < m.coda.size())
                    m.coda[m.nCoda++] = c;

            out.push_back (m);
        }

        return out;
    }

    struct Dictionary
    {
        const char* text = nullptr;
        std::size_t size = 0;
        std::once_flag built;
        std::unordered_map<std::string, std::string> words;
    };

    Dictionary& dictionary()
    {
        static Dictionary d;
        return d;
    }

    const std::string* lookUp (const std::string& word)
    {
        auto& d = dictionary();

        if (d.text == nullptr)
            return nullptr;

        std::call_once (d.built, [&d]
        {
            d.words.reserve (130000);
            std::size_t i = 0;

            while (i < d.size)
            {
                std::size_t e = i;

                while (e < d.size && d.text[e] != '\n')
                    ++e;

                const std::string line (d.text + i, e - i);

                if (const auto sp = line.find (' '); sp != std::string::npos)
                    d.words.emplace (line.substr (0, sp), line.substr (sp + 1));

                i = e + 1;
            }
        });

        const auto it = d.words.find (word);
        return it != d.words.end() ? &it->second : nullptr;
    }
}

void setEnglishDictionary (const char* text, std::size_t size)
{
    auto& d = dictionary();

    if (d.text == nullptr)
    {
        d.text = text;
        d.size = size;
    }
}

bool hasEnglishDictionary()  { return dictionary().text != nullptr; }

std::vector<Mora> parseEnglish (const std::string& utf8)
{
    std::string body;

    for (char ch : utf8)
        if (ch != '[' && ch != ']')
            body += ch;

    const auto tokens = splitOn (body, " \t\r\n");

    if (tokens.size() == 1 && tokens[0] == "+")
    {
        Mora m;
        m.v = '-';
        m.cont = true;
        return { m };
    }

    if (tokens.empty())
        return {};

    if (body.find_first_of (".012") == std::string::npos)
    {
        std::vector<Mora> out;
        bool all = true;

        for (auto word : tokens)
        {
            for (auto& ch : word)
                if (ch >= 'A' && ch <= 'Z')
                    ch = (char) (ch - 'A' + 'a');

            while (! word.empty() && ! ((word.front() >= 'a' && word.front() <= 'z') || word.front() == '\''))
                word.erase (word.begin());

            while (! word.empty() && ! ((word.back() >= 'a' && word.back() <= 'z') || word.back() == '\''))
                word.pop_back();

            const auto* pron = word.empty() ? nullptr : lookUp (word);

            if (pron == nullptr)
            {
                all = false;
                break;
            }

            // A word at a time: no consonant moves across a word's edge.
            const auto syllables = parseArpabet (*pron);
            out.insert (out.end(), syllables.begin(), syllables.end());
        }

        if (all && ! out.empty())
            return out;
    }

    return parseArpabet (body);
}

double pressureFor (int midi, char vowel, double center, double dyn)
{
    // Per vowel: [the factor at the middle of the range, the least pressure
    // that keeps a low note going, its increase per semitone below the middle].
    double k = 0.8, lo = 950, slope = 60;   // a, and the fallback

    switch (vowel)
    {
        case 'o': k = 1.0;  lo = 900;  slope = 50;  break;
        case 'e': k = 0.92; lo = 1500; slope = 130; break;
        case 'i': k = 1.08; lo = 850;  slope = 40;  break;
        case 'u': k = 1.08; lo = 900;  slope = 80;  break;
        default: break;
    }

    return std::min (2400.0, std::max ((880 + 30 * (midi - center)) * k, lo + slope * std::max (0.0, center - midi))) * dyn;
}

double leadTime (const Mora& mora)
{
    const auto c = consInfo (mora.c);
    double t = c.exists ? leadOf (c.kind) * c.lead : 0.03;

    for (int j = 0; j < (int) mora.nPre; ++j)
    {
        const auto p = consInfo (mora.pre[(size_t) j]);
        t += leadOf (p.kind) * p.lead * clusterLead;
    }

    return t;
}

//==============================================================================
std::vector<Expression> autoExpress (const std::vector<SungNote>& notes, double strength)
{
    const double k = strength;
    std::vector<Expression> out (notes.size());
    size_t i0 = 0;

    for (size_t i = 0; i <= notes.size(); i++)
    {
        const bool brk = i == notes.size() || (i > 0 && notes[i].t - (notes[i - 1].t + notes[i - 1].dur) > 0.12);

        if (! brk)
            continue;

        // The phrase i0 .. i-1.
        const auto L = (int) (i - i0);
        int top = -1;
        size_t topI = i0;

        for (size_t j = i0; j < i; j++)
            if (notes[j].midi > top || (notes[j].midi == top && notes[j].dur > notes[topI].dur))
            {
                top = notes[j].midi;
                topI = j;
            }

        bool prevScoop = false;

        for (size_t j = i0; j < i; j++)
        {
            const auto& n = notes[j];
            const bool first = j == i0, last = j == i - 1;
            const int up = first ? 0 : n.midi - notes[j - 1].midi, nextUp = last ? 0 : notes[j + 1].midi - n.midi;
            const bool plain = n.mora.v == '-' || n.mora.v == 'Q' || n.mora.v == 'N';
            const bool peak = ! first && up > 0 && nextUp <= 0;

            // A scoop up from below, only where there is room for one: the
            // start of a phrase, a leap up of 3 semitones or more, a long
            // note reached by going up. Never two running unless leaping.
            const double room = std::min (1.0, std::max (0.0, (n.dur - 0.16) / 0.3));
            double scoop = 0;

            if (! plain && first && n.dur >= 0.15)
            {
                scoop = 0.5 + 0.8 * room;
            }
            else if (! plain && n.dur >= 0.22)
            {
                if (up >= 3)
                    scoop = std::min (1.3, 0.35 + 0.13 * up) * (0.5 + 0.5 * room);
                else if (n.dur >= 0.45 && up > 0 && (peak || last))
                    scoop = 0.45 + 0.25 * room;
                else if (n.dur >= 0.9 && up == 0)
                    scoop = 0.4;

                if (scoop > 0 && prevScoop && up < 5)
                    scoop = 0;
            }

            prevScoop = scoop > 0;

            // Overshoot past the target on a leap; none on a short note.
            const int au = std::abs (up);
            double over = au >= 3 ? std::min (0.8, 0.2 + 0.07 * au) : au > 0 ? 0.08 * au : 0;
            over *= std::min (1.0, std::max (0.0, (n.dur - 0.12) / 0.25)) * (up < 0 ? 0.6 : 1);

            if (scoop > 0)
                over = std::max (over, 0.25 * room);

            // Accent: the phrase's peak and its start.
            const double accent = j == topI || first ? 1 : peak && n.dur >= 0.2 ? 0.6 : 0;

            // The end: short ones flick up and stop; long ones just fade.
            // (Not falling away of their own accord: a fall is a choice of
            // the song's, not something every long last note should do.)
            auto end = Expression::End::none;

            if (last && ! plain && n.mora.v != '-' && n.dur < 0.75)
                end = Expression::End::up;

            const double vib = n.dur >= 0.5 ? (last ? 1 : 0.7) : 0;

            // Dynamics: growing towards the phrase's peak.
            const double pos = L > 1 ? (double) (j - i0) / (L - 1) : 0.5, peakPos = L > 1 ? (double) (topI - i0) / (L - 1) : 0.5;
            const double arc = 1 - 0.22 * std::min (1.0, std::abs (pos - peakPos) * 1.6);
            const double g = 1 + k * ((arc - 1) + 0.2 * accent - 0.04);

            auto& x = out[j];
            x.scoop = scoop * k;
            x.accent = accent * k;
            x.end = k > 0 ? end : Expression::End::none;
            x.endDepth = k;
            x.vib = k > 0 ? vib * (0.6 + 0.4 * k) : (n.dur >= 0.6 ? 0.6 : 0);
            x.g = g;
            x.bounce = k;
            x.over = over * k;
        }

        i0 = i;
    }

    return out;
}

//==============================================================================
// An English syllable's second half (the prototype's planCoda): where a
// diphthong goes, and the consonants at the end, put at the end of the note
// (before the next note's consonants), the vowel held until then. A nasal
// hums on into the next note, as ん does; an l stays on the ridge; a nd's d
// is not released; the last consonant neither opens nor voices again.
static void planCoda (const SungNote& n, const SungNote* next, bool phraseEnd, PlanState& st,
                      double psNote, bool narrowV, EventList& ev)
{
    auto R = [&ev] (double t, Param p, double v, double d)  { ev.push (rampEvent (t, p, v, d)); };
    const auto& mora = n.mora;
    const double t0 = n.t, te = t0 + n.dur;
    const int m = n.midi;

    double span0 = 0;

    for (int k = 0; k < (int) mora.nCoda; ++k)
    {
        const auto c = consInfo (mora.coda[(size_t) k]);
        span0 += leadOf (c.kind) * c.lead;
    }

    const double scd = span0 > 0 ? std::min (1.0, 0.45 * n.dur / span0) : 1;
    const double nextLead = next != nullptr && ! phraseEnd && ! n.open ? std::min (leadTime (next->mora), 0.5 * n.dur) : 0;
    const double codaEnd = te - nextLead - (phraseEnd ? 0.02 : 0);
    double tv = n.open ? t0 + 0.35 : codaEnd - span0 * scd;   // where the vowel ends

    // The reduced vowel and an l (little, twinkle's -le): the syllable is the l.
    if (mora.v == enVowelChar (vAX) && mora.nCoda > 0 && consInfo (mora.coda[0]).kind == Kind::lat)
        tv = std::min (tv, t0 + 0.06);

    if (mora.v2 != 0)
    {
        const double vd = std::max (0.05, tv - t0);
        ev.push (vowelEvent (tv - std::min (0.15, 0.35 * vd), vowelOf (mora.v2), std::min (0.12, 0.3 * vd), m));
        st.vowel = mora.v2;
    }

    if (n.open)   // a live key: what comes after is unknown, so no final consonants
        return;

    double cur = tv;

    for (int k = 0; k < (int) mora.nCoda; ++k)
    {
        const auto c = consInfo (mora.coda[(size_t) k]);
        const double csc = scd * c.lead, span = leadOf (c.kind) * csc;
        const bool hasNext = k + 1 < (int) mora.nCoda;
        const auto nextC = hasNext ? consInfo (mora.coda[(size_t) k + 1]) : ConsInfo {};

        if (c.kind == Kind::nas)
        {
            R (cur, velum, VELUM, 0.04);
            ev.push (closeEvent (cur + 0.005, c.place, 0, 0.04));
            st.nasal = c.place;
        }
        else if (c.kind == Kind::lat)
        {
            ev.push (closeEvent (cur, c.place, c.area, 0.05));
            hold (st, c.place);
        }
        else if (c.kind == Kind::gl)
        {
            ev.push (vowelEvent (cur, c.glide, 0.1, m));
            st.vowel = enVowelChar (c.glide) > 0 && c.glide >= vIY ? enVowelChar (c.glide) : st.vowel;
        }
        else
        {
            const auto prevC = k > 0 ? consInfo (mora.coda[(size_t) k - 1]) : ConsInfo {};
            const bool held = c.kind == Kind::stop && prevC.exists && prevC.kind == Kind::nas && prevC.place == c.place;
            consGesture (ev, c, cur + span, csc, { false, psNote, narrowV, m, hasNext ? &nextC : nullptr,
                                                   held ? CodaMode::held : CodaMode::coda });

            if (held)
            {
                st.nasal = -1;   // the nose shut here; the mouth opens at the next note
                hold (st, c.place);
            }
            else if (! hasNext)
            {
                if (c.voiceless)
                    st.abdOpen = true;

                if (c.kind == Kind::fric)
                    hold (st, c.place);
            }
        }

        cur += span;
    }
}

//==============================================================================
void planNote (const SungNote& n, const SungNote* prev, const SungNote* next,
               PlanState& st, double center, EventList& ev)
{
    const auto& mora = n.mora;
    const double t0 = n.t;
    const int m = n.midi;
    const double dyn = n.dyn;

    auto R = [&ev] (double t, Param p, double v, double d)  { ev.push (rampEvent (t, p, v, d)); };
    const int onsetFrom = ev.size;   // the moves into the note start here (see `speed`)

    const double gapBefore = prev != nullptr ? t0 - (prev->t + prev->dur) : 9;
    const double gapAfter = n.open ? 0 : next != nullptr ? next->t - (t0 + n.dur) : 9;
    const bool phraseStart = gapBefore > 0.12, phraseEnd = ! n.open && gapAfter > 0.12;
    const bool special = mora.v == 'N' || mora.v == 'Q' || mora.v == '-';
    const char vowel = special ? 0 : mora.v;
    auto c = special ? ConsInfo {} : consInfo (mora.c);

    // English: a t or d between vowels, starting a weak syllable, is a flap
    // (little: the tongue flicks the ridge).
    if (mora.en && (mora.c == Cons::t || mora.c == Cons::d) && mora.stress == 0 && mora.nPre == 0
         && prev != nullptr && prev->mora.en && prev->mora.nCoda == 0 && gapBefore <= 0.12)
        c = consInfo (Cons::fl);

    const bool weak = mora.en && mora.stress == 0;   // an English weak syllable: softer, unaccented
    const char sung = vowel != 0 ? vowel : st.vowel >= 0 ? (char) st.vowel : 'a';   // the vowel sounding in this mora
    const bool narrowV = isNarrow (sung);
    const int up = prev != nullptr && ! phraseStart ? m - prev->midi : 0;
    const auto x = n.hasExpression ? n.x : liveExpr (phraseStart, up);
    // English stress: a primary-stressed syllable gets at least a middling accent.
    const double accent = weak ? 0 : mora.en && mora.stress == 1 ? std::max (x.accent, 0.5) : x.accent;
    const double psNote = pressureFor (m, sung, center, dyn) * (weak ? 0.85 : 1 + 0.12 * accent);

    // After a short note the consonant shrinks (half of the vowel before is
    // kept). The sibilants dig deep into the note before instead, to keep
    // their hiss: a quarter longer after a long one, all but 75ms of a short one.
    // After a consonant, a w, y or r glides quickly (tw, pl, fr): drawn out,
    // it sounds as a vowel of its own.
    const double lk = c.exists ? c.lead * (mora.nPre > 0 && c.kind == Kind::gl ? 0.5 : 1) : 1;
    double lead = c.exists ? leadOf (c.kind) * lk : 0.03;
    const bool sib = c.exists && (c.kind == Kind::fric || c.kind == Kind::aff);
    const double sc0 = c.exists && ! phraseStart
                          ? (sib ? (prev->dur >= 0.5 ? 1.25 : std::min (1.0, std::max (0.55, std::min (prev->dur * 0.85, prev->dur - 0.075) / lead)))
                                 : std::min (1.0, std::max (0.55, prev->dur * 0.6 / lead)))
                          : 1;
    lead *= sc0;
    const double sc = sc0 * lk;   // the times within the consonant shrink from its kind's lead

    // With the consonants of a cluster before it, how long before t0 each
    // opens, and how long before the vowel it all starts.
    std::array<double, 4> preAt {};
    double leadAll = lead;

    for (int j = (int) mora.nPre - 1; j >= 0; --j)
    {
        const auto pc = consInfo (mora.pre[(size_t) j]);
        preAt[(size_t) j] = leadAll;
        leadAll += leadOf (pc.kind) * sc0 * clusterLead * pc.lead;
    }

    const auto first = mora.nPre > 0 ? consInfo (mora.pre[0]) : c;   // the consonant that comes first

    // Reopen what a ん closed (or carry on, into a nasal at the same place).
    if (st.nasal >= 0 && mora.v != 'N')
    {
        const double tn = t0 - leadAll - 0.01;

        if (! (c.exists && c.kind == Kind::nas && c.place == st.nasal))
            ev.push (openEvent (tn, st.nasal, 0.03));

        if (! (c.exists && c.kind == Kind::nas))
            R (tn, velum, 0, 0.04);

        st.nasal = -1;
    }

    // Open what an English syllable's end left closed (an l, a held nd, a final fricative).
    openHeld (st, t0 - leadAll - 0.01, 0.04, ev);

    // Close the glottis a っ or a voiceless consonant opened.
    if (st.abdOpen && ! (first.exists && first.voiceless) && mora.v != 'Q')
    {
        R (t0 - std::max (leadAll, 0.04), abd, ADD, 0.04);
        st.abdOpen = false;
    }

    // Consonants before the vowel's own (English st, tw): the furthest from
    // the vowel first, a little shorter.
    for (int j = (int) mora.nPre - 1; j >= 0; --j)
    {
        const auto pc = consInfo (mora.pre[(size_t) j]);
        const double psc = sc0 * clusterLead * pc.lead;
        const auto nextC = j + 1 < (int) mora.nPre ? consInfo (mora.pre[(size_t) j + 1]) : c;
        GestureContext o { phraseStart, psNote, narrowV, m, nextC.exists ? &nextC : nullptr, CodaMode::none };
        consGesture (ev, pc, t0 - preAt[(size_t) j], psc, o);
    }

    if (c.exists)
    {
        consGesture (ev, c, t0, sc, { phraseStart, psNote, narrowV, m, nullptr, CodaMode::none });

        if (c.voiceless)
            st.abdOpen = false;
    }

    // The vowel: the tongue heads for it during the consonant. A contracted
    // sound passes through い.
    if (vowel != 0)
    {
        if (c.exists && c.kind == Kind::gl)
        {
            ev.push (vowelEvent (t0 - 0.035, vowelOf (vowel), 0.1, m));
        }
        else if (mora.pal && vowel != 'i')
        {
            ev.push (vowelEvent (t0 - std::max (lead, 0.07), vi, 0.06, m));
            ev.push (vowelEvent (t0 - 0.03, vowelOf (vowel), 0.1, m));
        }
        else
        {
            // The same vowel again: ease the breath for a moment and say it anew.
            if (! c.exists && ! phraseStart && st.vowel == vowel && prev != nullptr && prev->mora.v != 'Q')
                R (t0 - 0.085, ps, pressureFor (m, vowel, center, dyn) * 0.6, 0.04);

            ev.push (vowelEvent (t0 - std::min (lead, 0.07), vowelOf (vowel), 0.09, m));
        }

        st.vowel = vowel;

        if (mora.en)
            planCoda (n, next, phraseEnd, st, psNote, narrowV, ev);
    }
    else if (mora.v == '-')
    {
        ev.push (vowelEvent (t0 - 0.03, vowelOf (sung), 0.08, m));   // ー: refit the mouth to the pitch
    }
    else if (mora.v == 'N')
    {
        // ん: close the mouth, into the nose, at the next consonant's place
        // (さんぽ m, ほんと n, げんき ng).
        int pl = uvu;

        if (next != nullptr && ! phraseEnd)
        {
            const auto nc = consInfo (next->mora.c);

            if (nc.exists && nc.place >= 0)
                pl = nc.place == pal || nc.place == apl || nc.place == den ? alv : nc.place;
        }

        R (t0 - 0.04, velum, VELUM, 0.045);
        ev.push (closeEvent (t0 - 0.03, pl, 0, 0.045));
        st.nasal = pl;
    }
    else if (mora.v == 'Q')
    {
        // っ: the next consonant lengthened by the beat. A fricative hisses
        // through it (いっしょ), a stop holds its closure silent, anything
        // else holds the breath.
        const auto nc = next != nullptr ? consInfo (next->mora.c) : ConsInfo {};

        if (nc.exists && nc.kind == Kind::fric)
        {
            ev.push (closeEvent (t0 - 0.035, nc.place, nc.area, 0.04));
            R (t0 - 0.045, abd, nc.voiceless ? OPEN : 0.11e-4, 0.045);
            st.abdOpen = nc.voiceless;
        }
        else if (nc.exists && nc.kind == Kind::h)
        {
            R (t0 - 0.045, abd, nc.area, 0.045);
            st.abdOpen = true;
        }
        else
        {
            if (nc.exists && (nc.kind == Kind::stop || nc.kind == Kind::aff))
                ev.push (closeEvent (t0 - 0.02, nc.place, 0, 0.03));
            else
                R (t0 - 0.02, ps, 250, 0.05);

            if (! nc.exists || nc.voiceless)
            {
                R (t0 - 0.02, abd, 0.22e-4, 0.04);
                st.abdOpen = true;
            }
        }
    }

    // Pitch: a scoop from below, an overshoot on a leap, both done within
    // about 0.1s of the note's start.
    if (mora.v == 'Q')
    {
        R (t0 - 0.045, pitch, m, 0.075);
    }
    else if (x.scoop > 0 && mora.v != '-')
    {
        if (phraseStart)
            R (t0 - leadAll - 0.12, pitch, m - x.scoop, 0);
        else
            R (t0 - 0.09, pitch, m - x.scoop * 0.85, 0.05);

        R (t0 - 0.04, pitch, m + x.over, 0.085);

        if (x.over > 0)
            R (t0 + 0.05, pitch, m, 0.08);
    }
    else
    {
        const double ov = (up < 0 ? -1 : 1) * x.over;
        R (t0 - 0.05, pitch, m + ov, 0.065);

        if (! same (ov, 0))
            R (t0 + 0.02, pitch, m, 0.08);
    }

    // Dynamics: a short note leans on its start, a long one swells.
    if (mora.v != 'Q')
    {
        const double g = x.g * (weak ? 0.85 : 1), bo = x.bounce;

        if (mora.v == '-')
        {
            R (t0 - 0.03, gain, g, 0.08);
        }
        else if (n.open || n.dur < 0.5)
        {
            R (t0 - 0.035, gain, g * (1 + 0.14 * bo), 0.04);
            R (t0 + 0.06, gain, g * (1 - 0.1 * bo), std::min (0.2, std::max (0.05, n.dur * 0.6)));
        }
        else
        {
            R (t0 - 0.035, gain, g * (1 - 0.06 * bo), 0.04);   // and swells, below
        }
    }

    // Breath.
    if (mora.v != 'Q')
    {
        const double psv = psNote;

        if (phraseStart)
        {
            if (gapBefore > 0.7 && ! n.open)
            {
                // A breath in before the phrase: glottis open, the pressure negative.
                const double tb = t0 - leadAll - 0.42;
                R (tb, abd, 0.5e-4, 0.08);
                R (tb + 0.02, ps, -260, 0.08);
                R (tb + 0.2, ps, 0, 0.09);
                R (tb + 0.26, abd, first.exists && first.voiceless ? OPEN : ADD, 0.08);
                ev.push (vowelEvent (tb, vowelOf (sung), 0.25, m));
            }

            // Starting on a consonant that closes or narrows the mouth, push
            // only once it has: before, the vowel sounds first (measure as
            // "i-measure"), and a flow cut off half way through closing clicks.
            const bool stopStart = first.exists && (first.kind == Kind::stop || first.kind == Kind::aff || first.kind == Kind::nas
                                                     || first.kind == Kind::lat || first.vz);
            R (stopStart ? t0 - leadAll + 0.035 : t0 - leadAll - 0.035, ps, psv, stopStart ? 0.04 : 0.07);
        }
        else if (mora.v == '-')
        {
            R (t0 - 0.03, ps, psv, 0.09);
        }
        else
        {
            R (t0 - 0.03, ps, psv * 1.1, 0.06);   // a push on the start, to bounce
            R (t0 + 0.07, ps, psv, 0.14);
        }
    }

    // Everything so far is the move into the note: the consonant, the
    // tongue heading for the vowel, the pitch's scoop and settle, the push
    // of breath and the accent on the start. `speed` makes all of it
    // quicker (or slower) about the note's start, together, so a fast song
    // can have crisp consonants and quick slurs. Slowed, a consonant digs
    // no further into the note before than 60% of it.
    if (! same (n.speed, 1.0) && n.speed > 0)
    {
        double k = 1.0 / n.speed;

        if (k > 1 && prev != nullptr && ! phraseStart)
        {
            double earliest = t0;

            for (int i = onsetFrom; i < ev.size; ++i)
                earliest = std::min (earliest, ev.events[i].t);

            if (t0 - earliest > 0)
                k = std::min (k, std::max (t0 - earliest, prev->dur * 0.6) / (t0 - earliest));
        }

        for (int i = onsetFrom; i < ev.size; ++i)
        {
            auto& e = ev.events[i];
            e.t = t0 + (e.t - t0) * k;
            e.duration *= k;
        }
    }

    // A long note swells and eases off, over its own length.
    if (mora.v != 'Q' && mora.v != '-' && ! n.open && n.dur >= 0.5)
    {
        const double gw = x.g * (weak ? 0.85 : 1);
        R (t0 + 0.1, gain, gw * (1 + 0.12 * x.bounce), n.dur * 0.5);
        R (t0 + n.dur * 0.68, gain, gw * (1 - 0.08 * x.bounce), n.dur * 0.3);
    }

    // Vibrato, from part way through a held note.
    R (t0 - 0.03, vib, 0, 0.07);

    if (! n.open && x.vib > 0 && mora.v != 'Q')
        R (t0 + 0.24, vib, (n.dur > 1.2 ? 30 : 20) * x.vib, 0.4);

    if (n.open)
        R (t0 + 0.4, vib, 24 * x.vib, 0.6);

    if (phraseEnd)
    {
        const double te = t0 + n.dur, d = x.endDepth;

        if (x.end == Expression::End::up)
        {
            // Flick the pitch up at the very end and stop.
            const double w = std::min (0.11, n.dur * 0.4);
            R (te - w, pitch, m + 2.4 * d, w + 0.02);
            R (te - w * 0.6, gain, 0.25, w * 0.6 + 0.03);
        }
        else if (x.end == Expression::End::fall)
        {
            // Fall away as the voice fades.
            const double w = std::min (0.24, n.dur * 0.3);
            R (te - w, pitch, m - 3 * d, w + 0.08);
            R (te - w, gain, 0.5, w + 0.05);
        }

        planRelease (te, n.dur, gapAfter, st, narrowV || (st.vowel >= 0 && isNarrow ((char) st.vowel)), x.end == Expression::End::up, ev);
    }
}

void planRelease (double te, double dur, double gapAfter, PlanState& st, bool narrow, bool quick, EventList& ev)
{
    auto R = [&ev] (double t, Param p, double v, double d)  { ev.push (rampEvent (t, p, v, d)); };

    const double rel = quick ? 0.06 : std::min (0.5, std::max (0.12, dur * 0.25));
    const double fo = quick ? 0.05 : tailFo * rel + tailFo0;
    const double over = std::max (0.08, std::min (tailOver, gapAfter - (gapAfter > 0.7 ? 0.6 : 0.2)));   // clear of the next breath or consonant

    R (te - fo, abd, narrow ? 0.07e-4 : tailAbd, fo + over);
    R (te - fo, air, narrow ? tailAirNarrow : tailAir, fo);
    R (te - fo * tailPsAt, ps, 0, fo * tailPsAt + over);
    R (te - rel * 0.3, vib, 0, rel * 0.6);

    const double t2 = te + over;
    R (t2, air, 1, 0.02);
    R (t2 + 0.05, gain, 1, 0.02);

    if (gapAfter <= 0.7 || gapAfter >= 9)
        R (t2, abd, ADD, 0.06);

    if (st.nasal >= 0)
    {
        ev.push (openEvent (t2, st.nasal, 0.08));
        R (t2, velum, 0, 0.08);
        st.nasal = -1;
    }

    openHeld (st, t2, 0.08, ev);

    // A narrow vowel relaxes the mouth before the voice has gone.
    ev.push (vowelEvent (narrow ? te - fo * 0.6 : te + rel * 0.5, vx, narrow ? 0.2 : 0.25, 0));
    st.abdOpen = false;
}

void planRelease (double te, double dur, double gapAfter, PlanState& st, EventList& ev)
{
    planRelease (te, dur, gapAfter, st, st.vowel >= 0 && isNarrow ((char) st.vowel), false, ev);
}

//==============================================================================
std::vector<Event> planScore (const std::vector<SungNote>& notes, double start, double center, double strength)
{
    std::vector<Event> out;
    PlanState st;
    const auto xs = autoExpress (notes, strength);

    auto at = [&] (size_t k)
    {
        auto n = notes[k];
        n.t = notes[k].t + start;
        n.hasExpression = true;
        n.x = xs[k];
        return n;
    };

    EventList list;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto cur = at (i);
        std::optional<SungNote> prev, next;

        if (i > 0)
            prev = at (i - 1);

        if (i + 1 < notes.size())
            next = at (i + 1);

        list.size = 0;
        planNote (cur, prev ? &*prev : nullptr, next ? &*next : nullptr, st, center, list);
        out.insert (out.end(), list.events, list.events + list.size);
    }

    for (auto& e : out)
        if (e.t < 0)
            e.t = 0;

    std::stable_sort (out.begin(), out.end(), [] (const Event& a, const Event& b) { return a.t < b.t; });
    return out;
}

std::vector<std::vector<Event>> planPart (const std::vector<PartNote>& notes, double center, double strength, double speed,
                                          bool english)
{
    std::vector<SungNote> sung;
    std::vector<size_t> source;

    std::vector<std::vector<Mora>> per (notes.size());

    for (size_t k = 0; k < notes.size(); ++k)
        per[k] = english ? parseEnglish (notes[k].lyric.empty() ? std::string ("aa1") : notes[k].lyric)
                         : parseLyrics (notes[k].lyric.empty() ? std::string ("\xe3\x81\x82") : notes[k].lyric);

    // English: a note whose lyric is "+" carries on the word of the note
    // before, a syllable each (the last note takes what is left over; with
    // syllables run short, the vowel before is held).
    if (english)
        for (size_t i = 0; i < per.size(); ++i)
        {
            size_t k = 0;

            while (i + k + 1 < per.size() && per[i + k + 1].size() == 1 && per[i + k + 1][0].cont)
                ++k;

            if (k == 0)
                continue;

            const auto all = per[i];
            Mora dash;
            dash.v = '-';

            for (size_t j = 0; j <= k; ++j)
            {
                if (j < k)
                    per[i + j] = { j < all.size() ? all[j] : dash };
                else
                    per[i + j] = all.size() > k ? std::vector<Mora> (all.begin() + (long) k, all.end()) : std::vector<Mora> { dash };
            }

            i += k;
        }

    for (size_t k = 0; k < notes.size(); ++k)
    {
        const auto& n = notes[k];
        const auto& moras = per[k];

        // Two syllables or more on a note: a stressed one longer, a weak one shorter.
        double sum = 0;

        for (const auto& m : moras)
            sum += m.en ? (m.stress == 1 ? 1.3 : m.stress == 2 ? 1.1 : 0.7) : 1;

        double at = 0;

        for (size_t i = 0; i < moras.size(); ++i)
        {
            const double w = moras[i].en ? (moras[i].stress == 1 ? 1.3 : moras[i].stress == 2 ? 1.1 : 0.7) : 1;
            SungNote s;
            s.t = n.start + at;
            s.dur = n.length * w / sum;
            at += s.dur;
            s.midi = n.midi;
            s.mora = moras[i];
            s.speed = speed;
            sung.push_back (s);
            source.push_back (k);
        }
    }

    auto xs = autoExpress (sung, strength);

    for (size_t i = 0; i < sung.size(); ++i)
    {
        sung[i].hasExpression = true;
        sung[i].x = xs[i];
        sung[i].x.g *= (double) notes[source[i]].velocity / 100.0;
    }

    std::vector<std::vector<Event>> out (notes.size());
    PlanState st;
    EventList list;

    for (size_t i = 0; i < sung.size(); ++i)
    {
        list.size = 0;
        planNote (sung[i], i > 0 ? &sung[i - 1] : nullptr, i + 1 < sung.size() ? &sung[i + 1] : nullptr, st, center, list);
        auto& events = out[source[i]];
        events.insert (events.end(), list.events, list.events + list.size);
    }

    return out;
}

} // namespace carve::voice::vocal
