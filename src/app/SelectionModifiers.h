#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// The modifier keys the two grid editors -- the playlist and the piano roll --
// read the same way, in one place so they cannot drift apart again. Both
// editors hold a selection of items on a grid, and the gestures on that
// selection are meant to be one habit, learned once:
//
//   click item              select it (a click on the selection keeps it)
//   Shift / Cmd + click     add to the selection, or take a selected item out
//   drag item               move the whole selection
//   Cmd + drag item         duplicate the selection and move the copies
//   right-click selection   delete the whole selection
//   Shift held              the select tool, for as long as it is held; from
//                           the draw / paint tool its band may start on an item
//   Cmd + rubber band       add to the selection instead of replacing it
//   right / Alt + drag      erase whatever the pointer sweeps over
//
// The functions say which key means what; the editors say what to do about it.
namespace carve::app::selection
{

// Erasing beats whichever tool is active.
inline bool isEraseGesture (const juce::ModifierKeys& mods)
{
    return mods.isRightButtonDown() || mods.isAltDown();
}

// Extends the selection rather than replacing it, on a click on an item.
inline bool isExtendModifier (const juce::ModifierKeys& mods)
{
    return mods.isCommandDown() || mods.isShiftDown();
}

// Turns a move into a copy. Cmd is the one the help bars name; Ctrl is taken
// too, since it does nothing else on either grid and is the same key on a PC.
inline bool isDuplicateModifier (const juce::ModifierKeys& mods)
{
    return mods.isCommandDown() || mods.isCtrlDown();
}

// Holding Shift is the select tool for as long as it is held, whatever the
// toolbar says. Picking a few items out of a part being drawn is a constant
// interruption otherwise: switch tool, rubber-band, switch back. The playlist
// lets that band start on a clip, since a painted section leaves no empty bar
// to start one from (PlaylistComponent::bandStartsOnClips).
inline bool isSelectToolOverride (const juce::ModifierKeys& mods)
{
    return mods.isShiftDown();
}

// A rubber band that starts from the current selection rather than replacing
// it. Cmd only: Shift is what asked for the select tool in the first place, so
// it cannot also mean "add", and a Shift-band from the draw tool has to be
// able to start a fresh selection.
inline bool isRubberBandExtendModifier (const juce::ModifierKeys& mods)
{
    return mods.isCommandDown();
}

} // namespace carve::app::selection
