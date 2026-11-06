// The UTAU voicebank reader (src/voice/Voicebank.h): text encodings, the
// vowel a lyric ends on, note names, and which oto.ini alias sings a lyric.
//
// Every string with kana in it is spelled out in UTF-8 bytes, so the file
// compiles the same whatever the compiler's source encoding.

#include <catch2/catch_test_macros.hpp>

#include "model/SongModel.h"
#include "voice/Voicebank.h"

namespace
{

using carve::voice::Voicebank;

juce::String u8 (const char* utf8)  { return juce::String (juce::CharPointer_UTF8 (utf8)); }

const auto ka  = u8 ("\xe3\x81\x8b");              // か
const auto a   = u8 ("\xe3\x81\x82");              // あ
const auto kya = u8 ("\xe3\x81\x8d\xe3\x82\x83");  // きゃ
const auto n   = u8 ("\xe3\x82\x93");              // ん
const auto KA  = u8 ("\xe3\x82\xab");              // カ
const auto kaa = u8 ("\xe3\x81\x8b\xe3\x83\xbc");  // かー

// A voicebank on disk with this oto.ini (and, optionally, prefix.map).
struct TempBank
{
    explicit TempBank (const juce::String& oto, const juce::MemoryBlock* rawOto = nullptr)
    {
        folder.createDirectory();

        if (rawOto != nullptr)
            folder.getChildFile ("oto.ini").replaceWithData (rawOto->getData(), rawOto->getSize());
        else
            folder.getChildFile ("oto.ini").replaceWithText (oto, false, false, "\n");
    }

    ~TempBank()  { folder.deleteRecursively(); }

    juce::File folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getNonexistentChildFile ("carve-voicebank-test", "", false);
};

} // namespace

TEST_CASE ("vowelOf reads the vowel a lyric ends on", "[voicebank]")
{
    using carve::voice::vowelOf;

    CHECK (vowelOf (ka) == "a");
    CHECK (vowelOf (kya) == "a");       // a small kana is the vowel
    CHECK (vowelOf (n) == "n");
    CHECK (vowelOf (KA) == "a");        // katakana too
    CHECK (vowelOf (kaa) == "a");       // ー lengthens what came before
    CHECK (vowelOf ("ka") == "a");      // romaji
    CHECK (vowelOf ("n") == "n");
    CHECK (vowelOf ("k").isEmpty());
    CHECK (vowelOf ({}).isEmpty());
}

TEST_CASE ("midiNoteFromName reads UTAU note names with C4 as 60", "[voicebank]")
{
    using carve::voice::midiNoteFromName;

    CHECK (midiNoteFromName ("C4") == 60);
    CHECK (midiNoteFromName ("A4") == 69);
    CHECK (midiNoteFromName ("C#4") == 61);
    CHECK (midiNoteFromName ("B7") == 107);
    CHECK (midiNoteFromName ("C-1") == 0);
    CHECK (midiNoteFromName ("H4") == -1);
    CHECK (midiNoteFromName ("C") == -1);
}

TEST_CASE ("voicebank text is UTF-8 when it is valid UTF-8, Shift_JIS otherwise", "[voicebank]")
{
    // か in Shift_JIS is 0x82 0xA9: not valid UTF-8.
    const juce::uint8 sjis[] = { 0x82, 0xa9 };
    CHECK (carve::voice::decodeVoicebankText (juce::MemoryBlock (sjis, sizeof (sjis))) == ka);

    // The same character in UTF-8, with and without a BOM.
    const juce::uint8 utf8[] = { 0xe3, 0x81, 0x8b };
    const juce::uint8 bom[] = { 0xef, 0xbb, 0xbf, 0xe3, 0x81, 0x8b };
    CHECK (carve::voice::decodeVoicebankText (juce::MemoryBlock (utf8, sizeof (utf8))) == ka);
    CHECK (carve::voice::decodeVoicebankText (juce::MemoryBlock (bom, sizeof (bom))) == ka);
}

TEST_CASE ("an oto.ini line is read field by field", "[voicebank]")
{
    TempBank bank ("_ka.wav=" + ka + ",24,56,-73,5,20\n"
                   + a + ".wav=,10,20,0,30,40\n");

    juce::String error;
    const auto loaded = Voicebank::load (bank.folder, error);
    REQUIRE (loaded != nullptr);
    CHECK (error.isEmpty());
    CHECK (loaded->getNumAliases() == 2);

    const auto* entry = loaded->find (ka);
    REQUIRE (entry != nullptr);
    CHECK (entry->file == bank.folder.getChildFile ("_ka.wav"));
    CHECK (entry->offsetMs == 24.0);
    CHECK (entry->consonantMs == 56.0);
    CHECK (entry->cutoffMs == -73.0);
    CHECK (entry->preutterMs == 5.0);
    CHECK (entry->overlapMs == 20.0);

    // An empty alias is the file's own name.
    CHECK (loaded->find (a) != nullptr);
}

TEST_CASE ("a Shift_JIS oto.ini is read", "[voicebank]")
{
    // "_か.wav=か,1,2,3,4,5" in Shift_JIS.
    const juce::uint8 line[] = { '_', 0x82, 0xa9, '.', 'w', 'a', 'v', '=', 0x82, 0xa9,
                                 ',', '1', ',', '2', ',', '3', ',', '4', ',', '5', '\n' };
    const juce::MemoryBlock raw (line, sizeof (line));
    TempBank bank ({}, &raw);

    juce::String error;
    const auto loaded = Voicebank::load (bank.folder, error);
    REQUIRE (loaded != nullptr);

    const auto* entry = loaded->find (ka);
    REQUIRE (entry != nullptr);
    CHECK (entry->file.getFileName() == "_" + ka + ".wav");
}

TEST_CASE ("a lyric is sung by the connected alias when it follows on, the CV one otherwise", "[voicebank]")
{
    // A bank with both kinds, the way a singer with CV and VCV banks merges.
    TempBank bank ("ka.wav=" + ka + ",0,0,0,0,0\n"
                   "ka_start.wav=- " + ka + ",0,0,0,0,0\n"
                   "a_ka.wav=a " + ka + ",0,0,0,0,0\n");

    juce::String error;
    const auto loaded = Voicebank::load (bank.folder, error);
    REQUIRE (loaded != nullptr);

    auto aliasFor = [&] (const juce::String& lyric, const juce::String& previous)
    {
        const auto* entry = loaded->resolve (lyric, previous, 60);
        return entry != nullptr ? entry->alias : juce::String ("(none)");
    };

    CHECK (aliasFor (ka, a) == "a " + ka);         // after あ: the connected sound
    CHECK (aliasFor (ka, {}) == "- " + ka);        // a phrase start
    CHECK (aliasFor (ka, n) == ka);                // no "n か" recorded: the CV one
    CHECK (aliasFor ("a " + ka, {}) == "a " + ka); // typed as an alias: taken as typed
    CHECK (aliasFor (a, {}) == "(none)");          // nothing sings あ
}

TEST_CASE ("romaji becomes the kana it spells", "[voicebank]")
{
    using carve::voice::romajiToKana;

    CHECK (romajiToKana ("ka") == ka);
    CHECK (romajiToKana ("kya") == kya);
    CHECK (romajiToKana ("KA") == ka);
    CHECK (romajiToKana ("nn") == n);
    CHECK (romajiToKana ("kaa") == ka + a);
    CHECK (romajiToKana ("ka-") == kaa);
    CHECK (romajiToKana ("kka") == u8 ("\xe3\x81\xa3") + ka);   // っか
    CHECK (romajiToKana ("shi") == u8 ("\xe3\x81\x97"));         // し
    CHECK (romajiToKana (ka).isEmpty());                          // already kana
    CHECK (romajiToKana ("x1").isEmpty());
}

TEST_CASE ("a romaji lyric sings the kana recording", "[voicebank]")
{
    TempBank bank ("ka.wav=" + ka + ",0,0,0,0,0\n"
                   "a_ka.wav=a " + ka + ",0,0,0,0,0\n");

    juce::String error;
    const auto loaded = Voicebank::load (bank.folder, error);
    REQUIRE (loaded != nullptr);

    CHECK (loaded->resolve ("ka", {}, 60)->alias == ka);
    CHECK (loaded->resolve ("ka", "a", 60)->alias == "a " + ka);   // the vowel of a romaji lyric too
}

TEST_CASE ("prefix.map picks the register's recording first", "[voicebank]")
{
    TempBank bank (ka + ".wav=" + ka + ",0,0,0,0,0\n"
                   + ka + "_hi.wav=" + ka + "_H,0,0,0,0,0\n");
    bank.folder.getChildFile ("prefix.map").replaceWithText ("C5\t\t_H\nC4\t\t\n", false, false, "\n");

    juce::String error;
    const auto loaded = Voicebank::load (bank.folder, error);
    REQUIRE (loaded != nullptr);

    CHECK (loaded->resolve (ka, {}, 72)->alias == ka + "_H");   // C5: the high recording
    CHECK (loaded->resolve (ka, {}, 60)->alias == ka);          // C4: no suffix
}

TEST_CASE ("a note's lyric is stored only when set", "[voicebank]")
{
    juce::ValueTree tree (carve::model::ids::NOTE);
    carve::model::Note note (tree);

    CHECK (note.getLyric().isEmpty());

    note.setLyric ("  " + ka + " ", nullptr);
    CHECK (note.getLyric() == ka);

    note.setLyric ("   ", nullptr);
    CHECK (! tree.hasProperty (carve::model::ids::lyric));
}

TEST_CASE ("a singer's voice settings are clamped and stored only when not the default", "[voicebank]")
{
    using carve::model::Generator;
    namespace ids = carve::model::ids;

    juce::ValueTree tree (ids::GENERATOR);
    Generator generator (tree);

    CHECK (generator.getVoiceStyle() == Generator::VoiceStyle {});

    generator.setVoiceStyleValue (ids::voiceGender, 250.0, nullptr);
    generator.setVoiceStyleValue (ids::voiceBreath, 80.0, nullptr);
    CHECK (generator.getVoiceStyle().gender == 100.0);
    CHECK (generator.getVoiceStyle().breath == 80.0);

    generator.setVoiceStyleValue (ids::voiceBreath, 50.0, nullptr);   // back to as recorded
    CHECK (! tree.hasProperty (ids::voiceBreath));

    juce::UndoManager um;
    generator.setVoiceStyleValue (ids::voiceModulation, 40.0, &um);
    um.undo();
    CHECK (generator.getVoiceStyle().modulation == 0.0);
}
