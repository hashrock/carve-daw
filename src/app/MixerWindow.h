#pragma once

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "MixerComponent.h"
#include "ShortcutHelpBar.h"

namespace carve::app
{

// Floating mixer, in the same spirit as the Pattern Editor window: the main
// window keeps the playlist, everything else is a popup.
//
// The width is not the user's to pick: it is exactly as wide as the channel
// strips need, so the window re-fits itself whenever a generator appears or
// disappears. Position and height are remembered between sessions.
class MixerWindow : public juce::DocumentWindow
{
public:
    MixerWindow (te::Edit& edit, juce::UndoManager& um,
                 std::function<void()> onCloseCallback,
                 std::function<bool (const juce::KeyPress&)> keyHandler)
        : juce::DocumentWindow ("Mixer",
                                juce::Colour (0xff232327),
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          onKey (std::move (keyHandler)),
          settings (createSettingsFile (edit.engine)),
          mixer (edit, um)
    {
        viewport.setViewedComponent (&mixer, false);
        viewport.setScrollBarsShown (false, true);

        // The slot gestures are mouse-only and invisible until you try them,
        // which is exactly what the help bar is for. They are always available,
        // so unlike the pattern editor's this list never changes.
        helpBar.setEntries ({ { "+ FX", "add effect" },
                              { "click", "open FX editor" },
                              { "drag", "reorder FX" },
                              { "dot", "bypass" },
                              { "right-click", "FX menu" } });

        content.setSize (mixer.getWidth(), defaultHeight + ShortcutHelpBar::preferredHeight);

        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);

        mixer.onContentWidthChanged = [this] { fitToContent(); };
        fitToContent();
        restoreBounds();

        setVisible (true);
        toFront (true);
    }

    void setSong (model::Song song)
    {
        mixer.setSong (std::move (song));
    }

    void closeButtonPressed() override
    {
        if (onClose)
            onClose();   // owner destroys this window (deferred)
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (onKey != nullptr && onKey (key))
            return true;
        return juce::DocumentWindow::keyPressed (key);
    }

    void moved() override
    {
        juce::DocumentWindow::moved();
        storeBounds();
    }

    void resized() override
    {
        juce::DocumentWindow::resized();

        // strips fill the window height; width stays content-driven so the
        // viewport scrolls horizontally when there are many generators
        mixer.setSize (mixer.getWidth(), viewport.getMaximumVisibleHeight());

        storeBounds();
    }

private:
    static constexpr int minWidth = 280;
    static constexpr int minHeight = 300;
    static constexpr int defaultHeight = 340;
    static constexpr const char* boundsKey = "mixerWindowBounds";

    // Window geometry is UI state, not part of the song, and tracktion's
    // PropertyStorage only takes keys from its own fixed SettingID enum. A
    // small PropertiesFile in the engine's own prefs folder keeps it in the
    // single settings location the app already has (see EngineSetup.h).
    static std::unique_ptr<juce::PropertiesFile> createSettingsFile (te::Engine& engine)
    {
        juce::PropertiesFile::Options options;
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        options.millisecondsBeforeSaving = 500;   // a drag writes on every frame

        return std::make_unique<juce::PropertiesFile> (
            engine.getPropertyStorage().getAppPrefsFolder().getChildFile ("windows.settings"),
            options);
    }

    juce::Rectangle<int> getDisplayArea() const
    {
        if (auto* display = juce::Desktop::getInstance().getDisplays()
                                .getDisplayForRect (getScreenBounds()))
            return display->userArea;

        return { 1280, 800 };
    }

    // Sizes the window to the strips it actually has, up to what the display
    // can show — beyond that the viewport scrolls instead of the window growing
    // off the screen.
    void fitToContent()
    {
        const auto border = getContentComponentBorder();
        const auto screen = getDisplayArea();

        const auto width = juce::jlimit (minWidth,
                                         juce::jmax (minWidth, screen.getWidth()),
                                         mixer.getWidth() + border.getLeftAndRight());

        // Pinning both width limits keeps a resize drag from fighting the next
        // re-fit; the height stays free.
        const auto smallest = minHeight + ShortcutHelpBar::preferredHeight;

        setResizeLimits (width, smallest + border.getTopAndBottom(),
                         width, juce::jmax (smallest, screen.getHeight()));
    }

    void restoreBounds()
    {
        const auto saved = juce::Rectangle<int>::fromString (settings->getValue (boundsKey));

        if (saved.isEmpty())
        {
            centreWithSize (getWidth(), getHeight());
            return;
        }

        // Only the position and the height were the user's choice; the width
        // has just been recomputed from the strips.
        setBoundsConstrained ({ saved.getX(), saved.getY(), getWidth(), saved.getHeight() });
        ensureOnScreen();
    }

    // The display a position was saved on may be gone (or smaller) by the next
    // run. If none of the title bar is reachable any more, start over centred.
    void ensureOnScreen()
    {
        const auto titleStrip = getScreenBounds().withHeight (24);

        for (const auto& display : juce::Desktop::getInstance().getDisplays().displays)
            if (display.userArea.getIntersection (titleStrip).getWidth() >= 80)
                return;

        centreWithSize (getWidth(), getHeight());
    }

    void storeBounds()
    {
        // cheap: PropertiesFile keeps the value in memory and writes the file
        // on a timer (and once more when it is destroyed)
        if (settings != nullptr)
            settings->setValue (boundsKey, getScreenBounds().toString());
    }

    std::function<void()> onClose;
    std::function<bool (const juce::KeyPress&)> onKey;
    std::unique_ptr<juce::PropertiesFile> settings;   // flushes itself on destruction
    juce::Viewport viewport;
    ShortcutHelpBar helpBar;
    MixerComponent mixer;

    // The strips scroll, the help bar does not, so they need a parent between
    // them and the window.
    struct Content : public juce::Component
    {
        Content (juce::Viewport& v, ShortcutHelpBar& b) : viewport (v), helpBar (b)
        {
            addAndMakeVisible (viewport);
            addAndMakeVisible (helpBar);
        }

        void resized() override
        {
            auto area = getLocalBounds();
            helpBar.setBounds (area.removeFromBottom (ShortcutHelpBar::preferredHeight));
            viewport.setBounds (area);
        }

        juce::Viewport& viewport;
        ShortcutHelpBar& helpBar;
    };

    Content content { viewport, helpBar };
};

} // namespace carve::app
