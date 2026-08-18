#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// One row of a slot list: everything needed to paint it and to name what the
// gestures act on. Flat rather than a model::Effect because not every chain has
// model Effects behind it -- the master bus's inserts live in the Edit.
struct EffectSlot
{
    juce::String id;
    juce::String name;
    bool enabled = true;
};

// Where a slot list's rows come from, and what its gestures do to them.
//
// The generator chains are model-backed and undoable; the master chain is not
// (see makeMasterEffectChain). Indices are never passed across this boundary:
// a model edit calls its listeners synchronously, so by the time an operation
// runs the row it came from may already have moved.
struct EffectChain
{
    std::function<std::vector<EffectSlot>()> getSlots;
    std::function<void (const juce::String& type, const juce::PluginDescription*)> add;
    std::function<void (const juce::String& id, bool enabled)> setEnabled;
    std::function<void (const juce::String& id)> remove;
    std::function<void (const juce::String& id, int newIndex)> move;
};

// One generator's chain, out of the song model, through the UndoManager.
EffectChain makeGeneratorEffectChain (model::Generator, juce::UndoManager&);

// The Edit's master plugin list.
//
// Not model-backed, and so neither undoable nor saved in the .carve: the song
// model has no node for a master chain, and adding one is a change to
// src/model, which this round does not own. See MixerComponent's master strip.
EffectChain makeMasterEffectChain (te::Edit&);

// The insert-effect slots of one mixer channel strip: the chain in signal
// order, plus a row at the bottom that adds to the end of it.
//
// The rows are painted rather than built out of child components. A model edit
// calls its listeners synchronously, so a bypass toggle or a reorder would
// otherwise be deleting the very button the mouse is still inside; with nothing
// but a cached vector to invalidate, that whole class of problem is gone.
//
// Sized to its content -- put it in a Viewport, the chain can outgrow the strip.
class EffectSlotList : public juce::Component
{
public:
    EffectSlotList (EffectChain, te::Engine&);

    static constexpr int rowHeight = 15;

    // Height at which every row, including the add row, is visible.
    int getPreferredHeight() const;

    // Re-reads the chain. Safe to call from a ValueTree callback: it only
    // touches the cached vector.
    void refreshFromChain();

    // The user clicked a slot and wants its editor. Handled by the mixer: it
    // is the only thing that knows which track -- or the master list -- carries
    // the live plugin behind this id.
    std::function<void (const juce::String& effectId)> onOpenEffectEditor;

    // The user is about to remove an effect, so anything holding on to its live
    // plugin (an open editor window) should let go first.
    std::function<void (const juce::String& effectId)> onEffectAboutToBeRemoved;

    // The row count changed, so the strip should re-run its layout.
    std::function<void()> onPreferredHeightChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    // Row indices run 0..slots.size(); the last one is the add row.
    int getRowAt (int y) const;
    bool isAddRow (int row) const  { return row == (int) slots.size(); }
    juce::Rectangle<int> getRowBounds (int row) const;

    void showAddMenu();
    void showSlotMenu (int row);
    void toggleBypass (int row);
    void removeEffect (int row);
    void moveEffect (int row, int newIndex);

    EffectChain chain;
    te::Engine& engine;

    std::vector<EffectSlot> slots;

    int hoveredRow = -1;
    int pressedRow = -1;
    bool isDragging = false;
    int dropIndex = -1;   // gap index 0..slots.size(), -1 when not dragging

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectSlotList)
};

} // namespace carve::app
