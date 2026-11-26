#include "LyricText.h"

#include "Voicebank.h"
#include "VocalScore.h"

namespace carve::voice::lyrics
{

namespace
{
    constexpr juce::juce_wchar longMark = 0x30fc;   // ー

    juce::String u8 (const char* utf8)  { return juce::String (juce::CharPointer_UTF8 (utf8)); }

    bool isKana (juce::juce_wchar c)  { return (c >= 0x3041 && c <= 0x3096) || (c >= 0x30a1 && c <= 0x30fa); }

    juce::juce_wchar toHiragana (juce::juce_wchar c)  { return c >= 0x30a1 && c <= 0x30f6 ? c - 0x60 : c; }

    // ぁぃぅぇぉ ゃゅょ ゎ ゕゖ: part of the kana before them.
    bool isSmallKana (juce::juce_wchar c)
    {
        switch (toHiragana (c))
        {
            case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:
            case 0x3083: case 0x3085: case 0x3087: case 0x308e: case 0x3095: case 0x3096:
                return true;
            default:
                return false;
        }
    }

    // ー, and the wave dashes people type for it.
    bool isLongMark (juce::juce_wchar c)  { return c == longMark || c == 0x301c || c == 0xff5e; }

    bool isSmallTsuOrN (juce::juce_wchar c)
    {
        const auto h = toHiragana (c);
        return h == 0x3063 || h == 0x3093;   // っ ん
    }

    bool isAsciiLetter (juce::juce_wchar c)  { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

    // Full-width letters read as the letters they are.
    juce::juce_wchar narrowed (juce::juce_wchar c)
    {
        if ((c >= 0xff21 && c <= 0xff3a) || (c >= 0xff41 && c <= 0xff5a))
            return c - 0xff21 + 'A';

        return c;
    }

    // Spaces and punctuation, in either width: they part lyrics but sing
    // nothing. A wave dash is not one of them -- it is a long vowel.
    bool isSeparator (juce::juce_wchar c)
    {
        if (juce::CharacterFunctions::isWhitespace (c) || c == 0x3000)
            return true;

        if (c < 0x80)
            return ! isAsciiLetter (c) && ! (c >= '0' && c <= '9');

        if (c >= 0x3001 && c <= 0x303f)
            return c != 0x301c;

        if (c == 0x30fb)   // ・
            return true;

        if (c >= 0xff01 && c <= 0xff65)
            return ! (c >= 0xff10 && c <= 0xff19) && narrowed (c) == c && c != 0xff5e;

        return false;
    }

    // The bracket at `i` and what is in it, which is one note as written; `i`
    // ends past the closing bracket (or at the end, if it was never closed).
    juce::String readBracket (const juce::String& text, int& i)
    {
        const auto close = text.indexOfChar (i + 1, ']');
        const auto end = close < 0 ? text.length() : close;
        const auto inside = text.substring (i + 1, end).trim();
        i = close < 0 ? text.length() : close + 1;
        return inside;
    }

    //==========================================================================
    class KanaCutter
    {
    public:
        explicit KanaCutter (Reading r) : reading (r) {}

        std::vector<juce::String> cut (const juce::String& text)
        {
            for (int i = 0; i < text.length();)
            {
                const auto c = narrowed (text[i]);

                if (c == '[')
                {
                    unit (readBracket (text, i));
                    separated = true;   // nothing joins a bracket: it is what it says
                    continue;
                }

                if (isAsciiLetter (c))
                {
                    romaji (text, i);
                    continue;
                }

                if (isSeparator (c))
                    separated = true;
                else if (isKana (c) || isLongMark (c))
                    kana (c);
                else
                    unit (juce::String::charToString (c));   // a kanji, a digit: it is a note, if one that cannot sing

                ++i;
            }

            return std::move (out);
        }

    private:
        void unit (const juce::String& s)
        {
            out.push_back (s);
            separated = false;
        }

        void attach (const juce::String& s)
        {
            if (separated || out.empty())
                unit (s);
            else
                out.back() += s;
        }

        void kana (juce::juce_wchar c)
        {
            if (isSmallKana (c))
                return attach (juce::String::charToString (c));

            if (isLongMark (c))
            {
                if (reading == Reading::modelled)
                    return attach (juce::String::charToString (longMark));

                // A voicebank holds a vowel by singing it again, joined on.
                const auto vowel = out.empty() ? juce::String() : vowelOf (out.back());
                static const char* const kanaFor[] = { "\xe3\x81\x82", "\xe3\x81\x84", "\xe3\x81\x86",
                                                       "\xe3\x81\x88", "\xe3\x81\x8a", "\xe3\x82\x93" };
                const auto index = juce::String ("aiueon").indexOf (vowel);
                return unit (vowel.length() == 1 && index >= 0 ? u8 (kanaFor[index]) : juce::String::charToString (longMark));
            }

            if (isSmallTsuOrN (c) && reading == Reading::modelled)
                return attach (juce::String::charToString (c));

            unit (juce::String::charToString (c));
        }

        // A run of letters (and the "-" romaji writes ー with) is the kana it
        // spells, cut like kana. One that spells nothing is a note as typed.
        void romaji (const juce::String& text, int& i)
        {
            juce::String run;

            while (i < text.length())
            {
                const auto c = narrowed (text[i]);

                if (! isAsciiLetter (c) && ! (c == '-' && run.isNotEmpty()))
                    break;

                run += juce::String::charToString (juce::CharacterFunctions::toLowerCase (c));
                ++i;
            }

            const auto spelled = romajiToKana (run);

            if (spelled.isEmpty() || spelled.containsAnyOf ("abcdefghijklmnopqrstuvwxyz-"))
                return unit (run);

            for (auto c : spelled)
                if (isKana (c) || isLongMark (c))
                    kana (c);
        }

        Reading reading;
        std::vector<juce::String> out;
        bool separated = true;
    };

    //==========================================================================
    void cutEnglishWord (juce::String token, std::vector<juce::String>& out)
    {
        if (token.containsOnly ("+"))
        {
            for (int k = 0; k < token.length(); ++k)
                out.push_back ("+");

            return;
        }

        auto keeps = [] (juce::juce_wchar c) { return juce::CharacterFunctions::isLetterOrDigit (c) || c == '\''; };

        while (token.isNotEmpty() && ! keeps (token[0]))
            token = token.substring (1);

        while (token.isNotEmpty() && ! keeps (token.getLastCharacter()))
            token = token.dropLastCharacters (1);

        if (token.isEmpty())
            return;

        size_t syllables = 1;
        juce::String word;

        if (token.containsChar ('-'))
        {
            const auto pieces = juce::StringArray::fromTokens (token, "-", "");
            syllables = 0;

            for (const auto& piece : pieces)
                if (piece.isNotEmpty())
                {
                    word += piece;
                    ++syllables;
                }
        }
        else
        {
            word = token;
            syllables = juce::jmax ((size_t) 1, vocal::parseEnglish (word.toStdString()).size());
        }

        out.push_back (word);

        for (size_t k = 1; k < syllables; ++k)
            out.push_back ("+");
    }

    std::vector<juce::String> cutEnglish (const juce::String& text)
    {
        std::vector<juce::String> out;
        juce::String token;

        auto flush = [&]
        {
            cutEnglishWord (token, out);
            token = {};
        };

        for (int i = 0; i < text.length();)
        {
            const auto c = text[i];

            if (c == '[')
            {
                flush();
                out.push_back (readBracket (text, i));
                continue;
            }

            if (juce::CharacterFunctions::isWhitespace (c) || c == 0x3000)
                flush();
            else
                token += juce::String::charToString (c);

            ++i;
        }

        flush();
        return out;
    }

    juce::String shown (const juce::String& lyric, Reading reading)
    {
        const auto trimmed = lyric.trim();

        if (trimmed.isNotEmpty() || reading == Reading::english)
            return trimmed;

        return u8 ("\xe3\x81\x82");   // あ, model::Note::getDefaultLyric()
    }

    juce::String bracketed (const juce::String& lyric)  { return "[" + lyric + "]"; }

    // Everything in brackets: safe whatever the lyrics are, if not pretty.
    juce::String joinBracketed (const std::vector<Sung>& notes, Reading reading)
    {
        juce::String text;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            if (i > 0)
                text += notes[i].startsPhrase ? "\n" : " ";

            text += bracketed (shown (notes[i].lyric, reading));
        }

        return text;
    }

    juce::String joinKana (const std::vector<Sung>& notes, Reading reading)
    {
        juce::String text, previous;
        juce::String previousUnit;

        for (size_t i = 0; i < notes.size(); ++i)
        {
            const auto lyric = shown (notes[i].lyric, reading);
            const juce::String breath = i > 0 && notes[i].startsPhrase ? "\n" : "";

            // Run on when it reads back as itself, beside what is before it;
            // a space apart when it would have joined that; else in brackets.
            const juce::String candidates[] = { breath + lyric, (i > 0 ? " " : "") + lyric, breath + bracketed (lyric) };
            juce::String chosen = candidates[2];

            for (const auto& candidate : candidates)
            {
                const auto read = split (previous + candidate, reading);
                const auto wanted = i == 0 ? std::vector<juce::String> { lyric }
                                           : std::vector<juce::String> { previousUnit, lyric };

                if (read == wanted)
                {
                    chosen = candidate;
                    break;
                }
            }

            text += chosen;
            previous = chosen.trimStart();
            previousUnit = lyric;
        }

        return text;
    }

    juce::String joinEnglish (const std::vector<Sung>& notes)
    {
        juce::String text;

        auto add = [&text] (const juce::String& token, bool breath)
        {
            if (text.isNotEmpty())
                text += breath ? "\n" : " ";

            text += token;
        };

        for (size_t i = 0; i < notes.size();)
        {
            const auto lyric = notes[i].lyric.trim();
            const auto breath = notes[i].startsPhrase;

            if (lyric == "+")
            {
                add ("+", breath);
                ++i;
                continue;
            }

            size_t pluses = 0;

            while (i + 1 + pluses < notes.size() && notes[i + 1 + pluses].lyric.trim() == "+")
                ++pluses;

            // The word plain when it reads back as itself and the "+" notes
            // after it, or fewer of them (the rest are written out).
            const auto read = split (lyric, Reading::english);
            const auto carried = read.empty() ? 0 : read.size() - 1;
            bool plain = ! read.empty() && read[0] == lyric && carried <= pluses;

            for (size_t k = 1; plain && k < read.size(); ++k)
                plain = read[k] == "+";

            add (plain ? lyric : bracketed (lyric), breath);
            const auto used = plain ? carried : 0;

            for (size_t k = used; k < pluses; ++k)
                add ("+", notes[i + 1 + k].startsPhrase);

            i += 1 + pluses;
        }

        return text;
    }
}

std::vector<juce::String> split (const juce::String& text, Reading reading)
{
    if (reading == Reading::english)
        return cutEnglish (text);

    return KanaCutter (reading).cut (text);
}

juce::String join (const std::vector<Sung>& notes, Reading reading)
{
    const auto text = reading == Reading::english ? joinEnglish (notes) : joinKana (notes, reading);

    std::vector<juce::String> wanted;

    for (const auto& note : notes)
        wanted.push_back (shown (note.lyric, reading));

    // Each note was checked beside the one before; this checks the whole, and
    // falls back to brackets throughout should some longer run read otherwise.
    return split (text, reading) == wanted ? text : joinBracketed (notes, reading);
}

} // namespace carve::voice::lyrics
