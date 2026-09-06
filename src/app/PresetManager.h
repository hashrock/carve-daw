#pragma once

#include <functional>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::app
{

// Saving and reloading one plugin's settings.
//
// tracktion has no preset framework, so this is the app's own. It leans on the
// two state formats the song already uses: an internal plugin's settings *are*
// its ValueTree, and an external plugin's are the base64 of
// getStateInformation. A preset is that same content in a file of its own, so
// nothing new has to be understood to save one, and loading a preset lands the
// plugin in exactly the state loading a song would have.
//
// Files live under the app's settings folder:
//
//   <appPrefs>/Presets/<pluginType>/<name>.carvepreset
//
//   <appPrefs>/Presets/4osc/Bright Pad.carvepreset
//   <appPrefs>/Presets/compressor/Drum Bus.carvepreset
//   <appPrefs>/Presets/VST3-Diva/Warm Keys.carvepreset
//
// and look like this -- internal on the left, external on the right:
//
//   <CARVEPRESET name="Bright Pad"       <CARVEPRESET name="Warm Keys"
//                pluginType="4osc"                    pluginType="VST3-Diva"
//                version="1">                         version="1">
//     <PLUGIN ampAttack="0.1" .../>        <STATE>PGJhc2U2NCB...</STATE>
//   </CARVEPRESET>                       </CARVEPRESET>
//
// The per-type folder is the whole of the "don't offer a 4OSC preset for a
// compressor" rule: a plugin only ever lists the directory that is its own.
class PluginPresets
{
public:
    // Takes the live plugin, internal or external -- the same object either
    // editor window is already showing.
    explicit PluginPresets (te::Plugin&);

    static constexpr const char* fileExtension = ".carvepreset";

    // False once EditSync has deleted the plugin under the open window. Every
    // operation below is a no-op after that.
    bool isAlive() const                { return plugin != nullptr; }

    // The directory this plugin's presets live in. Not created until a preset
    // is actually saved.
    juce::File getFolder() const;

    // Preset names, sorted, as shown in the menu.
    juce::StringArray getPresetNames() const;

    juce::File getFileForName (const juce::String& presetName) const;

    // Captures the plugin's current settings under this name, overwriting any
    // preset already saved with it. False if the file could not be written.
    bool save (const juce::String& presetName);

    // Applies a saved preset to the live plugin. False if the file is missing,
    // malformed, or was saved from a different kind of plugin.
    bool load (const juce::String& presetName);

    bool remove (const juce::String& presetName);

    // An external plugin's own presets -- the programs it ships with, or that
    // its host-side program list holds -- so they can be picked from here
    // without opening the plugin's UI and hunting for its browser. Empty for
    // an internal plugin, and for an external one that reports none (JUCE
    // gives every instance at least one program; a lone unnamed one is not
    // worth a menu).
    juce::StringArray getPluginProgramNames() const;
    int getCurrentPluginProgram() const;
    bool selectPluginProgram (int index);

private:
    juce::AudioPluginInstance* getExternalInstance() const;

    std::unique_ptr<juce::XmlElement> capture (const juce::String& presetName) const;
    bool apply (const juce::XmlElement& preset);

    te::SafeSelectable<te::Plugin> plugin;
    te::Engine& engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginPresets)
};

//==============================================================================
// The strip that puts the above in front of the user: a menu of saved presets
// plus save/delete, and the name of whatever was loaded last.
//
// It is the same strip in all three editors -- the 4OSC window, the generic
// internal-effect window, and above an external plugin's own UI, where there is
// nowhere else to put it.
class PresetBar : public juce::Component
{
public:
    explicit PresetBar (te::Plugin&);

    static constexpr int height = 24;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void showMenu();
    void promptForNameAndSave();
    void confirmAndDelete();
    void setCurrentPreset (const juce::String& presetName);
    void showPluginProgram (int index);

    PluginPresets presets;
    juce::String currentPreset;

    juce::TextButton menuButton { "Presets" };
    juce::Label nameLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

} // namespace carve::app
