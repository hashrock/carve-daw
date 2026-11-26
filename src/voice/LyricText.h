#pragma once

#include <vector>

#include <juce_core/juce_core.h>

// A line of lyrics as text <-> one lyric per note: what the piano roll's
// Lyrics panel pours over a run of notes, and what it shows them as when it
// opens, so the same box both types a song in and rewrites one.
//
// How the text is cut depends on who sings it, because each singer reads a
// note's lyric its own way:
//
//   voicebank  a mora a note -- a kana, with the small kana after it
//              (きゃ). っ, ん and ー are notes of their own, ー sung as the
//              vowel before it (かー -> か, あ), which is how a connected
//              voicebank names a held vowel. Romaji is read as the kana it
//              spells.
//   modelled   the same, but っ, ん and ー stay on the note before (かっ, さん,
//              かー): the modelled singer sings them inside that note. A
//              space before one gives it a note of its own.
//   english    a note per syllable: a word goes on its first note, and the
//              notes after it that carry the rest of it are "+" (see
//              voice::vocal::parseEnglish). The syllables are counted from the
//              pronouncing dictionary; a hyphen counts them by hand
//              (twin-kle). A "+" in the text is a note too, which holds
//              the syllable before it on over one more.
//
// Everywhere: spaces and punctuation separate but are not notes, and
// [anything] is exactly one note with exactly that lyric -- a voicebank alias
// with a space in it ([a か]), two kana for the modelled singer to share out
// over one note ([とう]), a word kept whole ([hello]), or [] for a note left to
// sing the default.
namespace carve::voice::lyrics
{

enum class Reading
{
    voicebank,
    modelled,
    english
};

// The text, cut into one lyric per note in singing order.
std::vector<juce::String> split (const juce::String& text, Reading);

// A note's lyric for join, and whether a breath comes before it (a phrase
// starts there: see model::singing::followsOn).
struct Sung
{
    juce::String lyric;
    bool startsPhrase = false;
};

// The notes as text that split reads back as the same lyrics, a phrase a
// line. A note with no lyric is written as what it sings: あ, or [] for the
// English singer, whose default has no spelling.
juce::String join (const std::vector<Sung>&, Reading);

} // namespace carve::voice::lyrics
