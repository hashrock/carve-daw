#include "EffectSlotList.h"
#include "Fonts.h"

#include "../plugins/DelayPlugin.h"
#include "../plugins/LimiterPlugin.h"
#include "../plugins/OvertopPlugin.h"
#include "../plugins/DistortionPlugin.h"
#include "../plugins/SaturationPlugin.h"

namespace carve::app
{

namespace
{
    // The internal plugins worth offering as inserts -- tracktion's and our
    // own -- in the order they appear in the menu. `type` is the plugin's
    // xmlTypeName, which is what the model stores and EditSync creates from.
    //
    // Our own are named through the class rather than spelled out: the
    // distortion went missing from this menu for a while because nothing
    // tied the two spellings together.
    struct InternalEffect
    {
        const char* type;
        const char* menuName;
        const char* shortName;   // what fits on a 100px slot row
    };

    const InternalEffect internalEffects[] =
    {
        { "compressor",   "Compressor",           "Comp" },
        { plugins::LimiterPlugin::xmlTypeName, "Limiter", "Limit" },
        { plugins::OvertopPlugin::xmlTypeName, "Overtop (multiband)", "Overtop" },
        { "4bandEq",      "4-Band EQ",            "EQ" },
        { "lowpass",      "Low / High Pass",      "Filter" },
        { plugins::DistortionPlugin::xmlTypeName, "Distortion", "Dist" },
        { plugins::SaturationPlugin::xmlTypeName, "Saturation", "Sat" },
        { "reverb",       "Reverb",               "Reverb" },
        { plugins::DelayPlugin::xmlTypeName, "Delay", "Delay" },
        { "chorus",       "Chorus",               "Chorus" },
        { "phaser",       "Phaser",               "Phaser" },
        { "pitchShifter", "Pitch Shifter",        "Pitch" },
    };

    constexpr int numInternalEffects = (int) (sizeof (internalEffects) / sizeof (internalEffects[0]));

    // Menu id ranges. Effect types and scanned plugins share one menu, so they
    // get separate blocks rather than an index the caller has to disambiguate.
    constexpr int internalMenuIdBase = 100;
    constexpr int externalMenuIdBase = 1000;

    // The strip of the row the bypass LED owns; a click left of this toggles
    // rather than opening the editor.
    constexpr int ledColumnWidth = 14;

    juce::Array<juce::PluginDescription> getScannedEffects (te::Engine& engine)
    {
        juce::Array<juce::PluginDescription> result;

        for (const auto& type : engine.getPluginManager().knownPluginList.getTypes())
            if (! type.isInstrument)
                result.add (type);

        return result;
    }

    juce::String getSlotName (const model::Effect& effect)
    {
        if (effect.isExternal())
        {
            if (auto description = effect.getPluginDescription())
                return description->name;

            return "Plugin";
        }

        const auto type = effect.getType();

        for (const auto& internal : internalEffects)
            if (type == internal.type)
                return internal.shortName;

        // Something the menu never offered -- an older song, or a type added
        // later. Its xmlTypeName is at least recognisable.
        return type;
    }
} // namespace

//==============================================================================
EffectChain makeGeneratorEffectChain (model::Generator generatorToShow, juce::UndoManager& undoManager)
{
    EffectChain chain;

    chain.getSlots = [generator = generatorToShow]
    {
        std::vector<EffectSlot> result;

        for (const auto& effect : generator.getEffects())
            result.push_back ({ effect.getId(), getSlotName (effect), effect.isEnabled() });

        return result;
    };

    chain.add = [generator = generatorToShow, &undoManager]
                (const juce::String& type, const juce::PluginDescription* description) mutable
    {
        undoManager.beginNewTransaction();
        generator.addEffect (type, description, &undoManager);
    };

    chain.setEnabled = [generator = generatorToShow, &undoManager]
                       (const juce::String& id, bool enabled)
    {
        if (auto effect = generator.findEffect (id))
        {
            undoManager.beginNewTransaction();
            effect->setEnabled (enabled, &undoManager);
        }
    };

    chain.remove = [generator = generatorToShow, &undoManager] (const juce::String& id) mutable
    {
        if (auto effect = generator.findEffect (id))
        {
            undoManager.beginNewTransaction();
            generator.removeEffect (*effect, &undoManager);
        }
    };

    chain.move = [generator = generatorToShow, &undoManager] (const juce::String& id, int newIndex) mutable
    {
        if (auto effect = generator.findEffect (id))
        {
            undoManager.beginNewTransaction();
            generator.moveEffect (*effect, newIndex, &undoManager);
        }
    };

    return chain;
}

//==============================================================================
// The master and return chains are the generator chain with a different owner:
// same EFFECT nodes, same ids, same undo. Only the node they hang off differs.
template <typename Owner>
static EffectChain makeOwnedEffectChain (Owner ownerToShow, juce::UndoManager& undoManager)
{
    EffectChain chain;

    chain.getSlots = [owner = ownerToShow]
    {
        std::vector<EffectSlot> result;

        for (const auto& effect : owner.getEffects())
            result.push_back ({ effect.getId(), getSlotName (effect), effect.isEnabled() });

        return result;
    };

    chain.add = [owner = ownerToShow, &undoManager] (const juce::String& type,
                                                     const juce::PluginDescription* description) mutable
    {
        undoManager.beginNewTransaction();
        owner.addEffect (type, description, &undoManager);
    };

    chain.setEnabled = [owner = ownerToShow, &undoManager] (const juce::String& id, bool enabled) mutable
    {
        if (auto effect = owner.findEffect (id))
        {
            undoManager.beginNewTransaction();
            effect->setEnabled (enabled, &undoManager);
        }
    };

    chain.remove = [owner = ownerToShow, &undoManager] (const juce::String& id) mutable
    {
        if (auto effect = owner.findEffect (id))
        {
            undoManager.beginNewTransaction();
            owner.removeEffect (*effect, &undoManager);
        }
    };

    chain.move = [owner = ownerToShow, &undoManager] (const juce::String& id, int newIndex) mutable
    {
        if (auto effect = owner.findEffect (id))
        {
            undoManager.beginNewTransaction();
            owner.moveEffect (*effect, newIndex, &undoManager);
        }
    };

    return chain;
}

EffectChain makeMasterEffectChain (model::MasterBus busToShow, juce::UndoManager& undoManager)
{
    return makeOwnedEffectChain (std::move (busToShow), undoManager);
}

EffectChain makeReturnEffectChain (model::Return returnToShow, juce::UndoManager& undoManager)
{
    return makeOwnedEffectChain (std::move (returnToShow), undoManager);
}

//==============================================================================
EffectSlotList::EffectSlotList (EffectChain chainToShow, te::Engine& e)
    : chain (std::move (chainToShow)), engine (e)
{
    slots = chain.getSlots();
}

int EffectSlotList::getPreferredHeight() const
{
    return ((int) slots.size() + 1) * rowHeight;
}

void EffectSlotList::refreshFromChain()
{
    const auto previousHeight = getPreferredHeight();
    slots = chain.getSlots();

    // A drag whose rows moved under it has lost its meaning; drop it rather
    // than committing a move the user can no longer see.
    if (isDragging && getPreferredHeight() != previousHeight)
    {
        isDragging = false;
        pressedRow = -1;
        dropIndex = -1;
    }

    if (getPreferredHeight() != previousHeight && onPreferredHeightChanged != nullptr)
        onPreferredHeightChanged();

    repaint();
}

int EffectSlotList::getRowAt (int y) const
{
    if (y < 0)
        return -1;

    const int row = y / rowHeight;
    return row <= (int) slots.size() ? row : -1;
}

juce::Rectangle<int> EffectSlotList::getRowBounds (int row) const
{
    return { 0, row * rowHeight, getWidth(), rowHeight };
}

void EffectSlotList::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff212125));

    const auto font = uiFont (fonts::small);

    for (size_t i = 0; i < slots.size(); ++i)
    {
        const auto& slot = slots[i];
        const bool enabled = slot.enabled;
        auto row = getRowBounds ((int) i).reduced (0, 1).withTrimmedRight (1);

        g.setColour (juce::Colour ((int) i == hoveredRow ? 0xff3d3d46 : 0xff32323a));
        g.fillRect (row);

        // The dot is the bypass state and its own hit target: filled means the
        // effect is in the signal path.
        auto led = row.removeFromLeft (ledColumnWidth);
        g.setColour (enabled ? juce::Colour (0xff4fc27a) : juce::Colour (0xff55555e));
        const auto dot = led.withSizeKeepingCentre (7, 7).toFloat();

        if (enabled)
            g.fillEllipse (dot);
        else
            g.drawEllipse (dot, 1.0f);

        g.setColour (enabled ? juce::Colour (0xffd8d8dc) : juce::Colour (0xff76767e));
        g.setFont (font);
        g.drawFittedText (slot.name, row.reduced (2, 0),
                          juce::Justification::centredLeft, 1, 0.75f);
    }

    // The add row, drawn as an outline so it reads as an empty slot rather
    // than another effect.
    auto addRow = getRowBounds ((int) slots.size()).reduced (0, 1).withTrimmedRight (1);
    const bool addHovered = hoveredRow == (int) slots.size();

    g.setColour (juce::Colour (addHovered ? 0xff3d3d46 : 0xff2a2a31));
    g.fillRect (addRow);
    g.setColour (juce::Colour (addHovered ? 0xffd8d8dc : 0xff76767e));
    g.setFont (font);
    g.drawText ("+ FX", addRow, juce::Justification::centred);

    if (isDragging && dropIndex >= 0)
    {
        g.setColour (juce::Colour (0xffe0a24f));
        g.fillRect (0, juce::jmin (dropIndex * rowHeight, getHeight() - 2), getWidth(), 2);
    }
}

void EffectSlotList::mouseMove (const juce::MouseEvent& e)
{
    const int row = getRowAt (e.y);

    if (row != hoveredRow)
    {
        hoveredRow = row;
        repaint();
    }
}

void EffectSlotList::mouseExit (const juce::MouseEvent&)
{
    if (hoveredRow != -1)
    {
        hoveredRow = -1;
        repaint();
    }
}

void EffectSlotList::mouseDown (const juce::MouseEvent& e)
{
    pressedRow = -1;
    isDragging = false;
    dropIndex = -1;

    const int row = getRowAt (e.y);

    if (row < 0)
        return;

    if (isAddRow (row))
    {
        showAddMenu();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        showSlotMenu (row);
        return;
    }

    if (e.x < ledColumnWidth)
    {
        toggleBypass (row);
        return;
    }

    // Everything else is either a click (open the editor) or the start of a
    // reorder drag; mouseUp decides which.
    pressedRow = row;
}

void EffectSlotList::mouseDrag (const juce::MouseEvent& e)
{
    if (pressedRow < 0)
        return;

    if (! isDragging && std::abs (e.getDistanceFromDragStartY()) < 4)
        return;

    isDragging = true;

    // Round to the nearest gap between rows, so the marker sits where the slot
    // will end up.
    dropIndex = juce::jlimit (0, (int) slots.size(),
                              (e.y + rowHeight / 2) / rowHeight);
    repaint();
}

void EffectSlotList::mouseUp (const juce::MouseEvent&)
{
    const int row = pressedRow;
    const bool wasDragging = isDragging;
    const int dropAt = dropIndex;

    pressedRow = -1;
    isDragging = false;
    dropIndex = -1;
    repaint();

    if (row < 0 || row >= (int) slots.size())
        return;

    if (! wasDragging)
    {
        if (onOpenEffectEditor != nullptr)
            onOpenEffectEditor (slots[(size_t) row].id);

        return;
    }

    if (dropAt < 0)
        return;

    // dropAt is a gap in the list as it stands; removing the dragged slot
    // first shifts every gap below it up by one.
    const int newIndex = dropAt > row ? dropAt - 1 : dropAt;

    if (newIndex != row)
        moveEffect (row, newIndex);
}

void EffectSlotList::showAddMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("Insert Effect");

    for (int i = 0; i < numInternalEffects; ++i)
        menu.addItem (internalMenuIdBase + i, internalEffects[i].menuName);

    const auto plugins = getScannedEffects (engine);

    if (! plugins.isEmpty())
    {
        juce::PopupMenu pluginMenu;

        for (int i = 0; i < plugins.size(); ++i)
            pluginMenu.addItem (externalMenuIdBase + i,
                                plugins[i].name + "  (" + plugins[i].pluginFormatName + ")");

        menu.addSeparator();
        menu.addSubMenu ("Plugins", pluginMenu);
    }

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (getRowBounds ((int) slots.size()))),
                        [safe = juce::Component::SafePointer (this), plugins] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result >= externalMenuIdBase && result < externalMenuIdBase + plugins.size())
        {
            const auto& description = plugins.getReference (result - externalMenuIdBase);
            safe->chain.add (model::Effect::externalType, &description);
        }
        else if (result >= internalMenuIdBase && result < internalMenuIdBase + numInternalEffects)
        {
            safe->chain.add (internalEffects[result - internalMenuIdBase].type, nullptr);
        }
    });
}

void EffectSlotList::showSlotMenu (int row)
{
    if (row < 0 || row >= (int) slots.size())
        return;

    const auto slot = slots[(size_t) row];

    juce::PopupMenu menu;
    menu.addSectionHeader (slot.name);
    menu.addItem (1, "Open Editor");
    menu.addItem (2, "Bypass", true, ! slot.enabled);
    menu.addSeparator();
    menu.addItem (3, "Move Up", row > 0);
    menu.addItem (4, "Move Down", row < (int) slots.size() - 1);
    menu.addSeparator();
    menu.addItem (5, "Remove");

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (getRowBounds (row))),
                        [safe = juce::Component::SafePointer (this), row] (int result)
    {
        if (safe == nullptr)
            return;

        // The chain may have changed while the menu was open.
        if (row >= (int) safe->slots.size())
            return;

        switch (result)
        {
            case 1:
                if (safe->onOpenEffectEditor != nullptr)
                    safe->onOpenEffectEditor (safe->slots[(size_t) row].id);
                break;
            case 2:  safe->toggleBypass (row); break;
            case 3:  safe->moveEffect (row, row - 1); break;
            case 4:  safe->moveEffect (row, row + 1); break;
            case 5:  safe->removeEffect (row); break;
            default: break;
        }
    });
}

void EffectSlotList::toggleBypass (int row)
{
    if (row < 0 || row >= (int) slots.size())
        return;

    // By value: the edit calls back into refreshFromChain(), which replaces
    // the vector this row came out of.
    const auto slot = slots[(size_t) row];
    chain.setEnabled (slot.id, ! slot.enabled);
}

void EffectSlotList::removeEffect (int row)
{
    if (row < 0 || row >= (int) slots.size())
        return;

    const auto id = slots[(size_t) row].id;

    // The editor window holds the plugin this is about to delete.
    if (onEffectAboutToBeRemoved != nullptr)
        onEffectAboutToBeRemoved (id);

    chain.remove (id);
}

void EffectSlotList::moveEffect (int row, int newIndex)
{
    if (row < 0 || row >= (int) slots.size())
        return;

    chain.move (slots[(size_t) row].id, newIndex);
}

} // namespace carve::app
