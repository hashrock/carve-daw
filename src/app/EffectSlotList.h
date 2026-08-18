#pragma once

#include <functional>
#include <optional>
#include <vector>

#include <tracktion_engine/tracktion_engine.h>

#include "model/SongModel.h"

namespace te = tracktion;

namespace orionish::app
{

// The insert-effect slots of one mixer channel strip: the generator's chain in
// signal order, plus a row at the bottom that adds to the end of it.
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
    EffectSlotList (model::Generator generatorToShow, te::Engine&, juce::UndoManager&);

    static constexpr int rowHeight = 15;

    // Height at which every row, including the add row, is visible.
    int getPreferredHeight() const;

    // Re-reads the chain from the model. Safe to call from a ValueTree
    // callback: it only touches the cached vector.
    void updateFromModel();

    // The user clicked a slot and wants its editor. Handled by the mixer: it
    // is the only thing that knows which track this generator maps to, and so
    // which live plugin carries the effect's id.
    std::function<void (const model::Effect&)> onOpenEffectEditor;

    // The user is about to remove an effect, so anything holding on to its live
    // plugin (an open editor window) should let go first.
    std::function<void (const model::Effect&)> onEffectAboutToBeRemoved;

    // The row count changed, so the strip should re-run its layout.
    std::function<void()> onPreferredHeightChanged;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    // Row indices run 0..effects.size(); the last one is the add row.
    int getRowAt (int y) const;
    bool isAddRow (int row) const  { return row == (int) effects.size(); }
    juce::Rectangle<int> getRowBounds (int row) const;

    void showAddMenu();
    void showSlotMenu (int row);
    void addEffect (const juce::String& type, const juce::PluginDescription*);
    void toggleBypass (int row);
    void removeEffect (int row);
    void moveEffect (int row, int newIndex);

    juce::String getSlotName (const model::Effect&) const;

    model::Generator generator;
    te::Engine& engine;
    juce::UndoManager& undoManager;

    std::vector<model::Effect> effects;

    int hoveredRow = -1;
    int pressedRow = -1;
    bool isDragging = false;
    int dropIndex = -1;   // gap index 0..effects.size(), -1 when not dragging

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EffectSlotList)
};

} // namespace orionish::app
