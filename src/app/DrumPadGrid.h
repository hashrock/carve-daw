#pragma once

#include <array>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/SongModel.h"

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
class DrumPadGrid final : public juce::Component,
                          public juce::FileDragAndDropTarget
{
public:
    DrumPadGrid();

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

    // Empty name = no sample on this pad. `usedByPattern` lights the pads the
    // pattern being edited actually plays, so the kit and the piano roll line
    // up without opening the roll.
    void setPadState (int pad, const juce::String& sampleName, bool usedByPattern);
    void clearPads();

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
        juce::String sampleName;
        bool usedByPattern = false;
    };

    std::optional<int> getPadAt (juce::Point<int>) const;
    void setDragTarget (std::optional<int> pad);

    std::array<PadState, (size_t) drumkit::numPads> pads;
    std::optional<int> dragTargetPad;
};

} // namespace carve::app
