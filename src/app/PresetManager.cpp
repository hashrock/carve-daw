#include "PresetManager.h"

#include "sync/EngineIds.h"

namespace carve::app
{

namespace
{
    const juce::Identifier presetTag ("CARVEPRESET");
    const juce::Identifier stateTag ("STATE");

    constexpr int presetFormatVersion = 1;

    // The folder a plugin's presets live in, and the whole of the "never offer
    // a 4OSC preset for a compressor" rule -- an editor only ever lists its own
    // directory.
    //
    // For an external plugin that is the format and the name rather than
    // PluginDescription::createIdentifierString(): that carries the version and
    // the unique id, so updating a plugin would orphan every preset saved from
    // the build before it.
    juce::String getTypeFolderName (te::Plugin& plugin)
    {
        if (auto* external = dynamic_cast<te::ExternalPlugin*> (&plugin))
            return juce::File::createLegalFileName (external->desc.pluginFormatName
                                                      + "-" + external->desc.name).trim();

        return plugin.getPluginType();
    }

    // What in a plugin's state says where it sits in the Edit rather than how it
    // sounds. None of it may travel in a preset: `id` is the EditItemID, the
    // window properties are where the editor was last opened, `enabled` is the
    // bypass switch the user set on this slot, and carveEffectId is what
    // EditSync reconciles the chain by -- copying that onto another plugin would
    // leave two slots claiming the same effect.
    bool isStructuralProperty (const juce::Identifier& property)
    {
        return property == te::IDs::type
                || property == te::IDs::id
                || property == te::IDs::enabled
                || property == te::IDs::process
                || property == te::IDs::windowX
                || property == te::IDs::windowY
                || property == te::IDs::windowLocked
                || property == te::IDs::quickParamName
                || property == sync::effectIdProperty;
    }

    // Automation belongs to the song, not to the sound: a preset load should
    // leave a curve the user drew exactly where it was.
    bool isStructuralChild (const juce::Identifier& type)
    {
        return type == te::IDs::AUTOMATIONCURVE
                || type == te::IDs::MACROPARAMETERS
                || type == te::IDs::MODIFIERASSIGNMENTS;
    }

    void stripStructuralParts (juce::ValueTree& state)
    {
        for (int i = state.getNumProperties(); --i >= 0;)
            if (const auto name = state.getPropertyName (i); isStructuralProperty (name))
                state.removeProperty (name, nullptr);

        for (int i = state.getNumChildren(); --i >= 0;)
            if (isStructuralChild (state.getChild (i).getType()))
                state.removeChild (i, nullptr);
    }
} // namespace

//==============================================================================
PluginPresets::PluginPresets (te::Plugin& pluginToEdit)
    : plugin (pluginToEdit), engine (pluginToEdit.edit.engine)
{
}

juce::AudioPluginInstance* PluginPresets::getExternalInstance() const
{
    if (auto external = dynamic_cast<te::ExternalPlugin*> (plugin.get()))
        return external->getAudioPluginInstance();

    return nullptr;
}

juce::StringArray PluginPresets::getPluginProgramNames() const
{
    juce::StringArray names;

    if (auto instance = getExternalInstance())
    {
        const int count = instance->getNumPrograms();

        if (count > 1 || (count == 1 && instance->getProgramName (0).trim().isNotEmpty()))
            for (int i = 0; i < count; ++i)
                names.add (instance->getProgramName (i));
    }

    return names;
}

int PluginPresets::getCurrentPluginProgram() const
{
    if (auto instance = getExternalInstance())
        return instance->getCurrentProgram();

    return -1;
}

bool PluginPresets::selectPluginProgram (int index)
{
    auto instance = getExternalInstance();

    if (instance == nullptr || ! juce::isPositiveAndBelow (index, instance->getNumPrograms()))
        return false;

    instance->setCurrentProgram (index);
    return true;
}

juce::File PluginPresets::getFolder() const
{
    auto* p = plugin.get();

    return engine.getPropertyStorage().getAppPrefsFolder()
              .getChildFile ("Presets")
              .getChildFile (p != nullptr ? getTypeFolderName (*p) : juce::String ("Unknown"));
}

juce::File PluginPresets::getFileForName (const juce::String& presetName) const
{
    return getFolder().getChildFile (juce::File::createLegalFileName (presetName.trim())
                                       + fileExtension);
}

juce::StringArray PluginPresets::getPresetNames() const
{
    juce::StringArray names;

    for (const auto& file : getFolder().findChildFiles (juce::File::findFiles, false,
                                                        juce::String ("*") + fileExtension))
        names.add (file.getFileNameWithoutExtension());

    names.sortNatural();
    return names;
}

std::unique_ptr<juce::XmlElement> PluginPresets::capture (const juce::String& presetName) const
{
    auto* p = plugin.get();

    if (p == nullptr)
        return {};

    auto preset = std::make_unique<juce::XmlElement> (presetTag);
    preset->setAttribute ("name", presetName);
    preset->setAttribute ("pluginType", getTypeFolderName (*p));
    preset->setAttribute ("version", presetFormatVersion);

    if (auto* external = dynamic_cast<te::ExternalPlugin*> (p))
    {
        auto* instance = external->getAudioPluginInstance();

        if (instance == nullptr)
            return {};

        juce::MemoryBlock block;
        instance->getStateInformation (block);

        if (block.isEmpty())
            return {};

        preset->createNewChildElement (stateTag)->addTextElement (block.toBase64Encoding());
    }
    else
    {
        // The plugin's ValueTree is its parameter store, so a copy of it with
        // the Edit-specific bits taken out is already the preset. It is not
        // flushed first: EditSync saves the tree into the song the same way,
        // and internal plugins write through to it as parameters move.
        auto stripped = p->state.createCopy();
        stripStructuralParts (stripped);

        if (auto xml = stripped.createXml())
            preset->addChildElement (xml.release());
        else
            return {};
    }

    return preset;
}

bool PluginPresets::apply (const juce::XmlElement& preset)
{
    auto* p = plugin.get();

    if (p == nullptr)
        return false;

    if (auto* external = dynamic_cast<te::ExternalPlugin*> (p))
    {
        auto* stateXml = preset.getChildByName (stateTag);

        if (stateXml == nullptr)
            return false;

        juce::MemoryBlock block;

        if (! block.fromBase64Encoding (stateXml->getAllSubText().trim()) || block.isEmpty())
            return false;

        auto* instance = external->getAudioPluginInstance();

        if (instance == nullptr)
            return false;

        instance->setStateInformation (block.getData(), (int) block.getSize());
        return true;
    }

    auto* pluginXml = preset.getChildByName (te::IDs::PLUGIN.toString());

    if (pluginXml == nullptr)
        return false;

    const auto stored = juce::ValueTree::fromXml (*pluginXml);

    if (! stored.isValid())
        return false;

    // Deliberately not Plugin::restorePluginStateFromValueTree(): the base
    // implementation is a jassertfalse, and several of the internal effects the
    // app offers -- lowpass among them -- never override it. Writing the
    // properties is what the ones that do override it do anyway, because the
    // tree is the store.
    //
    // Also deliberately not undoable, like every other parameter move in the
    // app: EditSync copies the tree into the model when the song is saved.
    auto state = p->state;

    // A CachedValue returns its default while its property is absent, so a
    // plugin's tree only ever holds what has been moved away from the defaults
    // -- and a saved preset is exactly that much. Clearing the settings the
    // preset does not mention is therefore what puts them back to default;
    // without it, loading a second preset would leave the first one's stray
    // values behind.
    for (int i = state.getNumProperties(); --i >= 0;)
        if (const auto name = state.getPropertyName (i);
            ! isStructuralProperty (name) && ! stored.hasProperty (name))
            state.removeProperty (name, nullptr);

    for (int i = 0; i < stored.getNumProperties(); ++i)
        if (const auto name = stored.getPropertyName (i); ! isStructuralProperty (name))
            state.setProperty (name, stored[name], nullptr);

    // 4OSC keeps its modulation matrix in a child tree, and the same rule
    // applies to it: the preset's children replace the plugin's, and one the
    // preset does not carry goes away. Automation curves and the rest are
    // structural, so they stay where they were.
    for (int i = state.getNumChildren(); --i >= 0;)
        if (const auto type = state.getChild (i).getType();
            ! isStructuralChild (type) && ! stored.getChildWithName (type).isValid())
            state.removeChild (i, nullptr);

    for (const auto& child : stored)
    {
        if (isStructuralChild (child.getType()))
            continue;

        state.removeChild (state.getChildWithName (child.getType()), nullptr);
        state.appendChild (child.createCopy(), nullptr);
    }

    // The parameters cache their value alongside the tree, and a property write
    // is not enough to move the cached one.
    for (auto* parameter : p->getAutomatableParameters())
        if (parameter != nullptr)
            parameter->updateFromAttachedValue();

    return true;
}

bool PluginPresets::save (const juce::String& presetName)
{
    const auto name = presetName.trim();

    if (name.isEmpty())
        return false;

    auto preset = capture (name);

    if (preset == nullptr)
        return false;

    auto folder = getFolder();

    if (! folder.createDirectory())
        return false;

    return preset->writeTo (getFileForName (name));
}

bool PluginPresets::load (const juce::String& presetName)
{
    const auto file = getFileForName (presetName);

    if (! file.existsAsFile())
        return false;

    const auto preset = juce::parseXML (file);

    if (preset == nullptr || ! preset->hasTagName (presetTag))
        return false;

    auto* p = plugin.get();

    // Presets are filed by plugin type, so this only trips on a file that was
    // dropped into the wrong folder by hand.
    if (p == nullptr || preset->getStringAttribute ("pluginType") != getTypeFolderName (*p))
        return false;

    return apply (*preset);
}

bool PluginPresets::remove (const juce::String& presetName)
{
    return getFileForName (presetName).deleteFile();
}

//==============================================================================
namespace
{
    // Menu ids. The saved presets get a block of their own so their index is
    // not something the handler has to disambiguate.
    constexpr int saveAsId     = 1;
    constexpr int overwriteId  = 2;
    constexpr int deleteId     = 3;
    constexpr int revealId     = 4;
    constexpr int noPresetsId  = 5;
    constexpr int presetIdBase = 100;

    // The plugin's own programs, after ours. A plugin can carry thousands, so
    // the two ranges are kept well apart, and past this many the list goes
    // into pages so the menu stays a menu rather than a scroll.
    constexpr int programIdBase = 100000;
    constexpr int programsPerPage = 32;
} // namespace

PresetBar::PresetBar (te::Plugin& plugin)
    : presets (plugin)
{
    menuButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff3d3d46));
    menuButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffd8d8dc));
    menuButton.onClick = [this] { showMenu(); };
    addAndMakeVisible (menuButton);

    nameLabel.setFont (juce::FontOptions (12.0f));
    nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9a9aa4));
    addAndMakeVisible (nameLabel);

    setCurrentPreset ({});

    // An external plugin opens on whatever program it is sitting on, so say
    // which rather than "(not saved)" as if nothing were loaded.
    if (const auto program = presets.getCurrentPluginProgram();
        program >= 0 && ! presets.getPluginProgramNames().isEmpty())
        showPluginProgram (program);
}

void PresetBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2a2a31));
    g.setColour (juce::Colour (0xff17171a));
    g.fillRect (0, getHeight() - 1, getWidth(), 1);
}

void PresetBar::resized()
{
    auto area = getLocalBounds().reduced (4, 2);
    menuButton.setBounds (area.removeFromLeft (68));
    nameLabel.setBounds (area.withTrimmedLeft (6));
}

void PresetBar::setCurrentPreset (const juce::String& presetName)
{
    currentPreset = presetName;
    nameLabel.setText (presetName.isEmpty() ? "(not saved)" : presetName,
                       juce::dontSendNotification);
}

void PresetBar::showPluginProgram (int index)
{
    // A plugin program is not one of our presets: nothing to overwrite or
    // delete, so the current preset is cleared and only the name is shown.
    currentPreset.clear();
    nameLabel.setText (presets.getPluginProgramNames()[index], juce::dontSendNotification);
}

void PresetBar::showMenu()
{
    if (! presets.isAlive())
        return;

    const auto names = presets.getPresetNames();

    juce::PopupMenu menu;
    menu.addSectionHeader ("Presets");

    if (names.isEmpty())
        menu.addItem (noPresetsId, "No presets saved yet", false, false);

    for (int i = 0; i < names.size(); ++i)
        menu.addItem (presetIdBase + i, names[i], true, names[i] == currentPreset);

    // Then the plugin's own, when it has any. Ours come first because they are
    // the ones the user made; the plugin's factory list can run to hundreds.
    if (const auto programs = presets.getPluginProgramNames(); ! programs.isEmpty())
    {
        const int current = presets.getCurrentPluginProgram();

        menu.addSeparator();
        menu.addSectionHeader ("Plugin Presets");

        auto addProgram = [&] (juce::PopupMenu& into, int i)
        {
            into.addItem (programIdBase + i, programs[i], true, i == current);
        };

        if (programs.size() <= programsPerPage)
        {
            for (int i = 0; i < programs.size(); ++i)
                addProgram (menu, i);
        }
        else
        {
            for (int first = 0; first < programs.size(); first += programsPerPage)
            {
                const int last = juce::jmin (programs.size(), first + programsPerPage) - 1;
                juce::PopupMenu page;

                for (int i = first; i <= last; ++i)
                    addProgram (page, i);

                menu.addSubMenu (juce::String (first + 1) + " - " + juce::String (last + 1),
                                 page, true, nullptr, current >= first && current <= last);
            }
        }
    }

    menu.addSeparator();
    menu.addItem (saveAsId, "Save Preset As...");
    menu.addItem (overwriteId, "Overwrite \"" + currentPreset + "\"", currentPreset.isNotEmpty());
    menu.addItem (deleteId, "Delete \"" + currentPreset + "\"", currentPreset.isNotEmpty());
    menu.addSeparator();
    menu.addItem (revealId, "Show Presets Folder");

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (menuButton),
                        [safe = juce::Component::SafePointer (this), names] (int result)
    {
        if (safe == nullptr || result == 0 || ! safe->presets.isAlive())
            return;

        switch (result)
        {
            case saveAsId:
                safe->promptForNameAndSave();
                return;

            case overwriteId:
                safe->presets.save (safe->currentPreset);
                return;

            case deleteId:
                safe->confirmAndDelete();
                return;

            case revealId:
            {
                auto folder = safe->presets.getFolder();
                folder.createDirectory();
                folder.revealToUser();
                return;
            }

            default:
                break;
        }

        if (result >= programIdBase)
        {
            if (safe->presets.selectPluginProgram (result - programIdBase))
                safe->showPluginProgram (result - programIdBase);

            return;
        }

        // The rest of the menu is the saved presets themselves.
        if (const int index = result - presetIdBase; juce::isPositiveAndBelow (index, names.size()))
            if (safe->presets.load (names[index]))
                safe->setCurrentPreset (names[index]);
    });
}

void PresetBar::promptForNameAndSave()
{
    // Held by the ModalComponentManager, which runs the callback before it
    // deletes the window -- so reading the editor back out of it here is safe.
    auto* prompt = new juce::AlertWindow ("Save Preset", {}, juce::MessageBoxIconType::NoIcon, this);
    prompt->addTextEditor ("name", currentPreset, "Preset name:");
    prompt->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    prompt->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    prompt->enterModalState (true,
                             juce::ModalCallbackFunction::create (
                                 [safe = juce::Component::SafePointer (this), prompt] (int result)
                                 {
                                     if (result != 1 || safe == nullptr || ! safe->presets.isAlive())
                                         return;

                                     const auto name = prompt->getTextEditorContents ("name").trim();

                                     if (safe->presets.save (name))
                                         safe->setCurrentPreset (name);
                                 }),
                             true);
}

void PresetBar::confirmAndDelete()
{
    const auto name = currentPreset;

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::MessageBoxIconType::QuestionIcon)
                                      .withTitle ("Delete Preset")
                                      .withMessage ("Delete the preset \"" + name + "\"?")
                                      .withButton ("Delete")
                                      .withButton ("Cancel")
                                      .withAssociatedComponent (this),
                                  [safe = juce::Component::SafePointer (this), name] (int result)
    {
        // showAsync reports the index of the button, so 0 is "Delete".
        if (result != 0 || safe == nullptr || ! safe->presets.isAlive())
            return;

        safe->presets.remove (name);
        safe->setCurrentPreset ({});
    });
}

} // namespace carve::app
