#include "DemoSong.h"

namespace carve::model
{

Song buildDemoSong()
{
    auto song = Song::create ("Carve Demo");
    song.setTempo (120.0, nullptr);
    auto playlist = song.getPlaylist();

    // Bass: 4-beat pattern, played through the whole song (beats 0..16)
    {
        auto bass = song.addGenerator ("Bass", "internal-synth", nullptr);
        auto pattern = bass.addPattern ("Bassline", 4.0, nullptr);
        pattern.addNote (0.0, 0.9, 45, 110, nullptr);   // A1
        pattern.addNote (1.0, 0.9, 45, 90, nullptr);
        pattern.addNote (2.0, 0.9, 52, 100, nullptr);   // E2
        pattern.addNote (3.0, 0.45, 53, 95, nullptr);   // F2
        pattern.addNote (3.5, 0.45, 55, 95, nullptr);   // G2
        for (double start : { 0.0, 4.0, 8.0, 12.0 })
            playlist.addClip (bass, pattern, start, nullptr);
    }

    // Chords: 8-beat pattern (Am -> F), beats 0..16
    {
        auto chords = song.addGenerator ("Chords", "internal-synth", nullptr);
        auto pattern = chords.addPattern ("Am F", 8.0, nullptr);
        for (int pitch : { 57, 60, 64 })                // A3 C4 E4
            pattern.addNote (0.0, 3.8, pitch, 70, nullptr);
        for (int pitch : { 53, 57, 60 })                // F3 A3 C4
            pattern.addNote (4.0, 3.8, pitch, 70, nullptr);
        for (double start : { 0.0, 8.0 })
            playlist.addClip (chords, pattern, start, nullptr);
    }

    // Lead: 8-beat melody, enters at beat 8
    {
        auto lead = song.addGenerator ("Lead", "internal-synth", nullptr);
        auto pattern = lead.addPattern ("Melody", 8.0, nullptr);
        const struct { double start, length; int pitch; } notes[] = {
            { 0.0, 0.45, 69 }, { 0.5, 0.45, 72 }, { 1.0, 0.9,  76 },
            { 2.0, 0.45, 74 }, { 2.5, 0.45, 72 }, { 3.0, 0.9,  69 },
            { 4.0, 0.45, 65 }, { 4.5, 0.45, 69 }, { 5.0, 0.9,  72 },
            { 6.0, 1.8,  71 },
        };
        for (const auto& n : notes)
            pattern.addNote (n.start, n.length, n.pitch, 100, nullptr);
        playlist.addClip (lead, pattern, 8.0, nullptr);
    }

    return song;
}

} // namespace carve::model
