#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "VocalModel.h"

// Japanese lyrics and notes -> what the body does, and when: the port of the
// web-voice prototype's voice-jp.js (parseLyrics, autoExpress, planNote,
// planRelease, planScore, leadTime, pressureFor).
//
// A mora is a consonant and a vowel. In singing the vowel starts on the beat,
// so the consonant goes before it, into the end of the note before. Every
// consonant is written as where the tongue or lips narrow the tract, how far,
// when, and whether the glottis opens: the hiss and the bursts are what the
// synth makes of that (VocalModel.h).
//
// planNote and planRelease write into a fixed-size EventList and allocate
// nothing, so a live key can be planned on the audio thread. The rest is for
// the message thread.
namespace carve::voice::vocal
{

// English adds v, th, dh, zh, ng, l (air past the tongue's sides), rr (the
// English r: glides from the r-coloured vowel), fl (the flap of little's t),
// and ek, eg (English k and g, with a burst of their own).
enum class Cons : std::uint8_t { none, k, g, t, d, p, b, s, z, sh, hy, f, h, ch, ts, j, m, n, ny, r, y, w,
                                 v, th, dh, zh, ng, l, rr, fl, ek, eg };

// The English vowels, as Mora::v: 1 + their offset from vIY (control
// characters, so none collides with a Japanese vowel or N, Q, -).
constexpr char enVowelChar (Vowel v)   { return (char) (1 + (int) v - (int) vIY); }

struct Mora
{
    Cons c = Cons::none;
    bool pal = false;    // a contracted sound (きゃ): passes through the い shape
    char v = 'a';        // a i u e o, or N (ん), Q (っ), - (ー); or an English vowel (enVowelChar)

    // English syllables: the consonants before c (st, tw: c is the last of
    // them), those after the vowel, where a diphthong goes, and the stress
    // (0 weak, 1 primary, 2 secondary).
    bool en = false;
    std::uint8_t nPre = 0, nCoda = 0;
    std::array<Cons, 3> pre {};
    std::array<Cons, 4> coda {};
    char v2 = 0;         // a diphthong's second vowel (enVowelChar), 0 for none
    std::uint8_t stress = 1;
    bool cont = false;   // "+": the rest of the word on the note before (planPart shares it out)
};

// Hiragana, katakana and romaji (a run of letters is one mora). Anything it
// cannot read -- spaces, punctuation -- is skipped. UTF-8 in. Full-width
// letters are read as letters; half-width katakana are not read.
std::vector<Mora> parseLyrics (const std::string& utf8);

// English: words looked up in the pronouncing dictionary (any number, split
// by spaces), or ARPAbet if it has a dot or a stress digit in it
// ("hh.ah0.l.ow1"). Each vowel is a syllable. "+" alone is the rest of the
// word on the note before. Brackets around it ([hello]) are allowed.
std::vector<Mora> parseEnglish (const std::string& utf8);

// The pronouncing dictionary (the CMU Pronouncing Dictionary as the
// prototype's tools/build-en-dict.mjs writes it: "word arpabet..." a line).
// Read once, on first use, from whatever was handed here; until then English
// words read as nothing (ARPAbet still reads).
void setEnglishDictionary (const char* text, std::size_t size);
bool hasEnglishDictionary();

// How a note is sung, beyond its pitch (autoExpress's result): a scoop up
// from below and an overshoot past the pitch [semitones], an accent, how the
// end of a phrase goes, the vibrato, and the dynamics.
struct Expression
{
    enum class End : std::uint8_t { none, up, fall };

    double scoop = 0, accent = 0;
    End end = End::none;
    double endDepth = 1, vib = 0, g = 1, bounce = 1, over = 0;
};

// One mora to sing, in seconds. `open` is a held key: it lasts until it is
// let go of. Without an expression it is sung as a live key is (liveExpr).
struct SungNote
{
    double t = 0, dur = 0;
    int midi = 60;
    Mora mora;
    double dyn = 1;
    bool open = false;
    double speed = 1;    // how quickly it moves into the note: 2 takes half the time
    bool hasExpression = false;
    Expression x;
};

// What carries over from one mora to the next.
struct PlanState
{
    int vowel = -1;      // the vowel last sung ('a'...), -1 for none
    int nasal = -1;      // the Place a ん closed, -1 for none
    bool abdOpen = false;

    // Places an English syllable's end left closed or narrowed (an l, the d
    // of a held nd, a final fricative), opened at the next note's start.
    std::array<std::int8_t, 4> hold { -1, -1, -1, -1 };
};

struct EventList
{
    static constexpr int capacity = 128;   // an English syllable with a cluster and a coda takes more than a mora
    Event events[capacity];
    int size = 0;

    void push (const Event& e)
    {
        if (size < capacity)
            events[size++] = e;
    }
};

// The lung pressure for a note [Pa]: higher notes push harder. `center` is
// Synth::getCenter().
double pressureFor (int midi, char vowel, double center, double dyn = 1);

// How long before the vowel the consonant takes.
double leadTime (const Mora&);

// The expression for every mora of a score, from the shape of its phrases
// (a gap of more than 0.12s ends one). strength 0 sings it as written.
std::vector<Expression> autoExpress (const std::vector<SungNote>& notes, double strength = 1);

void planNote (const SungNote& note, const SungNote* previous, const SungNote* next,
               PlanState& state, double center, EventList& out);

// The end of a phrase: the folds slacken into breath and the voice fades.
void planRelease (double te, double dur, double gapAfter, PlanState& state, bool narrow, bool quick, EventList& out);
void planRelease (double te, double dur, double gapAfter, PlanState& state, EventList& out);   // narrow from the last vowel

// A whole score, as the prototype's planScore: notes in seconds from `start`,
// the events in time order.
std::vector<Event> planScore (const std::vector<SungNote>& notes, double start, double center, double strength = 1);

// A note of a part as the DAW has it, and the moras it is sung as (a note
// with two kana sings them in equal halves).
struct PartNote
{
    double start = 0, length = 0;   // seconds
    int midi = 60;
    int velocity = 100;
    std::string lyric;              // UTF-8; empty sings あ
};

// A part's notes in singing order -> each note's events, at absolute times
// on the part's clock. Velocity scales the note's dynamics (100 = as planned).
// A note whose lyric reads as nothing has no events.
// `speed` is SungNote::speed for every note.
// `english` reads the lyrics with parseEnglish (and shares a word out over
// the "+" notes after it) rather than parseLyrics.
std::vector<std::vector<Event>> planPart (const std::vector<PartNote>& notes, double center, double strength, double speed = 1,
                                          bool english = false);

} // namespace carve::voice::vocal
