// Lyrics as text <-> a lyric a note (src/voice/LyricText.h): how each singer's
// text is cut, and that what join writes reads back as the notes it came from.
//
// Kana are spelled out in UTF-8 bytes, so the file compiles the same whatever
// the compiler's source encoding.

#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>

#include "voice/LyricText.h"
#include "voice/VocalScore.h"

namespace
{

using carve::voice::lyrics::Reading;
using carve::voice::lyrics::Sung;
using carve::voice::lyrics::join;
using carve::voice::lyrics::split;
using Lyrics = std::vector<juce::String>;

juce::String u8 (const char* utf8)  { return juce::String (juce::CharPointer_UTF8 (utf8)); }

const auto a    = u8 ("\xe3\x81\x82");              // あ
const auto ka   = u8 ("\xe3\x81\x8b");              // か
const auto ki   = u8 ("\xe3\x81\x8d");              // き
const auto ra   = u8 ("\xe3\x82\x89");              // ら
const auto ta   = u8 ("\xe3\x81\x9f");              // た
const auto sa   = u8 ("\xe3\x81\x95");              // さ
const auto po   = u8 ("\xe3\x81\xbd");              // ぽ
const auto n    = u8 ("\xe3\x82\x93");              // ん
const auto tsu  = u8 ("\xe3\x81\xa3");              // っ (small)
const auto bar  = u8 ("\xe3\x83\xbc");              // ー
const auto kya  = u8 ("\xe3\x81\x8d\xe3\x82\x83");  // きゃ
const auto KYA  = u8 ("\xe3\x82\xad\xe3\x83\xa3");  // キャ
const auto to   = u8 ("\xe3\x81\xa8");              // と
const auto u    = u8 ("\xe3\x81\x86");              // う
const auto comma = u8 ("\xe3\x80\x81");             // 、
const auto hoshi = u8 ("\xe6\x98\x9f");             // 星

// The words VocalModelTests reads English with: the dictionary is set once a
// process, so both suites hand in the same one.
const char* const testDictionary =
    "hello hh ah0 l ow1\n"
    "friends f r eh1 n d z\n"
    "little l ih1 t ah0 l\n"
    "star s t aa1 r\n"
    "twinkle t w ih1 ng k ah0 l\n"
    "my m ay1\n";

void useTestDictionary()
{
    static const std::string text (testDictionary);
    carve::voice::vocal::setEnglishDictionary (text.data(), text.size());
}

std::vector<Sung> sung (const Lyrics& lyrics)
{
    std::vector<Sung> notes;

    for (const auto& lyric : lyrics)
        notes.push_back ({ lyric, false });

    return notes;
}

} // namespace

TEST_CASE ("kana are cut a mora a note, small kana with the one before", "[lyrics]")
{
    for (auto reading : { Reading::voicebank, Reading::modelled })
    {
        CHECK (split (ki + ra + ki + ra, reading) == Lyrics { ki, ra, ki, ra });
        CHECK (split (kya + ka, reading) == Lyrics { kya, ka });
        CHECK (split (KYA, reading) == Lyrics { KYA });                    // katakana as typed
        CHECK (split (ka + " " + ki + comma + ra + "\n" + ta, reading) == Lyrics { ka, ki, ra, ta });
        CHECK (split ("kirakira", reading) == Lyrics { ki, ra, ki, ra });   // romaji is the kana it spells
        CHECK (split ("[a " + ka + "] [" + to + u + "]", reading) == Lyrics { "a " + ka, to + u });
        CHECK (split ("[]", reading) == Lyrics { "" });
        CHECK (split (hoshi + a, reading) == Lyrics { hoshi, a });          // a kanji is a note, if a silent one
        CHECK (split ("  " + comma, reading).empty());
    }
}

TEST_CASE ("a voicebank sings っ, ん and ー on notes of their own", "[lyrics]")
{
    CHECK (split (sa + n + po, Reading::voicebank) == Lyrics { sa, n, po });
    CHECK (split (ka + tsu + ta, Reading::voicebank) == Lyrics { ka, tsu, ta });
    CHECK (split (ka + bar, Reading::voicebank) == Lyrics { ka, a });           // held: the vowel again
    CHECK (split (kya + u8 ("\xe3\x80\x9c"), Reading::voicebank) == Lyrics { kya, a });   // 〜 too
    CHECK (split (sa + n + bar, Reading::voicebank) == Lyrics { sa, n, n });
}

TEST_CASE ("the modelled singer keeps っ, ん and ー on the note before, unless spaced", "[lyrics]")
{
    CHECK (split (sa + n + po, Reading::modelled) == Lyrics { sa + n, po });
    CHECK (split (ka + tsu + ta, Reading::modelled) == Lyrics { ka + tsu, ta });
    CHECK (split (ka + bar, Reading::modelled) == Lyrics { ka + bar });
    CHECK (split (sa + " " + n + po, Reading::modelled) == Lyrics { sa, n, po });
}

TEST_CASE ("English is a note a syllable, the rest of a word on + notes", "[lyrics]")
{
    useTestDictionary();

    CHECK (split ("Twinkle, twinkle little star", Reading::english)
           == Lyrics { "Twinkle", "+", "twinkle", "+", "little", "+", "star" });
    CHECK (split ("my friends!", Reading::english) == Lyrics { "my", "friends" });
    CHECK (split ("twin-kle sta-r", Reading::english) == Lyrics { "twinkle", "+", "star", "+" });   // a hyphen counts by hand
    CHECK (split ("star + +", Reading::english) == Lyrics { "star", "+", "+" });
    CHECK (split ("[hello] xyzzy", Reading::english) == Lyrics { "hello", "xyzzy" }); // kept whole; unknown is one
    CHECK (split ("hh.ah0.l.ow1", Reading::english) == Lyrics { "hh.ah0.l.ow1", "+" });
    CHECK (split ("[] ...", Reading::english) == Lyrics { "" });
}

TEST_CASE ("join writes the notes plainly where it can", "[lyrics]")
{
    useTestDictionary();

    CHECK (join (sung ({ ki, ra, ki, ra }), Reading::voicebank) == ki + ra + ki + ra);
    CHECK (join (sung ({ "", ka }), Reading::voicebank) == a + ka);                 // unset sings あ
    CHECK (join (sung ({ "a " + ka }), Reading::voicebank) == "[a " + ka + "]");
    CHECK (join (sung ({ sa, n }), Reading::modelled) == sa + " " + n);             // spaced, or it would join
    CHECK (join (sung ({ "ka" }), Reading::voicebank) == "[ka]");                   // romaji kept as typed
    CHECK (join ({ { ka, false }, { ki, true } }, Reading::voicebank) == ka + "\n" + ki);

    CHECK (join (sung ({ "twinkle", "+", "star" }), Reading::english) == "twinkle star");
    CHECK (join (sung ({ "hello" }), Reading::english) == "[hello]");               // two syllables on one note
    CHECK (join (sung ({ "star", "+" }), Reading::english) == "star +");
    CHECK (join (sung ({ "+", "" }), Reading::english) == "+ []");
}

TEST_CASE ("what join writes, split reads back as the notes", "[lyrics]")
{
    useTestDictionary();

    const Lyrics kanaPool { "", a, ka, ki, kya, KYA, n, tsu, bar, sa + n, ka + bar, to + u, "a " + ka, "ka", hoshi, "+", " " };
    const Lyrics englishPool { "", "+", "hello", "star", "my", "friends", "little", "xyzzy", "hh.ah0.l.ow1", "a b", "Star!" };

    for (auto reading : { Reading::voicebank, Reading::modelled, Reading::english })
    {
        const auto& pool = reading == Reading::english ? englishPool : kanaPool;

        rc::check ("join then split", [&]
        {
            const auto picks = *rc::gen::container<std::vector<std::size_t>> (rc::gen::inRange<std::size_t> (0, pool.size()));
            const auto breaths = *rc::gen::container<std::vector<bool>> (picks.size(), rc::gen::arbitrary<bool>());
            std::vector<Sung> notes;
            Lyrics wanted;

            for (std::size_t i = 0; i < picks.size(); ++i)
            {
                const auto& lyric = pool[picks[i]];
                notes.push_back ({ lyric, breaths[i] });

                const auto trimmed = lyric.trim();
                wanted.push_back (trimmed.isEmpty() && reading != Reading::english ? a : trimmed);
            }

            RC_ASSERT (split (join (notes, reading), reading) == wanted);
        });
    }
}
