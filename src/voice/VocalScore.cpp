#include "VocalScore.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

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
    // Consonants: where, what kind, voiceless or not, and the area a
    // fricative narrows to.

    enum class Kind : std::uint8_t { stop, fric, aff, h, nas, tap, gl };

    struct ConsInfo
    {
        bool exists = false;
        int place = -1;      // -1: none (h, the glides)
        Kind kind = Kind::stop;
        bool voiceless = false;
        double area = 0;
        Vowel glide = va;
    };

    ConsInfo consInfo (Cons c)
    {
        switch (c)
        {
            case Cons::k:  return { true, vel, Kind::stop, true, 0, va };
            case Cons::g:  return { true, vel, Kind::stop, false, 0, va };
            case Cons::t:  return { true, alv, Kind::stop, true, 0, va };
            case Cons::d:  return { true, alv, Kind::stop, false, 0, va };
            case Cons::p:  return { true, lab, Kind::stop, true, 0, va };
            case Cons::b:  return { true, lab, Kind::stop, false, 0, va };
            case Cons::s:  return { true, den, Kind::fric, true, 0.10e-4, va };
            case Cons::z:  return { true, den, Kind::fric, false, 0.12e-4, va };
            case Cons::sh: return { true, apl, Kind::fric, true, 0.11e-4, va };
            case Cons::hy: return { true, pal, Kind::fric, true, 0.22e-4, va };
            case Cons::f:  return { true, lab, Kind::fric, true, 0.16e-4, va };
            case Cons::h:  return { true, -1, Kind::h, false, 0, va };
            case Cons::ch: return { true, apl, Kind::aff, true, 0.11e-4, va };
            case Cons::ts: return { true, den, Kind::aff, true, 0.10e-4, va };
            case Cons::j:  return { true, apl, Kind::aff, false, 0.13e-4, va };
            case Cons::m:  return { true, lab, Kind::nas, false, 0, va };
            case Cons::n:  return { true, alv, Kind::nas, false, 0, va };
            case Cons::ny: return { true, apl, Kind::nas, false, 0, va };
            case Cons::r:  return { true, alv, Kind::tap, false, 0, va };
            case Cons::y:  return { true, -1, Kind::gl, false, 0, vi };
            case Cons::w:  return { true, -1, Kind::gl, false, 0, vu };
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
        }

        return 0.03;
    }

    constexpr double ADD = 0.05e-4, OPEN = 0.30e-4;   // the glottis [m^2]: voicing, a voiceless consonant
    constexpr double VELUM = 1.0e-4;                   // the velum, for a nasal

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
            default:  return vx;
        }
    }

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
    return c.exists ? leadOf (c.kind) : 0.03;
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

            // The end: short ones flick up and stop, long ones fall away.
            auto end = Expression::End::none;

            if (last && ! plain)
                end = n.dur < 0.75 ? Expression::End::up : Expression::End::fall;

            if (last && n.mora.v == '-')
                end = Expression::End::fall;

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
void planNote (const SungNote& n, const SungNote* prev, const SungNote* next,
               PlanState& st, double center, EventList& ev)
{
    const auto& mora = n.mora;
    const double t0 = n.t;
    const int m = n.midi;
    const double dyn = n.dyn;

    auto R = [&ev] (double t, Param p, double v, double d)  { ev.push (rampEvent (t, p, v, d)); };

    const double gapBefore = prev != nullptr ? t0 - (prev->t + prev->dur) : 9;
    const double gapAfter = n.open ? 0 : next != nullptr ? next->t - (t0 + n.dur) : 9;
    const bool phraseStart = gapBefore > 0.12, phraseEnd = ! n.open && gapAfter > 0.12;
    const bool special = mora.v == 'N' || mora.v == 'Q' || mora.v == '-';
    const char vowel = special ? 0 : mora.v;
    const auto c = special ? ConsInfo {} : consInfo (mora.c);
    const char sung = vowel != 0 ? vowel : st.vowel >= 0 ? (char) st.vowel : 'a';   // the vowel sounding in this mora
    const bool narrowV = sung == 'i' || sung == 'u';
    const int up = prev != nullptr && ! phraseStart ? m - prev->midi : 0;
    const auto x = n.hasExpression ? n.x : liveExpr (phraseStart, up);
    const double psNote = pressureFor (m, sung, center, dyn) * (1 + 0.12 * x.accent);

    // After a short note the consonant shrinks (half of the vowel before is
    // kept). The sibilants dig deep into the note before instead, to keep
    // their hiss: a quarter longer after a long one, all but 75ms of a short one.
    double lead = c.exists ? leadOf (c.kind) : 0.03;
    const bool sib = c.exists && (c.kind == Kind::fric || c.kind == Kind::aff);
    const double sc = c.exists && ! phraseStart
                          ? (sib ? (prev->dur >= 0.5 ? 1.25 : std::min (1.0, std::max (0.55, std::min (prev->dur * 0.85, prev->dur - 0.075) / lead)))
                                 : std::min (1.0, std::max (0.55, prev->dur * 0.6 / lead)))
                          : 1;
    lead *= sc;
    auto T = [t0, sc] (double dt)  { return t0 - dt * sc; };
    auto D = [sc] (double d)       { return d * sc; };

    // Reopen what a ん closed (or carry on, into a nasal at the same place).
    if (st.nasal >= 0 && mora.v != 'N')
    {
        const double tn = t0 - lead - 0.01;

        if (! (c.exists && c.kind == Kind::nas && c.place == st.nasal))
            ev.push (openEvent (tn, st.nasal, 0.03));

        if (! (c.exists && c.kind == Kind::nas))
            R (tn, velum, 0, 0.04);

        st.nasal = -1;
    }

    // Close the glottis a っ or a voiceless consonant opened.
    if (st.abdOpen && ! (c.exists && c.voiceless) && mora.v != 'Q')
    {
        R (t0 - std::max (lead, 0.04), abd, ADD, 0.04);
        st.abdOpen = false;
    }

    if (c.exists)
    {
        const int pl = c.place;

        if (c.kind == Kind::stop)
        {
            // Close, let the pressure build, and the release bursts. Pushed
            // harder while closed, for a clearer release.
            ev.push (closeEvent (T (0.1), pl, 0, D (0.028)));
            ev.push (openEvent (T (0.016), pl, D (0.018)));

            if (c.voiceless)
            {
                R (T (0.105), abd, 0.26e-4, D (0.035));
                R (t0 + (narrowV ? -0.006 : 0.004), abd, ADD, 0.04);
            }

            if (! phraseStart)
                R (T (0.085), ps, psNote * 1.3, D (0.04));
        }
        else if (c.kind == Kind::fric)
        {
            // Narrow with the air flowing. Voiced (ざ) opens the glottis a little.
            ev.push (closeEvent (T (0.17), pl, c.area, 0.028));
            ev.push (openEvent (T (0.024), pl, D (0.035)));

            if (c.voiceless)
            {
                R (T (0.18), abd, OPEN, D (0.045));
                R (T (0.042), abd, ADD, 0.045);
            }
            else
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
                R (T (0.038), abd, ADD, 0.045);
            }
            else
            {
                R (T (0.1), abd, 0.1e-4, 0.02);
                R (T (0.03), abd, ADD, 0.04);
            }
        }
        else if (c.kind == Kind::h)
        {
            // は: the mouth keeps the vowel's shape, the glottis opens, breath only.
            R (T (0.12), abd, 0.42e-4, D (0.045));
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
            // ら: the tongue tip flicks the ridge.
            ev.push (closeEvent (T (0.055), pl, 0.006e-4, D (0.018)));
            ev.push (openEvent (T (0.022), pl, 0.02));
        }
        else if (c.kind == Kind::gl)
        {
            // や・わ: glide from い or う into the vowel.
            ev.push (vowelEvent (T (0.12), c.glide, 0.05, m));
        }

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
            R (t0 - 0.045, abd, 0.42e-4, 0.045);
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
            R (t0 - lead - 0.12, pitch, m - x.scoop, 0);
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
        const double g = x.g, bo = x.bounce;

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
            R (t0 - 0.035, gain, g * (1 - 0.06 * bo), 0.04);
            R (t0 + 0.1, gain, g * (1 + 0.12 * bo), n.dur * 0.5);
            R (t0 + n.dur * 0.68, gain, g * (1 - 0.08 * bo), n.dur * 0.3);
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
                const double tb = t0 - lead - 0.42;
                R (tb, abd, 0.5e-4, 0.08);
                R (tb + 0.02, ps, -260, 0.08);
                R (tb + 0.2, ps, 0, 0.09);
                R (tb + 0.26, abd, c.exists && c.voiceless ? OPEN : ADD, 0.08);
                ev.push (vowelEvent (tb, vowelOf (sung), 0.25, m));
            }

            // Starting on a stop: push only once it is closed (a flow cut
            // off half way through closing clicks).
            const bool stopStart = c.exists && (c.kind == Kind::stop || c.kind == Kind::aff);
            R (stopStart ? t0 - lead + 0.035 : t0 - lead - 0.035, ps, psv, stopStart ? 0.04 : 0.07);
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

        planRelease (te, n.dur, gapAfter, st, narrowV, x.end == Expression::End::up, ev);
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

    // A narrow vowel relaxes the mouth before the voice has gone.
    ev.push (vowelEvent (narrow ? te - fo * 0.6 : te + rel * 0.5, vx, narrow ? 0.2 : 0.25, 0));
    st.abdOpen = false;
}

void planRelease (double te, double dur, double gapAfter, PlanState& st, EventList& ev)
{
    planRelease (te, dur, gapAfter, st, st.vowel == 'i' || st.vowel == 'u', false, ev);
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

std::vector<std::vector<Event>> planPart (const std::vector<PartNote>& notes, double center, double strength)
{
    std::vector<SungNote> sung;
    std::vector<size_t> source;

    for (size_t k = 0; k < notes.size(); ++k)
    {
        const auto& n = notes[k];
        const auto moras = parseLyrics (n.lyric.empty() ? std::string ("\xe3\x81\x82") : n.lyric);
        const auto count = (double) moras.size();

        for (size_t i = 0; i < moras.size(); ++i)
        {
            SungNote s;
            s.t = n.start + (double) i * n.length / count;
            s.dur = n.length / count;
            s.midi = n.midi;
            s.mora = moras[i];
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
