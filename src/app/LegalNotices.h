#pragma once

#include <functional>

#include <juce_gui_extra/juce_gui_extra.h>

#include "Fonts.h"

namespace carve::app
{

// The attribution and the third-party notices, in the Apple menu. The AGPL
// build has to offer its legal notices somewhere a person will actually look;
// the Tracktion Engine Personal licence, which the App Store build uses, asks
// for the words "Powered by Tracktion Engine" in that same kind of place.
// One window does both, and the notices file rides inside the bundle so a
// review of the binary can see them without the source tree.

inline juce::File shippedResource (const juce::String& name)
{
    const auto bundled = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                             .getChildFile ("Contents/Resources")
                             .getChildFile (name);

    if (bundled.existsAsFile())
        return bundled;

    const auto beside = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                            .getSiblingFile (name);

    if (beside.existsAsFile())
        return beside;

    return {};
}

inline juce::File thirdPartyNoticesFile()
{
    if (const auto shipped = shippedResource ("THIRD_PARTY_NOTICES.txt"); shipped != juce::File())
        return shipped;

   #ifdef CARVE_SOURCE_NOTICES_FILE
    if (const juce::File inSource (CARVE_SOURCE_NOTICES_FILE); inSource.existsAsFile())
        return inSource;
   #endif

    return {};
}

inline juce::File tracktionLogoFile()
{
    if (const auto shipped = shippedResource ("powered-by-tracktion-engine.png");
        shipped != juce::File())
        return shipped;

   #ifdef CARVE_SOURCE_TRACKTION_LOGO
    if (const juce::File inSource (CARVE_SOURCE_TRACKTION_LOGO); inSource.existsAsFile())
        return inSource;
   #endif

    return {};
}

inline juce::String thirdPartyNoticesText()
{
    if (const auto file = thirdPartyNoticesFile(); file != juce::File())
        return file.loadFileAsString();

    // The copy step did not run and the source path was not compiled in.
    // Still name the pieces a binary of this app is built on, rather than
    // show an empty window.
    return "Third-party notices for Carve DAW\n"
           "\n"
           "The notices file was not found beside the application.\n"
           "\n"
           "Powered by Tracktion Engine\n"
           "https://engine.tracktion.com/agreement\n"
           "\n"
           "Built with JUCE, copyright Raw Material Software Limited.\n"
           "https://juce.com/legal/juce-8-licence/\n"
           "\n"
           "Also: Signalsmith Stretch (MIT, Signalsmith Audio), "
           "TinySoundFont (MIT, Bernhard Schelling) and WORLD "
           "(modified BSD, M. Morise).\n"
           "\n"
           "The full text ships as THIRD_PARTY_NOTICES.txt in the source tree.";
}

// What the AGPL calls the appropriate legal notices, for the build that is
// actually under that licence. The App Store binary is a different licence
// and must not offer to pass that binary on under the AGPL.
inline juce::String aboutBody()
{
   #if CARVE_APP_STORE
    return "This program comes with ABSOLUTELY NO WARRANTY.\n"
           "\n"
           "This Mac App Store copy is not offered under the GNU Affero General "
           "Public License. You receive it under Apple's App Store terms. It is "
           "built with JUCE under the JUCE 8 Starter licence "
           "(https://juce.com/legal/juce-8-licence/) and with Tracktion Engine "
           "under the Tracktion Engine Personal licence "
           "(https://engine.tracktion.com/agreement).\n"
           "\n"
           "JUCE is copyright Raw Material Software Limited.\n"
           "https://juce.com\n"
           "\n"
           "The source code is available separately under the GNU Affero General "
           "Public License, version 3 or later:\n"
           "https://github.com/hashrock/carve-daw\n"
           "https://www.gnu.org/licenses/agpl-3.0.html";
   #else
    return "This program comes with ABSOLUTELY NO WARRANTY.\n"
           "\n"
           "It is free software: you may redistribute it under the terms of the "
           "GNU Affero General Public License, version 3 or later. The full text is "
           "in the LICENSE file beside the source, and at "
           "https://www.gnu.org/licenses/agpl-3.0.html\n"
           "\n"
           "Source: https://github.com/hashrock/carve-daw\n"
           "\n"
           "Built with tracktion_engine under GPL-3.0-or-later, and with JUCE under "
           "AGPL-3.0. JUCE is copyright Raw Material Software Limited.\n"
           "https://juce.com\n"
           "https://juce.com/legal/juce-8-licence/\n"
           "\n"
           "The \"Powered by Tracktion Engine\" line above is attribution. This "
           "build does not use the Tracktion Engine Personal licence.";
   #endif
}

namespace
{
    const juce::Colour noticeBackground { 0xff232327 };
    const juce::Colour noticeText { 0xffd8d8dc };
    const juce::Colour noticeDimText { 0xff9a9aa4 };
    const juce::Colour noticeButton { 0xff3d3d46 };

    void styleNoticeLabel (juce::Label& label, juce::Colour colour, float height,
                           juce::Justification justification = juce::Justification::centredLeft)
    {
        label.setFont (uiFont (height));
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (justification);
    }

    void styleNoticeEditor (juce::TextEditor& editor, juce::Colour background)
    {
        editor.setMultiLine (true);
        editor.setReadOnly (true);
        editor.setScrollbarsShown (true);
        editor.setCaretVisible (false);
        editor.setPopupMenuEnabled (true);
        editor.setFont (juce::Font (uiFont (fonts::normal)));
        editor.setIndents (8, 8);
        editor.setColour (juce::TextEditor::backgroundColourId, background);
        editor.setColour (juce::TextEditor::textColourId, noticeText);
        editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        editor.setColour (juce::TextEditor::highlightColourId, juce::Colour (0xff3d4d5c));
        editor.setColour (juce::TextEditor::highlightedTextColourId, noticeText);
    }
}

class AboutContent : public juce::Component
{
public:
    AboutContent (const juce::String& applicationName,
                  const juce::String& applicationVersion,
                  std::function<void()> onShowNoticesIn)
        : onShowNotices (std::move (onShowNoticesIn))
    {
        styleNoticeLabel (title, noticeText, fonts::title, juce::Justification::centred);
        title.setText (applicationName + " " + applicationVersion, juce::dontSendNotification);

        styleNoticeLabel (copyright, noticeDimText, fonts::normal, juce::Justification::centred);
        copyright.setText ("Copyright (C) 2026 hashrock", juce::dontSendNotification);

        const auto logoFile = tracktionLogoFile();
        const auto image = logoFile != juce::File() ? juce::ImageFileFormat::loadFrom (logoFile)
                                                    : juce::Image();

        // The words are a label either way: the Personal licence asks for the
        // phrase, and the lockup is the picture of it. A missing PNG must not
        // take the phrase with it.
        logo.setImage (image, juce::RectanglePlacement::centred);
        logo.setTitle ("Powered by Tracktion Engine");
        logo.setDescription ("Powered by Tracktion Engine");

        styleNoticeLabel (powered, noticeText, fonts::normal, juce::Justification::centred);
        powered.setText ("Powered by Tracktion Engine", juce::dontSendNotification);

        styleNoticeEditor (body, noticeBackground);
        body.setText (aboutBody(), false);

        noticesButton.setButtonText ("Third-Party Notices...");
        noticesButton.setColour (juce::TextButton::buttonColourId, noticeButton);
        noticesButton.setColour (juce::TextButton::textColourOffId, noticeText);
        noticesButton.onClick = [this]
        {
            if (onShowNotices)
                onShowNotices();
        };

        addAndMakeVisible (title);
        addAndMakeVisible (copyright);
        addAndMakeVisible (logo);
        addAndMakeVisible (powered);
        addAndMakeVisible (body);
        addAndMakeVisible (noticesButton);

        if (! image.isValid())
            logo.setVisible (false);

        setSize (480, 560);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (noticeBackground);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);

        title.setBounds (area.removeFromTop (28));
        copyright.setBounds (area.removeFromTop (22));
        area.removeFromTop (12);

        if (logo.isVisible())
        {
            auto row = area.removeFromTop (78);
            logo.setBounds (row.withSizeKeepingCentre (juce::jmin (row.getWidth(), 280), 72));
            area.removeFromTop (4);
        }

        powered.setBounds (area.removeFromTop (24));
        area.removeFromTop (8);

        noticesButton.setBounds (area.removeFromBottom (30).withSizeKeepingCentre (220, 28));
        area.removeFromBottom (10);
        body.setBounds (area);
    }

private:
    std::function<void()> onShowNotices;
    juce::Label title, copyright, powered;
    juce::ImageComponent logo;
    juce::TextEditor body;
    juce::TextButton noticesButton;
};

class NoticesContent : public juce::Component
{
public:
    NoticesContent()
    {
        styleNoticeEditor (body, juce::Colour (0xff1c1c20));
        body.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a3a40));
        body.setText (thirdPartyNoticesText(), false);
        addAndMakeVisible (body);
        setSize (640, 520);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (noticeBackground);
    }

    void resized() override
    {
        body.setBounds (getLocalBounds().reduced (12));
    }

private:
    juce::TextEditor body;
};

class AboutWindow : public juce::DocumentWindow
{
public:
    AboutWindow (const juce::String& applicationName,
                 const juce::String& applicationVersion,
                 std::function<void()> onShowNotices,
                 std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("About " + applicationName,
                                noticeBackground,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content (applicationName, applicationVersion, std::move (onShowNotices))
    {
        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (420, 480, 900, 1000);
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
    AboutContent content;
};

class NoticesWindow : public juce::DocumentWindow
{
public:
    explicit NoticesWindow (std::function<void()> onCloseCallback)
        : juce::DocumentWindow ("Third-Party Notices",
                                noticeBackground,
                                juce::DocumentWindow::closeButton),
          onClose (std::move (onCloseCallback)),
          content()
    {
        setContentNonOwned (&content, true);
        setUsingNativeTitleBar (true);
        setResizable (true, false);
        setResizeLimits (420, 320, 1100, 1200);
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
    NoticesContent content;
};

} // namespace carve::app
