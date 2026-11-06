#pragma once

#include <array>
#include <map>
#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

namespace carve::voice
{

// The text files a UTAU voicebank is described by -- oto.ini, character.txt,
// prefix.map -- are Shift_JIS from Windows UTAU, or UTF-8 from OpenUtau and
// newer tools, with nothing in the file to say which. Valid UTF-8 is taken as
// UTF-8 (a BOM is dropped); anything else is decoded as Shift_JIS (cp932).
juce::String decodeVoicebankText (const juce::MemoryBlock&);
juce::String readVoicebankText (const juce::File&);

// One line of an oto.ini: which stretch of which recording sings an alias.
// All times are in milliseconds from the start of the file, as UTAU writes
// them.
//
//   offset      where the sound begins
//   consonant   how long the part after offset is that must never be
//               stretched (the consonant, in a CV sample; the vowel-to-
//               consonant transition, in a VCV one)
//   cutoff      where the sound ends: negative, that far after offset;
//               positive, that far before the end of the file; zero, at the
//               end of the file
//   preutter    how far after offset the note itself begins -- everything
//               before it (the consonant) sounds before the note's start
//   overlap     how far after offset this sound has faded in over the end of
//               the one before
struct OtoEntry
{
    juce::String alias;
    juce::File file;
    double offsetMs = 0.0, consonantMs = 0.0, cutoffMs = 0.0, preutterMs = 0.0, overlapMs = 0.0;
};

// The vowel a lyric ends on, as connected (VCV) voicebanks name it: "a", "i",
// "u", "e", "o", or "n" for ん. Kana (either script, small kana and ー
// included) and romaji both work. Empty for anything else -- a rest, a breath,
// an alias the table does not know.
juce::String vowelOf (const juce::String& lyric);

// A voicebank: every oto.ini under its folder, merged. A singer's folder
// usually holds several banks -- single-sound (CV), connected (VCV), extras
// -- each in a subfolder with its own oto.ini, and a lyric is sung from
// whichever has the best alias for it (see resolve).
class Voicebank
{
public:
    // Reads the folder and the oto.ini files at most two levels below it.
    // Nothing on failure, with the reason in `error`.
    static std::shared_ptr<const Voicebank> load (const juce::File& folder, juce::String& error);

    // The same, but shared: two generators singing from one voicebank parse
    // its files once. A folder whose oto.ini files changed since is read again.
    static std::shared_ptr<const Voicebank> loadShared (const juce::File& folder, juce::String& error);

    // The name from character.txt, or the folder's name.
    static juce::String readName (const juce::File& folder);

    juce::File getFolder() const      { return folder; }
    juce::String getName() const      { return name; }
    int getNumAliases() const         { return (int) entries.size(); }

    const OtoEntry* find (const juce::String& alias) const;

    // Which recording sings `lyric` on `midiNote`, given the lyric of the note
    // it follows on from -- empty when it starts a phrase (nothing directly
    // before it). In order, the first alias the voicebank has:
    //
    //   - the lyric as written, when it is already an alias with a space in
    //     it ("a か", "- か"): whoever typed that meant it
    //   - following on: "<vowel of the previous lyric> <lyric>" -- the
    //     connected sound, which is what makes a VCV bank sing legato
    //   - starting a phrase: "- <lyric>"
    //   - "<lyric>", then "- <lyric>", then "* <lyric>"
    //
    // Each is tried with the pitch's prefix/suffix from prefix.map first (the
    // bank's recordings for that register), then without. A lyric in romaji
    // that finds nothing is tried again as the kana it spells. Nothing when
    // none of them is there.
    const OtoEntry* resolve (const juce::String& lyric, const juce::String& previousLyric, int midiNote) const;

    // The recording live notes (a keyboard, the piano roll's previews) are
    // played from: the bank's plain "あ", or failing that its first alias.
    const OtoEntry* getPreviewEntry() const;

private:
    const OtoEntry* resolveAsWritten (const juce::String& lyric, const juce::String& previousLyric, int midiNote) const;
    void addOto (const juce::File& otoFile);
    void readPrefixMap (const juce::File&);

    juce::File folder;
    juce::String name;
    std::map<juce::String, OtoEntry> entries;
    std::array<std::pair<juce::String, juce::String>, 128> prefixMap;   // by MIDI note: prefix, suffix
    juce::String signature;   // every oto.ini's path and time, to tell a changed bank
};

// Hiragana for a lyric typed in romaji ("ka" -> か, "kya" -> きゃ, "nn" ->
// ん, "kka" -> っか, "-" -> ー), or empty when it is not romaji.
juce::String romajiToKana (const juce::String& romaji);

// For the piano roll's note names and prefix.map: "C4" is MIDI 60, as UTAU
// has it. -1 for anything that is not a note name.
int midiNoteFromName (const juce::String&);

} // namespace carve::voice
