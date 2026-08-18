#include "EffectSlotList.h"

namespace orionish::app
{

namespace
{
    // The tracktion internal plugins worth offering as inserts, in the order
    // they appear in the menu. `type` is the plugin's xmlTypeName, which is
    // what the model stores and EditSync creates from.
    struct InternalEffect
    {
        const char* type;
        const char* menuName;
        const char* shortName;   // what fits on a 100px slot row
    };

    constexpr InternalEffect internalEffects[] =
    {
        { "compressor",   "Compressor / Limiter", "Comp" },
        { "4bandEq",      "4-Band EQ",            "EQ" },
        { "lowpass",      "Low / High Pass",      "Filter" },
        { "reverb",       "Reverb",               "Reverb" },
        { "delay",        "Delay",                "Delay" },
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
} // namespace

//==============================================================================
EffectSlotList::EffectSlotList (model::Generator generatorToShow, te::Engine& e,
                                juce::UndoManager& um)
    : generator (std::move (generatorToShow)), engine (e), undoManager (um)
{
    effects = generator.getEffects();
}

int EffectSlotList::getPreferredHeight() const
{
    return ((int) effects.size() + 1) * rowHeight;
}

void EffectSlotList::updateFromModel()
{
    const auto previousHeight = getPreferredHeight();
    effects = generator.getEffects();

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
    return row <= (int) effects.size() ? row : -1;
}

juce::Rectangle<int> EffectSlotList::getRowBounds (int row) const
{
    return { 0, row * rowHeight, getWidth(), rowHeight };
}

void EffectSlotList::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff212125));

    const auto font = juce::FontOptions (10.0f);

    for (size_t i = 0; i < effects.size(); ++i)
    {
        const auto& effect = effects[i];
        const bool enabled = effect.isEnabled();
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
        g.drawFittedText (getSlotName (effect), row.reduced (2, 0),
                          juce::Justification::centredLeft, 1, 0.75f);
    }

    // The add row, drawn as an outline so it reads as an empty slot rather
    // than another effect.
    auto addRow = getRowBounds ((int) effects.size()).reduced (0, 1).withTrimmedRight (1);
    const bool addHovered = hoveredRow == (int) effects.size();

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
    dropIndex = juce::jlimit (0, (int) effects.size(),
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

    if (row < 0 || row >= (int) effects.size())
        return;

    if (! wasDragging)
    {
        if (onOpenEffectEditor != nullptr)
            onOpenEffectEditor (effects[(size_t) row]);

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
                            .withTargetScreenArea (localAreaToGlobal (getRowBounds ((int) effects.size()))),
                        [safe = juce::Component::SafePointer (this), plugins] (int result)
    {
        if (safe == nullptr || result == 0)
            return;

        if (result >= externalMenuIdBase && result < externalMenuIdBase + plugins.size())
        {
            const auto& description = plugins.getReference (result - externalMenuIdBase);
            safe->addEffect (model::Effect::externalType, &description);
        }
        else if (result >= internalMenuIdBase && result < internalMenuIdBase + numInternalEffects)
        {
            safe->addEffect (internalEffects[result - internalMenuIdBase].type, nullptr);
        }
    });
}

void EffectSlotList::showSlotMenu (int row)
{
    if (row < 0 || row >= (int) effects.size())
        return;

    const auto effect = effects[(size_t) row];

    juce::PopupMenu menu;
    menu.addSectionHeader (getSlotName (effect));
    menu.addItem (1, "Open Editor");
    menu.addItem (2, "Bypass", true, ! effect.isEnabled());
    menu.addSeparator();
    menu.addItem (3, "Move Up", row > 0);
    menu.addItem (4, "Move Down", row < (int) effects.size() - 1);
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
        if (row >= (int) safe->effects.size())
            return;

        switch (result)
        {
            case 1:
                if (safe->onOpenEffectEditor != nullptr)
                    safe->onOpenEffectEditor (safe->effects[(size_t) row]);
                break;
            case 2:  safe->toggleBypass (row); break;
            case 3:  safe->moveEffect (row, row - 1); break;
            case 4:  safe->moveEffect (row, row + 1); break;
            case 5:  safe->removeEffect (row); break;
            default: break;
        }
    });
}

void EffectSlotList::addEffect (const juce::String& type, const juce::PluginDescription* description)
{
    undoManager.beginNewTransaction();
    generator.addEffect (type, description, &undoManager);
}

void EffectSlotList::toggleBypass (int row)
{
    if (row < 0 || row >= (int) effects.size())
        return;

    // By value: the model write calls back into updateFromModel(), which
    // replaces the vector this row came out of.
    auto effect = effects[(size_t) row];

    undoManager.beginNewTransaction();
    effect.setEnabled (! effect.isEnabled(), &undoManager);
}

void EffectSlotList::removeEffect (int row)
{
    if (row < 0 || row >= (int) effects.size())
        return;

    auto effect = effects[(size_t) row];

    // The editor window holds the plugin this is about to delete.
    if (onEffectAboutToBeRemoved != nullptr)
        onEffectAboutToBeRemoved (effect);

    undoManager.beginNewTransaction();
    generator.removeEffect (effect, &undoManager);
}

void EffectSlotList::moveEffect (int row, int newIndex)
{
    if (row < 0 || row >= (int) effects.size())
        return;

    auto effect = effects[(size_t) row];

    undoManager.beginNewTransaction();
    generator.moveEffect (effect, newIndex, &undoManager);
}

juce::String EffectSlotList::getSlotName (const model::Effect& effect) const
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

} // namespace orionish::app
