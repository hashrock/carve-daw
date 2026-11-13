#pragma once

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

enum class Cons : std::uint8_t { none, k, g, t, d, p, b, s, z, sh, hy, f, h, ch, ts, j, m, n, ny, r, y, w };

struct Mora
{
    Cons c = Cons::none;
    bool pal = false;    // a contracted sound (きゃ): passes through the い shape
    char v = 'a';        // a i u e o, or N (ん), Q (っ), - (ー)
};

// Hiragana, katakana and romaji (a run of letters is one mora). Anything it
// cannot read -- spaces, punctuation -- is skipped. UTF-8 in. Full-width
// letters are read as letters; half-width katakana are not read.
std::vector<Mora> parseLyrics (const std::string& utf8);

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
    bool hasExpression = false;
    Expression x;
};

// What carries over from one mora to the next.
struct PlanState
{
    int vowel = -1;      // the vowel last sung ('a'...), -1 for none
    int nasal = -1;      // the Place a ん closed, -1 for none
    bool abdOpen = false;
};

struct EventList
{
    static constexpr int capacity = 64;
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
std::vector<std::vector<Event>> planPart (const std::vector<PartNote>& notes, double center, double strength);

} // namespace carve::voice::vocal
