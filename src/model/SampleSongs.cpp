#include "SampleSongs.h"
#include "DemoSong.h"

namespace carve::model
{

namespace
{
    // The 808's General MIDI notes, named so a pattern below reads as drums
    // rather than as numbers (see DrumSynthPlugin.h for the whole kit).
    constexpr int kick = 36, rim = 37, snare = 38, clap = 39,
                  closedHat = 42, openHat = 46, lowTom = 41, highTom = 48;

    struct Hit { double start; int note; int velocity; };
    struct Tone { double start, length; int pitch; int velocity; };

    void addHits (Pattern pattern, std::initializer_list<Hit> hits)
    {
        // A drum note has no length worth setting: the 808's voices run to
        // their own decay and ignore the note off.
        for (const auto& hit : hits)
            pattern.addNote (hit.start, 0.25, hit.note, hit.velocity, nullptr);
    }

    void addTones (Pattern pattern, std::initializer_list<Tone> tones)
    {
        for (const auto& tone : tones)
            pattern.addNote (tone.start, tone.length, tone.pitch, tone.velocity, nullptr);
    }

    // A chord as one call: the same start, length and velocity for each note
    // of it, which is what every pad and stab in here wants.
    void addChord (Pattern pattern, double start, double length,
                   std::initializer_list<int> pitches, int velocity)
    {
        for (int pitch : pitches)
            pattern.addNote (start, length, pitch, velocity, nullptr);
    }

    void place (Playlist playlist, const Generator& generator, const Pattern& pattern,
                std::initializer_list<double> starts)
    {
        for (double start : starts)
            playlist.addClip (generator, pattern, start, nullptr);
    }

    //==========================================================================
    // Eight bars of four-to-the-floor: the arrangement most people would build
    // first, laid out so that muting a row is enough to hear what it does.
    Song buildNeonStreets()
    {
        auto song = Song::create ("Neon Streets");
        song.setTempo (124.0, nullptr);
        auto playlist = song.getPlaylist();

        {
            auto drums = song.addGenerator ("Drums", Generator::drumSynthType, nullptr);
            drums.setVolumeDb (-10.0f, nullptr);

            auto beat = drums.addPattern ("Four Floor", 4.0, nullptr);
            addHits (beat, { { 0.0, kick, 118 }, { 1.0, kick, 112 },
                             { 2.0, kick, 118 }, { 3.0, kick, 112 },
                             { 1.0, clap, 100 }, { 3.0, clap, 100 },
                             { 0.5, closedHat, 62 }, { 1.5, closedHat, 74 },
                             { 2.5, closedHat, 62 }, { 3.5, closedHat, 74 } });

            // The same bar with the hat left open on the last off-beat, which
            // is all it takes to hear a bar line without a fill.
            auto lift = drums.addPattern ("Four Floor Lift", 4.0, nullptr);
            addHits (lift, { { 0.0, kick, 118 }, { 1.0, kick, 112 },
                             { 2.0, kick, 118 }, { 3.0, kick, 112 },
                             { 1.0, clap, 100 }, { 3.0, clap, 100 },
                             { 0.5, closedHat, 62 }, { 1.5, closedHat, 74 },
                             { 2.5, closedHat, 62 }, { 3.5, openHat, 86 } });

            place (playlist, drums, beat, { 0.0, 4.0, 8.0, 16.0, 20.0, 24.0 });
            place (playlist, drums, lift, { 12.0, 28.0 });
        }

        {
            auto bass = song.addGenerator ("Bass", Generator::synthType, nullptr);
            bass.setVolumeDb (-13.0f, nullptr);

            auto line = bass.addPattern ("Bassline", 8.0, nullptr);
            addTones (line, { { 0.0, 0.9, 45, 112 }, { 1.5, 0.4, 45, 88 },
                              { 2.0, 0.9, 45, 104 }, { 3.5, 0.4, 52, 92 },
                              { 4.0, 0.9, 41, 112 }, { 5.5, 0.4, 41, 88 },
                              { 6.0, 0.9, 43, 104 }, { 7.5, 0.4, 48, 92 } });

            place (playlist, bass, line, { 0.0, 8.0, 16.0, 24.0 });
        }

        {
            auto chords = song.addGenerator ("Chords", Generator::synthType, nullptr);
            chords.setVolumeDb (-18.0f, nullptr);
            chords.setPan (-0.2f, nullptr);

            // Am - F - C - G, a bar each.
            auto pad = chords.addPattern ("Am F C G", 16.0, nullptr);
            addChord (pad,  0.0, 3.8, { 57, 60, 64 }, 72);
            addChord (pad,  4.0, 3.8, { 53, 57, 60 }, 72);
            addChord (pad,  8.0, 3.8, { 52, 55, 60 }, 72);
            addChord (pad, 12.0, 3.8, { 50, 55, 59 }, 72);

            place (playlist, chords, pad, { 0.0, 16.0 });
        }

        {
            auto lead = song.addGenerator ("Lead", Generator::synthType, nullptr);
            lead.setVolumeDb (-16.0f, nullptr);
            lead.setPan (0.2f, nullptr);

            // Enters halfway, which is the whole reason the arrangement is
            // eight bars rather than four.
            auto hook = lead.addPattern ("Hook", 16.0, nullptr);
            addTones (hook, { {  0.0, 0.9, 76, 100 }, {  1.0, 0.45, 74, 92 },
                              {  1.5, 0.45, 72, 92 }, {  2.0, 1.8,  69, 104 },
                              {  4.0, 0.9, 72, 100 }, {  5.0, 0.45, 74, 92 },
                              {  5.5, 0.45, 76, 92 }, {  6.0, 1.8,  72, 104 },
                              {  8.0, 0.9, 77, 100 }, {  9.0, 0.45, 76, 92 },
                              {  9.5, 0.45, 74, 92 }, { 10.0, 1.8,  72, 104 },
                              { 12.0, 0.9, 74, 100 }, { 13.0, 0.45, 72, 92 },
                              { 13.5, 0.45, 71, 92 }, { 14.0, 1.8,  69, 104 } });

            place (playlist, lead, hook, { 16.0 });
        }

        return song;
    }

    //==========================================================================
    // Slower, sparser, and in seventh chords: the same three rows doing an
    // entirely different job, mostly to show that they can.
    Song buildBoomBap()
    {
        auto song = Song::create ("Boom Bap");
        song.setTempo (88.0, nullptr);
        auto playlist = song.getPlaylist();

        {
            auto drums = song.addGenerator ("Drums", Generator::drumSynthType, nullptr);
            drums.setVolumeDb (-13.0f, nullptr);

            auto beat = drums.addPattern ("Boom Bap", 8.0, nullptr);
            addHits (beat, { { 0.0, kick, 120 }, { 2.5, kick, 104 }, { 3.0, kick, 96 },
                             { 1.0, snare, 112 }, { 3.0, snare, 112 },
                             { 4.0, kick, 120 }, { 5.5, kick, 100 }, { 6.5, kick, 104 },
                             { 5.0, snare, 112 }, { 7.0, snare, 112 },
                             { 0.0, closedHat, 70 }, { 0.5, closedHat, 54 },
                             { 1.0, closedHat, 70 }, { 1.5, closedHat, 54 },
                             { 2.0, closedHat, 70 }, { 2.5, closedHat, 54 },
                             { 3.0, closedHat, 70 }, { 3.5, closedHat, 54 },
                             { 4.0, closedHat, 70 }, { 4.5, closedHat, 54 },
                             { 5.0, closedHat, 70 }, { 5.5, closedHat, 54 },
                             { 6.0, closedHat, 70 }, { 6.5, closedHat, 54 },
                             { 7.0, closedHat, 70 }, { 7.5, closedHat, 54 } });

            // Two toms and a rim in place of the last snare: a fill written
            // out rather than left to the imagination.
            auto fill = drums.addPattern ("Fill", 8.0, nullptr);
            addHits (fill, { { 0.0, kick, 120 }, { 2.5, kick, 104 },
                             { 1.0, snare, 112 }, { 3.0, snare, 112 },
                             { 4.0, kick, 120 }, { 5.0, snare, 112 },
                             { 6.0, highTom, 104 }, { 6.5, highTom, 96 },
                             { 7.0, lowTom, 108 }, { 7.5, rim, 96 },
                             { 0.0, closedHat, 70 }, { 1.0, closedHat, 70 },
                             { 2.0, closedHat, 70 }, { 3.0, closedHat, 70 },
                             { 4.0, closedHat, 70 }, { 5.0, closedHat, 70 } });

            place (playlist, drums, beat, { 0.0, 8.0, 16.0 });
            place (playlist, drums, fill, { 24.0 });
        }

        {
            auto bass = song.addGenerator ("Bass", Generator::synthType, nullptr);
            bass.setVolumeDb (-16.0f, nullptr);

            auto line = bass.addPattern ("Upright", 16.0, nullptr);
            addTones (line, { {  0.0, 1.8, 38, 116 }, {  2.5, 0.4, 38, 84 },
                              {  4.0, 1.8, 43, 112 }, {  6.5, 0.4, 45, 84 },
                              {  8.0, 1.8, 36, 116 }, { 10.5, 0.4, 36, 84 },
                              { 12.0, 1.8, 41, 112 }, { 14.5, 0.4, 40, 88 } });

            place (playlist, bass, line, { 0.0, 16.0 });
        }

        {
            auto keys = song.addGenerator ("Keys", Generator::synthType, nullptr);
            keys.setVolumeDb (-22.0f, nullptr);
            keys.setPan (0.15f, nullptr);

            // Dm7 - Gm7 - Cmaj7 - Fmaj7, off the beat, the way a sampled
            // loop of a piano tends to sit.
            auto stabs = keys.addPattern ("Rhodes", 16.0, nullptr);
            addChord (stabs,  0.5, 1.4, { 50, 57, 60, 65 }, 78);
            addChord (stabs,  4.5, 1.4, { 55, 58, 62, 65 }, 78);
            addChord (stabs,  8.5, 1.4, { 48, 55, 59, 64 }, 74);
            addChord (stabs, 12.5, 1.4, { 53, 57, 60, 64 }, 74);

            place (playlist, keys, stabs, { 0.0, 16.0 });
        }

        return song;
    }

    //==========================================================================
    // No drums at all, long notes, and one row moving: what the app sounds
    // like when nothing is in a hurry.
    Song buildSlowBloom()
    {
        auto song = Song::create ("Slow Bloom");
        song.setTempo (72.0, nullptr);
        auto playlist = song.getPlaylist();

        {
            auto pad = song.addGenerator ("Pad", Generator::synthType, nullptr);
            pad.setVolumeDb (-3.0f, nullptr);

            auto chords = pad.addPattern ("Bloom", 16.0, nullptr);
            addChord (chords,  0.0, 7.6, { 48, 55, 59, 64 }, 68);   // Cmaj7
            addChord (chords,  8.0, 7.6, { 45, 52, 57, 64 }, 68);   // Am9-ish

            auto answer = pad.addPattern ("Bloom Answer", 16.0, nullptr);
            addChord (answer,  0.0, 7.6, { 41, 53, 57, 60 }, 66);   // Fmaj7
            addChord (answer,  8.0, 7.6, { 43, 50, 55, 62 }, 66);   // G add9

            place (playlist, pad, chords, { 0.0, 32.0 });
            place (playlist, pad, answer, { 16.0, 48.0 });
        }

        {
            auto arp = song.addGenerator ("Arp", Generator::synthType, nullptr);
            arp.setVolumeDb (-8.0f, nullptr);
            arp.setPan (-0.25f, nullptr);

            auto figure = arp.addPattern ("Figure", 8.0, nullptr);
            const int pitches[] = { 72, 76, 79, 83, 79, 76, 72, 76,
                                    72, 79, 76, 83, 79, 83, 76, 79 };

            for (int i = 0; i < 16; ++i)
                figure.addNote (i * 0.5, 0.45, pitches[i], 62 + (i % 4) * 6, nullptr);

            // In from the second phrase, out again before the last, so the
            // arrangement has somewhere to go without another instrument.
            place (playlist, arp, figure, { 16.0, 24.0, 32.0, 40.0 });
        }

        {
            auto bass = song.addGenerator ("Sub", Generator::synthType, nullptr);
            bass.setVolumeDb (-1.0f, nullptr);

            auto roots = bass.addPattern ("Roots", 16.0, nullptr);
            addTones (roots, { { 0.0, 7.6, 36, 92 }, { 8.0, 7.6, 33, 92 } });

            auto roots2 = bass.addPattern ("Roots Answer", 16.0, nullptr);
            addTones (roots2, { { 0.0, 7.6, 29, 92 }, { 8.0, 7.6, 31, 92 } });

            place (playlist, bass, roots, { 0.0, 32.0 });
            place (playlist, bass, roots2, { 16.0, 48.0 });
        }

        return song;
    }

    struct Entry
    {
        const char* name;
        Song (*build)();
    };

    const Entry entries[] = {
        { "Neon Streets", buildNeonStreets },
        { "Boom Bap",     buildBoomBap },
        { "Slow Bloom",   buildSlowBloom },
    };
} // namespace

juce::StringArray sampleSongNames()
{
    juce::StringArray names;

    for (const auto& entry : entries)
        names.add (entry.name);

    return names;
}

Song buildSampleSong (int index)
{
    if (index < 0 || index >= (int) std::size (entries))
        return buildDemoSong();

    return entries[index].build();
}

} // namespace carve::model
