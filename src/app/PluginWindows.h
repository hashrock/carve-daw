#pragma once

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace orionish::app
{

// Floating window hosting an external plugin's own editor UI
// (or a generic parameter editor if the plugin has none).
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    PluginEditorWindow (juce::AudioPluginInstance& instance, std::function<void()> onCloseCallback)
        : juce::DocumentWindow (instance.getName(),
                                juce::Colours::darkgrey,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback))
    {
        if (auto* editor = instance.createEditorIfNeeded())
            setContentOwned (editor, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (instance), true);

        setUsingNativeTitleBar (true);
        centreWithSize (juce::jmax (300, getWidth()), juce::jmax (200, getHeight()));
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window
    }

private:
    std::function<void()> onClose;
};

// Plugin scan/management window wrapping juce::PluginListComponent.
// Mutations to the KnownPluginList are persisted automatically by
// tracktion's PluginManager.
class PluginScanWindow : public juce::DocumentWindow
{
public:
    PluginScanWindow (te::Engine& engine, std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("Plugins",
                                juce::Colours::darkgrey,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback))
    {
        auto& pluginManager = engine.getPluginManager();
        auto list = std::make_unique<juce::PluginListComponent> (
            pluginManager.pluginFormatManager,
            pluginManager.knownPluginList,
            engine.getTemporaryFileManager().getTempFile ("PluginScanDeadMansPedal"),
            nullptr);
        list->setSize (640, 420);

        setContentOwned (list.release(), true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();
    }

private:
    std::function<void()> onClose;
};

} // namespace orionish::app
