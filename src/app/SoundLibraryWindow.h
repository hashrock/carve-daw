#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "SoundLibrary.h"

namespace carve::app
{

// The sound library: what can be downloaded, what is installed, and each
// pack's credits and terms. A list on the left; on the right the selected
// pack, its buttons, and the author's own terms in full, which are on screen
// before the Download button is pressed.
//
//   Download / Update / Retry / Cancel   by state
//   Use                                  a new generator with it, once installed
//   Re-download, Delete                  for an installed copy
class SoundLibraryComponent : public juce::Component,
                              private juce::ListBoxModel,
                              private juce::ChangeListener
{
public:
    SoundLibraryComponent (SoundLibrary&, std::function<void (SoundLibrary::Kind, const juce::File&)> onUse);
    ~SoundLibraryComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Opens on this pack, or on the first one.
    void select (const juce::String& id);

private:
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    void rebuild();
    void updateDetail();
    void loadTerms();
    void primaryClicked();
    void confirmDelete();
    juce::String describeStatus (const SoundLibrary::Pack&, const SoundLibrary::Status&) const;

    SoundLibrary& library;
    std::function<void (SoundLibrary::Kind, const juce::File&)> onUse;

    std::vector<SoundLibrary::Pack> packs;
    juce::String selectedId, termsShownFor;

    juce::Label listStateLabel;
    juce::TextButton refreshButton { "Refresh" };
    juce::ListBox list;

    juce::Label nameLabel, authorLabel, statusLabel, noteLabel;
    juce::HyperlinkButton homepageLink;
    double progressValue = 0.0;
    juce::ProgressBar progressBar { progressValue };
    juce::TextButton primaryButton, useButton { "Use in New Generator" },
                     redownloadButton { "Re-download" }, deleteButton { "Delete" }, revealButton;
    juce::TextEditor terms;

    std::shared_ptr<bool> alive = std::make_shared<bool> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundLibraryComponent)
};

class SoundLibraryWindow : public juce::DocumentWindow
{
public:
    SoundLibraryWindow (SoundLibrary&, std::function<void (SoundLibrary::Kind, const juce::File&)> onUse,
                        std::function<void()> onClose);

    void closeButtonPressed() override;
    void select (const juce::String& id)  { content.select (id); }

private:
    std::function<void()> onClose;
    SoundLibraryComponent content;
};

} // namespace carve::app
