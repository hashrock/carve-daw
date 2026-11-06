#include "Voicebank.h"

#include <cstring>
#include <mutex>

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <iconv.h>
#endif

namespace carve::voice
{

//==============================================================================
// Text

namespace
{
    juce::String decodeShiftJis (const char* data, size_t size)
    {
       #if JUCE_WINDOWS
        const auto wide = MultiByteToWideChar (932, 0, data, (int) size, nullptr, 0);

        if (wide <= 0)
            return {};

        std::vector<wchar_t> buffer ((size_t) wide);
        MultiByteToWideChar (932, 0, data, (int) size, buffer.data(), wide);
        return juce::String (buffer.data(), (size_t) wide);
       #else
        // CP932 is Microsoft's Shift_JIS, which is what Windows UTAU wrote:
        // plain SHIFT_JIS lacks its extensions (the circled numbers, ～ and a
        // few more a voicebank author might use). Both libiconv and glibc
        // know it by this name.
        auto cd = iconv_open ("UTF-8", "CP932");

        if (cd == (iconv_t) -1)
            cd = iconv_open ("UTF-8", "SHIFT_JIS");

        if (cd == (iconv_t) -1)
            return {};

        std::string out;
        out.resize (size * 3 + 16);

        auto* in = const_cast<char*> (data);
        auto inLeft = size;
        auto* outPtr = out.data();
        auto outLeft = out.size();

        while (inLeft > 0)
        {
            if (iconv (cd, &in, &inLeft, &outPtr, &outLeft) == (size_t) -1)
            {
                // A byte that is not Shift_JIS: skip it and carry on, so one
                // bad line costs that line rather than the whole file.
                if (errno == EILSEQ || errno == EINVAL)
                {
                    ++in;
                    --inLeft;
                    continue;
                }

                break;
            }
        }

        iconv_close (cd);
        out.resize (out.size() - outLeft);
        return juce::String (juce::CharPointer_UTF8 (out.c_str()));
       #endif
    }
}

juce::String decodeVoicebankText (const juce::MemoryBlock& bytes)
{
    auto* data = static_cast<const char*> (bytes.getData());
    auto size = bytes.getSize();

    if (size >= 3 && (juce::uint8) data[0] == 0xef && (juce::uint8) data[1] == 0xbb && (juce::uint8) data[2] == 0xbf)
    {
        data += 3;
        size -= 3;
    }

    if (juce::CharPointer_UTF8::isValidString (data, (int) size))
        return juce::String (juce::CharPointer_UTF8 (data), size);

    return decodeShiftJis (data, size);
}

juce::String readVoicebankText (const juce::File& file)
{
    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
        return {};

    return decodeVoicebankText (bytes);
}

//==============================================================================
// Vowels

namespace
{
    juce::juce_wchar toHiragana (juce::juce_wchar c)
    {
        // Katakana ァ..ヶ sit 0x60 above their hiragana.
        if (c >= 0x30a1 && c <= 0x30f6)
            return c - 0x60;

        return c;
    }

    juce::String vowelOfKana (juce::juce_wchar c)
    {
        static const std::map<juce::juce_wchar, const char*> table = []
        {
            std::map<juce::juce_wchar, const char*> t;

            auto add = [&t] (const char* utf8, const char* vowel)
            {
                const juce::String chars (juce::CharPointer_UTF8 { utf8 });

                for (auto ch : chars)
                    t[ch] = vowel;
            };

            add ("\xe3\x81\x82\xe3\x81\x8b\xe3\x81\x95\xe3\x81\x9f\xe3\x81\xaa\xe3\x81\xaf\xe3\x81\xbe\xe3\x82\x84\xe3\x82\x89\xe3\x82\x8f"
                 "\xe3\x81\x8c\xe3\x81\x96\xe3\x81\xa0\xe3\x81\xb0\xe3\x81\xb1\xe3\x81\x81\xe3\x82\x83\xe3\x82\x8e\xe3\x82\x95", "a");   // あかさたなはまやらわ がざだばぱ ぁゃゎゕ
            add ("\xe3\x81\x84\xe3\x81\x8d\xe3\x81\x97\xe3\x81\xa1\xe3\x81\xab\xe3\x81\xb2\xe3\x81\xbf\xe3\x82\x8a\xe3\x82\x90"
                 "\xe3\x81\x8e\xe3\x81\x98\xe3\x81\xa2\xe3\x81\xb3\xe3\x81\xb4\xe3\x81\x83", "i");                                     // いきしちにひみりゐ ぎじぢびぴ ぃ
            add ("\xe3\x81\x86\xe3\x81\x8f\xe3\x81\x99\xe3\x81\xa4\xe3\x81\xac\xe3\x81\xb5\xe3\x82\x80\xe3\x82\x86\xe3\x82\x8b"
                 "\xe3\x81\x90\xe3\x81\x9a\xe3\x81\xa5\xe3\x81\xb6\xe3\x81\xb7\xe3\x81\x85\xe3\x82\x85\xe3\x82\x94", "u");             // うくすつぬふむゆる ぐずづぶぷ ぅゅゔ
            add ("\xe3\x81\x88\xe3\x81\x91\xe3\x81\x9b\xe3\x81\xa6\xe3\x81\xad\xe3\x81\xb8\xe3\x82\x81\xe3\x82\x8c\xe3\x82\x91"
                 "\xe3\x81\x92\xe3\x81\x9c\xe3\x81\xa7\xe3\x81\xb9\xe3\x81\xba\xe3\x81\x87\xe3\x82\x96", "e");                         // えけせてねへめれゑ げぜでべぺ ぇゖ
            add ("\xe3\x81\x8a\xe3\x81\x93\xe3\x81\x9d\xe3\x81\xa8\xe3\x81\xae\xe3\x81\xbb\xe3\x82\x82\xe3\x82\x88\xe3\x82\x8d\xe3\x82\x92"
                 "\xe3\x81\x94\xe3\x81\x9e\xe3\x81\xa9\xe3\x81\xbc\xe3\x81\xbd\xe3\x81\x89\xe3\x82\x87", "o");                         // おこそとのほもよろを ごぞどぼぽ ぉょ
            add ("\xe3\x82\x93", "n");                                                                                                  // ん
            return t;
        }();

        const auto it = table.find (toHiragana (c));
        return it != table.end() ? juce::String (it->second) : juce::String();
    }
}

juce::String vowelOf (const juce::String& lyric)
{
    const auto text = lyric.trim();

    if (text.isEmpty())
        return {};

    // Romaji: the last letter, if it is a vowel or n.
    if (text.containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
    {
        const auto last = juce::CharacterFunctions::toLowerCase (text.getLastCharacter());

        if (juce::String ("aeioun").containsChar (last))
            return juce::String::charToString (last);

        return {};
    }

    // Kana: the last character that has a vowel of its own. ー and っ only
    // lengthen or stop what came before, so they are stepped over.
    const juce::juce_wchar longMark = 0x30fc, smallTsu = 0x3063;

    for (int i = text.length(); --i >= 0;)
    {
        const auto c = toHiragana (text[i]);

        if (c == longMark || c == smallTsu)
            continue;

        return vowelOfKana (c);
    }

    return {};
}

juce::String romajiToKana (const juce::String& romajiIn)
{
    struct Syllable { const char* romaji; const char* kana; };

    // Longest first, so "kya" is tried before "ky" can go wrong. Generated:
    // the five vowels on every consonant row, the y-combinations, and the
    // spellings a romaji typist reaches for (shi, chi, tsu, fu, ji, ...).
    static const Syllable table[] =
    {
            { "bya", "\xe3\x81\xb3\xe3\x82\x83" },
            { "bye", "\xe3\x81\xb3\xe3\x81\x87" },
            { "byo", "\xe3\x81\xb3\xe3\x82\x87" },
            { "byu", "\xe3\x81\xb3\xe3\x82\x85" },
            { "cha", "\xe3\x81\xa1\xe3\x82\x83" },
            { "che", "\xe3\x81\xa1\xe3\x81\x87" },
            { "chi", "\xe3\x81\xa1" },
            { "cho", "\xe3\x81\xa1\xe3\x82\x87" },
            { "chu", "\xe3\x81\xa1\xe3\x82\x85" },
            { "cya", "\xe3\x81\xa1\xe3\x82\x83" },
            { "cyo", "\xe3\x81\xa1\xe3\x82\x87" },
            { "cyu", "\xe3\x81\xa1\xe3\x82\x85" },
            { "dhi", "\xe3\x81\xa7\xe3\x81\x83" },
            { "dwu", "\xe3\x81\xa9\xe3\x81\x85" },
            { "dya", "\xe3\x81\xa2\xe3\x82\x83" },
            { "dyo", "\xe3\x81\xa2\xe3\x82\x87" },
            { "dyu", "\xe3\x81\xa2\xe3\x82\x85" },
            { "gya", "\xe3\x81\x8e\xe3\x82\x83" },
            { "gye", "\xe3\x81\x8e\xe3\x81\x87" },
            { "gyo", "\xe3\x81\x8e\xe3\x82\x87" },
            { "gyu", "\xe3\x81\x8e\xe3\x82\x85" },
            { "hya", "\xe3\x81\xb2\xe3\x82\x83" },
            { "hye", "\xe3\x81\xb2\xe3\x81\x87" },
            { "hyo", "\xe3\x81\xb2\xe3\x82\x87" },
            { "hyu", "\xe3\x81\xb2\xe3\x82\x85" },
            { "jya", "\xe3\x81\x98\xe3\x82\x83" },
            { "jyo", "\xe3\x81\x98\xe3\x82\x87" },
            { "jyu", "\xe3\x81\x98\xe3\x82\x85" },
            { "kya", "\xe3\x81\x8d\xe3\x82\x83" },
            { "kye", "\xe3\x81\x8d\xe3\x81\x87" },
            { "kyo", "\xe3\x81\x8d\xe3\x82\x87" },
            { "kyu", "\xe3\x81\x8d\xe3\x82\x85" },
            { "mya", "\xe3\x81\xbf\xe3\x82\x83" },
            { "mye", "\xe3\x81\xbf\xe3\x81\x87" },
            { "myo", "\xe3\x81\xbf\xe3\x82\x87" },
            { "myu", "\xe3\x81\xbf\xe3\x82\x85" },
            { "nya", "\xe3\x81\xab\xe3\x82\x83" },
            { "nye", "\xe3\x81\xab\xe3\x81\x87" },
            { "nyo", "\xe3\x81\xab\xe3\x82\x87" },
            { "nyu", "\xe3\x81\xab\xe3\x82\x85" },
            { "pya", "\xe3\x81\xb4\xe3\x82\x83" },
            { "pye", "\xe3\x81\xb4\xe3\x81\x87" },
            { "pyo", "\xe3\x81\xb4\xe3\x82\x87" },
            { "pyu", "\xe3\x81\xb4\xe3\x82\x85" },
            { "rya", "\xe3\x82\x8a\xe3\x82\x83" },
            { "rye", "\xe3\x82\x8a\xe3\x81\x87" },
            { "ryo", "\xe3\x82\x8a\xe3\x82\x87" },
            { "ryu", "\xe3\x82\x8a\xe3\x82\x85" },
            { "sha", "\xe3\x81\x97\xe3\x82\x83" },
            { "she", "\xe3\x81\x97\xe3\x81\x87" },
            { "shi", "\xe3\x81\x97" },
            { "sho", "\xe3\x81\x97\xe3\x82\x87" },
            { "shu", "\xe3\x81\x97\xe3\x82\x85" },
            { "sya", "\xe3\x81\x97\xe3\x82\x83" },
            { "syo", "\xe3\x81\x97\xe3\x82\x87" },
            { "syu", "\xe3\x81\x97\xe3\x82\x85" },
            { "thi", "\xe3\x81\xa6\xe3\x81\x83" },
            { "tsa", "\xe3\x81\xa4\xe3\x81\x81" },
            { "tse", "\xe3\x81\xa4\xe3\x81\x87" },
            { "tsi", "\xe3\x81\xa4\xe3\x81\x83" },
            { "tso", "\xe3\x81\xa4\xe3\x81\x89" },
            { "tsu", "\xe3\x81\xa4" },
            { "twu", "\xe3\x81\xa8\xe3\x81\x85" },
            { "tya", "\xe3\x81\xa1\xe3\x82\x83" },
            { "tyo", "\xe3\x81\xa1\xe3\x82\x87" },
            { "tyu", "\xe3\x81\xa1\xe3\x82\x85" },
            { "zya", "\xe3\x81\x98\xe3\x82\x83" },
            { "zyo", "\xe3\x81\x98\xe3\x82\x87" },
            { "zyu", "\xe3\x81\x98\xe3\x82\x85" },
            { "ba", "\xe3\x81\xb0" },
            { "be", "\xe3\x81\xb9" },
            { "bi", "\xe3\x81\xb3" },
            { "bo", "\xe3\x81\xbc" },
            { "bu", "\xe3\x81\xb6" },
            { "da", "\xe3\x81\xa0" },
            { "de", "\xe3\x81\xa7" },
            { "di", "\xe3\x81\xa2" },
            { "do", "\xe3\x81\xa9" },
            { "du", "\xe3\x81\xa5" },
            { "fa", "\xe3\x81\xb5\xe3\x81\x81" },
            { "fe", "\xe3\x81\xb5\xe3\x81\x87" },
            { "fi", "\xe3\x81\xb5\xe3\x81\x83" },
            { "fo", "\xe3\x81\xb5\xe3\x81\x89" },
            { "fu", "\xe3\x81\xb5" },
            { "ga", "\xe3\x81\x8c" },
            { "ge", "\xe3\x81\x92" },
            { "gi", "\xe3\x81\x8e" },
            { "go", "\xe3\x81\x94" },
            { "gu", "\xe3\x81\x90" },
            { "ha", "\xe3\x81\xaf" },
            { "he", "\xe3\x81\xb8" },
            { "hi", "\xe3\x81\xb2" },
            { "ho", "\xe3\x81\xbb" },
            { "hu", "\xe3\x81\xb5" },
            { "ja", "\xe3\x81\x98\xe3\x82\x83" },
            { "je", "\xe3\x81\x98\xe3\x81\x87" },
            { "ji", "\xe3\x81\x98" },
            { "jo", "\xe3\x81\x98\xe3\x82\x87" },
            { "ju", "\xe3\x81\x98\xe3\x82\x85" },
            { "ka", "\xe3\x81\x8b" },
            { "ke", "\xe3\x81\x91" },
            { "ki", "\xe3\x81\x8d" },
            { "ko", "\xe3\x81\x93" },
            { "ku", "\xe3\x81\x8f" },
            { "ma", "\xe3\x81\xbe" },
            { "me", "\xe3\x82\x81" },
            { "mi", "\xe3\x81\xbf" },
            { "mo", "\xe3\x82\x82" },
            { "mu", "\xe3\x82\x80" },
            { "na", "\xe3\x81\xaa" },
            { "ne", "\xe3\x81\xad" },
            { "ni", "\xe3\x81\xab" },
            { "no", "\xe3\x81\xae" },
            { "nu", "\xe3\x81\xac" },
            { "pa", "\xe3\x81\xb1" },
            { "pe", "\xe3\x81\xba" },
            { "pi", "\xe3\x81\xb4" },
            { "po", "\xe3\x81\xbd" },
            { "pu", "\xe3\x81\xb7" },
            { "ra", "\xe3\x82\x89" },
            { "re", "\xe3\x82\x8c" },
            { "ri", "\xe3\x82\x8a" },
            { "ro", "\xe3\x82\x8d" },
            { "ru", "\xe3\x82\x8b" },
            { "sa", "\xe3\x81\x95" },
            { "se", "\xe3\x81\x9b" },
            { "si", "\xe3\x81\x97" },
            { "so", "\xe3\x81\x9d" },
            { "su", "\xe3\x81\x99" },
            { "ta", "\xe3\x81\x9f" },
            { "te", "\xe3\x81\xa6" },
            { "ti", "\xe3\x81\xa1" },
            { "to", "\xe3\x81\xa8" },
            { "tu", "\xe3\x81\xa4" },
            { "va", "\xe3\x82\x94\xe3\x81\x81" },
            { "ve", "\xe3\x82\x94\xe3\x81\x87" },
            { "vi", "\xe3\x82\x94\xe3\x81\x83" },
            { "vo", "\xe3\x82\x94\xe3\x81\x89" },
            { "vu", "\xe3\x82\x94" },
            { "wa", "\xe3\x82\x8f" },
            { "we", "\xe3\x81\x86\xe3\x81\x87" },
            { "wi", "\xe3\x81\x86\xe3\x81\x83" },
            { "wo", "\xe3\x82\x92" },
            { "ya", "\xe3\x82\x84" },
            { "ye", "\xe3\x81\x84\xe3\x81\x87" },
            { "yo", "\xe3\x82\x88" },
            { "yu", "\xe3\x82\x86" },
            { "za", "\xe3\x81\x96" },
            { "ze", "\xe3\x81\x9c" },
            { "zi", "\xe3\x81\x98" },
            { "zo", "\xe3\x81\x9e" },
            { "zu", "\xe3\x81\x9a" },
            { "a", "\xe3\x81\x82" },
            { "e", "\xe3\x81\x88" },
            { "i", "\xe3\x81\x84" },
            { "o", "\xe3\x81\x8a" },
            { "u", "\xe3\x81\x86" },
    };

    const auto text = romajiIn.trim().toLowerCase();

    if (text.isEmpty() || ! text.containsOnly ("abcdefghijklmnopqrstuvwxyz'-"))
        return {};

    juce::String kana;
    int i = 0;

    while (i < text.length())
    {
        const auto rest = text.substring (i);
        bool matched = false;

        for (const auto& syllable : table)
            if (rest.startsWith (syllable.romaji))
            {
                kana << juce::String (juce::CharPointer_UTF8 (syllable.kana));
                i += (int) std::strlen (syllable.romaji);
                matched = true;
                break;
            }

        if (matched)
            continue;

        const auto c = rest[0];
        const auto next = rest.length() > 1 ? rest[1] : 0;

        if (c == 'n')                                   // ん: nn, n', or n before a consonant or the end
        {
            kana << juce::String (juce::CharPointer_UTF8 ("\xe3\x82\x93"));
            i += (next == 'n' || next == '\'') ? 2 : 1;
        }
        else if (c == next && c != 'a' && c != 'i' && c != 'u' && c != 'e' && c != 'o')
        {
            kana << juce::String (juce::CharPointer_UTF8 ("\xe3\x81\xa3"));   // っ: a doubled consonant
            ++i;
        }
        else if (c == '-')
        {
            kana << juce::String (juce::CharPointer_UTF8 ("\xe3\x83\xbc"));   // ー
            ++i;
        }
        else
        {
            return {};   // not romaji after all
        }
    }

    return kana;
}

int midiNoteFromName (const juce::String& nameIn)
{
    const auto noteName = nameIn.trim().toUpperCase();

    if (noteName.length() < 2)
        return -1;

    static const int semitones[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    const auto letter = noteName[0];

    if (letter < 'A' || letter > 'G')
        return -1;

    int index = 1;
    int semitone = semitones[letter - 'A'];

    if (noteName[index] == '#')
    {
        ++semitone;
        ++index;
    }

    const auto octaveText = noteName.substring (index);

    if (octaveText.isEmpty() || ! octaveText.containsOnly ("-0123456789"))
        return -1;

    const auto note = (octaveText.getIntValue() + 1) * 12 + semitone;
    return note >= 0 && note <= 127 ? note : -1;
}

//==============================================================================
// The voicebank

juce::String Voicebank::readName (const juce::File& folder)
{
    const auto character = readVoicebankText (folder.getChildFile ("character.txt"));

    for (const auto& line : juce::StringArray::fromLines (character))
        if (line.trim().startsWithIgnoreCase ("name="))
            if (auto value = line.fromFirstOccurrenceOf ("=", false, false).trim(); value.isNotEmpty())
                return value;

    return folder.getFileName();
}

void Voicebank::addOto (const juce::File& otoFile)
{
    const auto directory = otoFile.getParentDirectory();
    signature << otoFile.getFullPathName() << ":" << otoFile.getLastModificationTime().toMilliseconds() << ";";

    for (const auto& rawLine : juce::StringArray::fromLines (readVoicebankText (otoFile)))
    {
        const auto line = rawLine.trim();
        const auto equals = line.indexOfChar ('=');

        if (equals <= 0)
            continue;

        const auto wavName = line.substring (0, equals).replaceCharacter ('\\', '/');
        auto fields = juce::StringArray::fromTokens (line.substring (equals + 1), ",", "");

        while (fields.size() < 6)
            fields.add ({});

        OtoEntry entry;
        entry.file = directory.getChildFile (wavName);
        entry.alias = fields[0].trim();

        // An empty alias means the file's own name, which is how a plain CV
        // bank names "あ.wav" as あ.
        if (entry.alias.isEmpty())
            entry.alias = entry.file.getFileNameWithoutExtension();

        entry.offsetMs = fields[1].getDoubleValue();
        entry.consonantMs = fields[2].getDoubleValue();
        entry.cutoffMs = fields[3].getDoubleValue();
        entry.preutterMs = fields[4].getDoubleValue();
        entry.overlapMs = fields[5].getDoubleValue();

        // The first of a repeated alias wins, as in UTAU.
        entries.emplace (entry.alias, entry);
    }
}

void Voicebank::readPrefixMap (const juce::File& file)
{
    for (const auto& line : juce::StringArray::fromLines (readVoicebankText (file)))
    {
        const auto parts = juce::StringArray::fromTokens (line, "\t", "");

        if (parts.size() < 2)
            continue;

        if (const auto note = midiNoteFromName (parts[0]); note >= 0)
            prefixMap[(size_t) note] = { parts[1], parts.size() > 2 ? parts[2] : juce::String() };
    }
}

std::shared_ptr<const Voicebank> Voicebank::load (const juce::File& folderToLoad, juce::String& error)
{
    if (! folderToLoad.isDirectory())
    {
        error = "Voicebank folder not found: " + folderToLoad.getFullPathName();
        return {};
    }

    auto bank = std::make_shared<Voicebank>();
    bank->folder = folderToLoad;
    bank->name = readName (folderToLoad);

    // The folder's own oto.ini first, then its subfolders' in name order, and
    // theirs: a singer's folder holds banks, and a bank's folder can hold
    // pitch subfolders.
    juce::Array<juce::File> otoFiles;

    if (auto own = folderToLoad.getChildFile ("oto.ini"); own.existsAsFile())
        otoFiles.add (own);

    auto subfolders = folderToLoad.findChildFiles (juce::File::findDirectories, false);
    subfolders.sort();

    for (const auto& sub : subfolders)
    {
        if (auto oto = sub.getChildFile ("oto.ini"); oto.existsAsFile())
            otoFiles.add (oto);

        auto inner = sub.findChildFiles (juce::File::findDirectories, false);
        inner.sort();

        for (const auto& deeper : inner)
            if (auto oto = deeper.getChildFile ("oto.ini"); oto.existsAsFile())
                otoFiles.add (oto);
    }

    for (const auto& oto : otoFiles)
        bank->addOto (oto);

    if (auto map = folderToLoad.getChildFile ("prefix.map"); map.existsAsFile())
        bank->readPrefixMap (map);

    if (bank->entries.empty())
    {
        error = "No oto.ini with any entries in " + folderToLoad.getFileName();
        return {};
    }

    error = {};
    return bank;
}

std::shared_ptr<const Voicebank> Voicebank::loadShared (const juce::File& folderToLoad, juce::String& error)
{
    static std::mutex mutex;
    static std::map<juce::String, std::weak_ptr<const Voicebank>> cache;

    const std::lock_guard<std::mutex> lock (mutex);
    auto loaded = load (folderToLoad, error);

    if (loaded == nullptr)
        return {};

    // Parsed either way -- the parse is what says whether the files changed,
    // and it is quick next to synthesising a single note -- but handed out
    // shared when it matches, so every generator singing from this bank holds
    // the same one.
    const auto key = folderToLoad.getFullPathName();

    if (auto it = cache.find (key); it != cache.end())
        if (auto existing = it->second.lock(); existing != nullptr && existing->signature == loaded->signature)
            return existing;

    cache[key] = loaded;
    return loaded;
}

const OtoEntry* Voicebank::find (const juce::String& alias) const
{
    const auto it = entries.find (alias);
    return it != entries.end() ? &it->second : nullptr;
}

const OtoEntry* Voicebank::resolve (const juce::String& lyricIn, const juce::String& previousLyric, int midiNote) const
{
    const auto lyric = lyricIn.trim();

    if (lyric.isEmpty())
        return nullptr;

    if (auto* entry = resolveAsWritten (lyric, previousLyric, midiNote))
        return entry;

    // Typed in romaji with nothing recorded under that spelling: the kana it
    // spells, which is what a Japanese bank names its recordings.
    if (const auto kana = romajiToKana (lyric); kana.isNotEmpty() && kana != lyric)
        return resolveAsWritten (kana, previousLyric, midiNote);

    return nullptr;
}

const OtoEntry* Voicebank::resolveAsWritten (const juce::String& lyric, const juce::String& previousLyric, int midiNote) const
{
    const auto& [prefix, suffix] = prefixMap[(size_t) juce::jlimit (0, 127, midiNote)];

    auto lookup = [&] (const juce::String& alias) -> const OtoEntry*
    {
        if (prefix.isNotEmpty() || suffix.isNotEmpty())
            if (auto* entry = find (prefix + alias + suffix))
                return entry;

        return find (alias);
    };

    if (lyric.containsChar (' '))
        if (auto* entry = lookup (lyric))
            return entry;

    if (previousLyric.isNotEmpty())
    {
        if (const auto vowel = vowelOf (previousLyric); vowel.isNotEmpty())
            if (auto* entry = lookup (vowel + " " + lyric))
                return entry;
    }
    else if (auto* entry = lookup ("- " + lyric))
    {
        return entry;
    }

    for (const auto& alias : { lyric, "- " + lyric, "* " + lyric })
        if (auto* entry = lookup (alias))
            return entry;

    return nullptr;
}

const OtoEntry* Voicebank::getPreviewEntry() const
{
    if (auto* entry = resolve (juce::String (juce::CharPointer_UTF8 ("\xe3\x81\x82")), {}, 60))   // あ
        return entry;

    return entries.empty() ? nullptr : &entries.begin()->second;
}

} // namespace carve::voice
