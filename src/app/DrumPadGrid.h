#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"
#include "plugins/NoteMonitorPlugin.h"

namespace carve::app
{

// Where a drum kit's pads sit on the keyboard. General MIDI puts the standard
// kit at 35..81 with the kick on 36, so starting there means a pattern drawn
// on these pads reads the same in any other DAW, and the piano roll shows the
// kit inside one comfortable region instead of around middle C.
//
// Pad 0 is the bottom-left one and the notes ascend left to right, row by row
// upwards -- the MPC layout, and the one that matches the piano roll, where
// higher notes are drawn higher up.
namespace drumkit
{
    constexpr int numColumns = 4;
    constexpr int numRows = 4;
    constexpr int numPads = numColumns * numRows;
    constexpr int firstNote = 36;                       // GM kick

    // How hard a pad clicked for preview is struck. Drum samples are recorded
    // hot, so this is a firm hit rather than a full-scale one.
    constexpr int previewVelocity = 100;

    inline int getNoteForPad (int pad)  { return firstNote + pad; }

    // The pad a note lands on, or nothing when the note is outside the grid --
    // patterns may well hold notes the pads can't reach.
    std::optional<int> getPadForNote (int midiNote);

    // A pad owns the sound mapped to exactly its own note. A drum-kit sound is
    // always keyed that narrowly, so anything wider (a sampler generator's
    // whole-keyboard sound, say) belongs to no pad and is left alone.
    std::optional<model::SamplerSound> findSoundForPad (const model::Generator&, int pad);

    // The name shown on a pad's note badge, e.g. "C1" for 36.
    juce::String getNoteName (int midiNote);
} // namespace drumkit

// The 4x4 pad grid a drum-kit generator gets in place of a plain sampler's
// single sample. It holds no model state: the generator window pushes a state
// per pad on every refresh, so the grid never has to look at the tree or
// worry about it changing under it.
//
// The one live thing it does look at is the kit's note monitor, polled on a
// timer to light a pad while its sound plays -- from the pattern and from a
// click alike, since both reach the sampler as MIDI and the monitor sits in
// front of it. A pad lights on the hit and stays lit while the note is held
// and the sound has not run out, with a short minimum so a hit shorter than
// a timer tick is still seen.
class DrumPadGrid final : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          private juce::Timer
{
public:
    DrumPadGrid();
    ~DrumPadGrid() override;

    // The pad wants a sample, or the replace/clear menu: an empty pad clicked
    // with either button, or a filled one right-clicked. The panel knows which
    // and decides.
    std::function<void (int pad)> onPadClicked;

    // A filled pad was left-clicked: play it, so a kit can be auditioned pad by
    // pad without drawing a pattern first.
    std::function<void (int pad)> onPadTriggered;

    // Files were dropped starting at this pad. The whole list goes through so
    // the panel can spread a multi-file drop across consecutive pads, which is
    // how a folder of one-shots becomes a kit in one gesture.
    std::function<void (int startPad, const juce::StringArray& files)> onFilesDropped;

    // Whether a dragged file is something to accept, asked of the panel so
    // both agree on what counts as audio.
    std::function<bool (const juce::StringArray&)> isInterestedInFiles;

    struct PadInfo
    {
        juce::String sampleName;    // empty = no sample on this pad

        // The pattern being edited plays this pad: marked, so the kit and the
        // piano roll line up without opening the roll.
        bool usedByPattern = false;

        // How long a hit on this pad sounds, in seconds, or 0 when unknown.
        // Bounds how long the pad stays lit: a held note outlasting a short
        // hit is silence, and the pad should say so.
        double soundSeconds = 0.0;

        // The sound plays through regardless of note-off, so the pad stays
        // lit for the sound's length rather than for the note's.
        bool oneShot = false;
    };

    void setPadState (int pad, const PadInfo&);
    void clearPads();

    // Where the pads' note-ons are seen: the monitor EditSync keeps in front
    // of the kit's sampler. Null stops the lights. Polled, and held weakly --
    // the plugin can go with its track while the grid is up.
    void setActivitySource (plugins::NoteMonitorPlugin*);

    // Height needed for four rows of pads; the pads take whatever width there is.
    static int getPreferredHeight();

    // In this component's coordinates, so the panel can hang a menu off a pad.
    juce::Rectangle<int> getPadBounds (int pad) const;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    struct PadState
    {
        PadInfo info;

        // The monitor's hit count as last seen, and when it last moved: a
        // count that changed between two ticks is a hit, however short.
        std::uint32_t seenHits = 0;
        double lastHitMs = 0.0;   // 0 = never, on the hi-res clock
        bool lit = false;
    };

    std::optional<int> getPadAt (juce::Point<int>) const;
    void setDragTarget (std::optional<int> pad);
    void timerCallback() override;

    std::array<PadState, (size_t) drumkit::numPads> pads;
    std::optional<int> dragTargetPad;
    te::SafeSelectable<plugins::NoteMonitorPlugin> monitor;
};

} // namespace carve::app
