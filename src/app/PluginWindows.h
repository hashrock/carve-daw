#pragma once

#include <functional>
#include <memory>

#include <tracktion_engine/tracktion_engine.h>

#include "PresetManager.h"

namespace te = tracktion;

namespace carve::app
{

// Floating window hosting an external plugin's own editor UI
// (or a generic parameter editor if the plugin has none).
//
// The preset strip goes above the plugin's UI rather than in it: the plugin
// owns every pixel of its own editor, so there is nowhere else to put it.
class PluginEditorWindow : public juce::DocumentWindow
{
public:
    // Preferred: from the tracktion plugin, which knows both its instance and
    // the engine whose settings folder the presets live in. The plugin must
    // have loaded -- there is no editor to show otherwise, which is why both
    // call sites already test getAudioPluginInstance() first.
    PluginEditorWindow (te::ExternalPlugin& plugin, std::function<void()> onCloseCallback)
        : PluginEditorWindow (*plugin.getAudioPluginInstance(), &plugin, std::move (onCloseCallback))
    {
    }

    // Instance only: still opens the editor, but with no preset strip, since
    // a juce::AudioPluginInstance has no route back to the engine.
    PluginEditorWindow (juce::AudioPluginInstance& instance, std::function<void()> onCloseCallback)
        : PluginEditorWindow (instance, nullptr, std::move (onCloseCallback))
    {
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window
    }

private:
    PluginEditorWindow (juce::AudioPluginInstance& instance, te::Plugin* pluginForPresets,
                        std::function<void()> onCloseCallback)
        : juce::DocumentWindow (instance.getName(),
                                juce::Colours::darkgrey,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback))
    {
        setContentOwned (new Content (instance, pluginForPresets), true);
        setUsingNativeTitleBar (true);
        centreWithSize (juce::jmax (300, getWidth()), juce::jmax (200, getHeight()));
        setVisible (true);
        toFront (true);
    }

    // The plugin's editor plus the preset strip. A plugin editor resizes itself
    // -- a preset browser opening inside it, say -- so this follows it and lets
    // the window follow in turn.
    class Content : public juce::Component
    {
    public:
        Content (juce::AudioPluginInstance& instance, te::Plugin* pluginForPresets)
        {
            if (pluginForPresets != nullptr)
            {
                presetBar = std::make_unique<PresetBar> (*pluginForPresets);
                addAndMakeVisible (*presetBar);
            }

            editor.reset (instance.createEditorIfNeeded());

            if (editor == nullptr)
                editor = std::make_unique<juce::GenericAudioProcessorEditor> (instance);

            addAndMakeVisible (*editor);
            setSize (juce::jmax (200, editor->getWidth()), editor->getHeight() + getBarHeight());
        }

        void resized() override
        {
            auto area = getLocalBounds();

            if (presetBar != nullptr)
                presetBar->setBounds (area.removeFromTop (PresetBar::height));

            if (editor != nullptr)
                editor->setBounds (area);
        }

        void childBoundsChanged (juce::Component* child) override
        {
            if (child == editor.get())
                setSize (editor->getWidth(), editor->getHeight() + getBarHeight());
        }

    private:
        int getBarHeight() const  { return presetBar != nullptr ? PresetBar::height : 0; }

        std::unique_ptr<PresetBar> presetBar;
        std::unique_ptr<juce::AudioProcessorEditor> editor;
    };

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

} // namespace carve::app
