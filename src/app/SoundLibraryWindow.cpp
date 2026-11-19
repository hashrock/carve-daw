#include "SoundLibraryWindow.h"

#include "Fonts.h"
#include "TouchUi.h"

namespace carve::app
{

namespace
{
    const juce::Colour background { 0xff232327 };
    const juce::Colour panel { 0xff1c1c20 };
    const juce::Colour outline { 0xff3a3a40 };
    const juce::Colour text { 0xffd8d8dc };
    const juce::Colour dimText { 0xff9a9aa4 };
    const juce::Colour accent { 0xff5fa8e8 };
    const juce::Colour good { 0xff7cc48a };
    const juce::Colour bad { 0xffe07a6a };
    const juce::Colour button { 0xff3d3d46 };

    constexpr int rowHeight = 48;
    constexpr int listWidth = 290;

    void style (juce::Label& label, juce::Colour colour, float height)
    {
        label.setFont (uiFont (height));
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (juce::Justification::centredLeft);
        label.setMinimumHorizontalScale (1.0f);
    }

    void style (juce::TextButton& b, juce::Colour colour = button)
    {
        b.setColour (juce::TextButton::buttonColourId, colour);
        b.setColour (juce::TextButton::textColourOffId, text);
    }

    bool isBusy (SoundLibrary::State s)
    {
        return s == SoundLibrary::State::downloading || s == SoundLibrary::State::verifying
               || s == SoundLibrary::State::installing;
    }

    bool isInstalled (SoundLibrary::State s)
    {
        return s == SoundLibrary::State::installed || s == SoundLibrary::State::updateAvailable;
    }

   #if JUCE_MAC
    const char* revealName = "Show in Finder";
   #elif JUCE_WINDOWS
    const char* revealName = "Show in Explorer";
   #else
    const char* revealName = "Show Folder";
   #endif
} // namespace

SoundLibraryComponent::SoundLibraryComponent (SoundLibrary& libraryIn,
                                              std::function<void (SoundLibrary::Kind, const juce::File&)> onUseIn)
    : library (libraryIn), onUse (std::move (onUseIn))
{
    style (listStateLabel, dimText, fonts::small);
    style (refreshButton);
    refreshButton.onClick = [this] { library.refreshList(); };

    list.setModel (this);
    list.setRowHeight (rowHeight);
    list.setColour (juce::ListBox::backgroundColourId, panel);
    list.setColour (juce::ListBox::outlineColourId, outline);
    list.setOutlineThickness (1);

    style (nameLabel, text, fonts::title);
    style (authorLabel, dimText, fonts::normal);
    style (statusLabel, text, fonts::normal);
    style (noteLabel, dimText, fonts::small);
    noteLabel.setText ("Downloading means accepting the author's terms, shown below. "
                       "Each pack is credited here and in its README-Carve.txt.",
                       juce::dontSendNotification);
    noteLabel.setJustificationType (juce::Justification::topLeft);

    homepageLink.setFont (uiFont (fonts::normal), false, juce::Justification::centredLeft);
    homepageLink.setColour (juce::HyperlinkButton::textColourId, accent);

    progressBar.setColour (juce::ProgressBar::backgroundColourId, panel);
    progressBar.setColour (juce::ProgressBar::foregroundColourId, accent);

    style (primaryButton, juce::Colour (0xff2f5d8a));
    style (useButton, juce::Colour (0xff2f6a45));
    style (redownloadButton);
    style (deleteButton);
    style (revealButton);
    revealButton.setButtonText (revealName);

    primaryButton.onClick = [this] { primaryClicked(); };
    redownloadButton.onClick = [this] { library.download (selectedId); };
    deleteButton.onClick = [this] { confirmDelete(); };
    revealButton.onClick = [this]
    {
        const auto entry = library.getInstalledEntry (selectedId);
        if (entry != juce::File())
            entry.revealToUser();
    };
    useButton.onClick = [this]
    {
        const auto pack = library.getInstalledPack (selectedId);
        const auto entry = library.getInstalledEntry (selectedId);

        if (pack && entry.exists() && onUse)
            onUse (pack->kind, entry);
    };

    terms.setMultiLine (true);
    terms.setReadOnly (true);
    terms.setScrollbarsShown (true);
    terms.setCaretVisible (false);
    terms.setFont (juce::Font (uiFont (fonts::normal)));
    terms.setIndents (8, 8);
    terms.setColour (juce::TextEditor::backgroundColourId, panel);
    terms.setColour (juce::TextEditor::textColourId, text);
    terms.setColour (juce::TextEditor::outlineColourId, outline);
    terms.setColour (juce::TextEditor::focusedOutlineColourId, outline);

    for (auto* c : std::initializer_list<juce::Component*> {
             &listStateLabel, &refreshButton, &list, &nameLabel, &authorLabel, &homepageLink, &statusLabel,
             &progressBar, &primaryButton, &useButton, &redownloadButton, &deleteButton, &noteLabel, &terms })
        addAndMakeVisible (c);

    if (! isTouchUi)
        addAndMakeVisible (revealButton);

    library.addChangeListener (this);
    rebuild();
    library.refreshList();

    setSize (860, 600);
}

SoundLibraryComponent::~SoundLibraryComponent()
{
    *alive = false;
    library.removeChangeListener (this);
    list.setModel (nullptr);
}

void SoundLibraryComponent::select (const juce::String& id)
{
    for (int i = 0; i < (int) packs.size(); ++i)
        if (packs[(size_t) i].id == id)
            list.selectRow (i);
}

//==============================================================================
void SoundLibraryComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuild();
}

void SoundLibraryComponent::rebuild()
{
    packs = library.getPacks();

    switch (library.getListState())
    {
        case SoundLibrary::ListState::loading:
            listStateLabel.setText ("Checking the library...", juce::dontSendNotification);
            break;
        case SoundLibrary::ListState::online:
            listStateLabel.setText (juce::String (packs.size()) + " packs", juce::dontSendNotification);
            break;
        case SoundLibrary::ListState::offline:
            listStateLabel.setText ("Offline: " + library.getListError(), juce::dontSendNotification);
            break;
    }

    list.updateContent();

    int row = -1;

    for (int i = 0; i < (int) packs.size(); ++i)
        if (packs[(size_t) i].id == selectedId)
            row = i;

    if (row < 0 && ! packs.empty())
        row = 0;

    if (row >= 0 && list.getSelectedRow() != row)
        list.selectRow (row);   // calls selectedRowsChanged

    list.repaint();
    updateDetail();
}

int SoundLibraryComponent::getNumRows()
{
    return (int) packs.size();
}

void SoundLibraryComponent::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int) packs.size())
        return;

    const auto& pack = packs[(size_t) row];
    const auto status = library.getStatus (pack.id);

    g.fillAll (selected ? juce::Colour (0xff2c3a4e) : row % 2 == 1 ? juce::Colour (0xff202024) : panel);

    g.setColour (text);
    g.setFont (uiFont (fonts::normal));
    g.drawText (pack.displayName(), 10, 4, width - 20, height / 2 - 2, juce::Justification::bottomLeft, true);

    auto line = (pack.kind == SoundLibrary::Kind::soundFont ? juce::String ("SoundFont") : juce::String ("Voicebank"))
                + "  ·  v" + pack.version + "  ·  " + SoundLibrary::describeSize (pack.size);

    juce::Colour lineColour = dimText;

    if (isBusy (status.state))
    {
        const auto bar = juce::Rectangle<float> (10.0f, (float) height - 9.0f, (float) (width - 20), 3.0f);
        g.setColour (outline);
        g.fillRoundedRectangle (bar, 1.5f);
        g.setColour (accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * (float) juce::jlimit (0.0, 1.0, status.progress)), 1.5f);
        line = describeStatus (pack, status);
        lineColour = accent;
    }
    else if (status.state == SoundLibrary::State::installed)
    {
        line += "  ·  Installed";
        lineColour = good;
    }
    else if (status.state == SoundLibrary::State::updateAvailable)
    {
        line += "  ·  Update";
        lineColour = accent;
    }
    else if (status.state == SoundLibrary::State::failed)
    {
        line += "  ·  Failed";
        lineColour = bad;
    }

    g.setColour (lineColour);
    g.setFont (uiFont (fonts::small));
    g.drawText (line, 10, height / 2, width - 20, height / 2 - 8, juce::Justification::topLeft, true);
}

void SoundLibraryComponent::selectedRowsChanged (int lastRowSelected)
{
    if (lastRowSelected >= 0 && lastRowSelected < (int) packs.size())
        selectedId = packs[(size_t) lastRowSelected].id;

    updateDetail();
}

//==============================================================================
juce::String SoundLibraryComponent::describeStatus (const SoundLibrary::Pack& pack,
                                                    const SoundLibrary::Status& status) const
{
    switch (status.state)
    {
        case SoundLibrary::State::notInstalled:
            return "Not installed. Download " + SoundLibrary::describeSize (pack.size)
                   + ", " + SoundLibrary::describeSize (pack.installedSize) + " installed.";
        case SoundLibrary::State::downloading:
            return "Downloading... " + SoundLibrary::describeSize (status.bytesDone)
                   + " of " + SoundLibrary::describeSize (pack.size);
        case SoundLibrary::State::verifying:
            return "Checking the download...";
        case SoundLibrary::State::installing:
            return "Installing... " + juce::String (juce::roundToInt (status.progress * 100.0)) + "%";
        case SoundLibrary::State::installed:
            return "Installed, version " + status.installedVersion + ".";
        case SoundLibrary::State::updateAvailable:
            return "Version " + status.installedVersion + " is installed. Version " + pack.version + " is available.";
        case SoundLibrary::State::failed:
            return "Failed: " + status.error;
    }

    return {};
}

void SoundLibraryComponent::updateDetail()
{
    const auto it = std::find_if (packs.begin(), packs.end(),
                                  [this] (const SoundLibrary::Pack& p) { return p.id == selectedId; });

    const bool has = it != packs.end();

    for (auto* c : std::initializer_list<juce::Component*> {
             &nameLabel, &authorLabel, &homepageLink, &statusLabel, &primaryButton, &terms, &noteLabel })
        c->setVisible (has);

    if (! has)
    {
        progressBar.setVisible (false);
        useButton.setVisible (false);
        redownloadButton.setVisible (false);
        deleteButton.setVisible (false);
        revealButton.setVisible (false);
        return;
    }

    const auto& pack = *it;
    const auto status = library.getStatus (pack.id);
    const bool busy = isBusy (status.state);
    const bool installed = isInstalled (status.state)
                           || (busy && status.installedVersion.isNotEmpty())
                           || (status.state == SoundLibrary::State::failed && status.installedVersion.isNotEmpty());
    const bool listedNow = library.getListState() != SoundLibrary::ListState::loading
                           || ! pack.url.isEmpty();

    nameLabel.setText (pack.displayName(), juce::dontSendNotification);
    authorLabel.setText ("by " + pack.author, juce::dontSendNotification);
    homepageLink.setButtonText (pack.homepage);
    homepageLink.setURL (juce::URL (pack.homepage));

    statusLabel.setText (describeStatus (pack, status), juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, status.state == SoundLibrary::State::failed ? bad : text);

    progressValue = status.state == SoundLibrary::State::verifying ? -1.0 : status.progress;
    progressBar.setVisible (busy);

    switch (status.state)
    {
        case SoundLibrary::State::notInstalled:     primaryButton.setButtonText ("Accept Terms & Download"); break;
        case SoundLibrary::State::updateAvailable:  primaryButton.setButtonText ("Update to " + pack.version); break;
        case SoundLibrary::State::failed:           primaryButton.setButtonText ("Retry"); break;
        case SoundLibrary::State::downloading:
        case SoundLibrary::State::verifying:
        case SoundLibrary::State::installing:       primaryButton.setButtonText ("Cancel"); break;
        case SoundLibrary::State::installed:        break;
    }

    primaryButton.setVisible (status.state != SoundLibrary::State::installed);
    primaryButton.setEnabled (busy || listedNow);

    useButton.setVisible (installed && ! busy);
    redownloadButton.setVisible (installed && ! busy && status.state == SoundLibrary::State::installed);
    deleteButton.setVisible (installed && ! busy);
    revealButton.setVisible (! isTouchUi && installed && ! busy);

    if (termsShownFor != pack.id + "/" + status.installedVersion)
    {
        termsShownFor = pack.id + "/" + status.installedVersion;
        loadTerms();
    }

    resized();
}

void SoundLibraryComponent::loadTerms()
{
    const auto pack = library.findPack (selectedId);

    if (! pack)
        return;

    const auto header = "Credit\n  " + pack->credit + "\n\n"
                        + "Terms in short\n  " + pack->licenseSummary + "\n\n"
                        + pack->description + "\n\n";

    terms.setText (header + "Loading the author's full terms...", false);

    library.fetchReadme (selectedId, [this, weak = std::weak_ptr<bool> (alive), id = selectedId, header] (const juce::String& readme)
    {
        if (auto a = weak.lock(); a == nullptr || ! *a || id != selectedId)
            return;

        terms.setText (readme.isNotEmpty() ? readme
                                           : header + "The full terms could not be loaded. They are at the "
                                                      "author's site, and in README-Carve.txt once installed.",
                       false);
        terms.moveCaretToTop (false);
    });
}

void SoundLibraryComponent::primaryClicked()
{
    const auto status = library.getStatus (selectedId);

    if (isBusy (status.state))
        library.cancel (selectedId);
    else
        library.download (selectedId);
}

void SoundLibraryComponent::confirmDelete()
{
    const auto pack = library.getInstalledPack (selectedId);

    if (! pack)
        return;

    juce::AlertWindow::showOkCancelBox (
        juce::MessageBoxIconType::QuestionIcon,
        "Delete " + pack->displayName() + "?",
        "Songs that use it will be missing it until it is downloaded again.",
        "Delete", "Cancel", this,
        juce::ModalCallbackFunction::create ([this, weak = std::weak_ptr<bool> (alive), id = pack->id] (int result)
        {
            if (auto a = weak.lock(); a == nullptr || ! *a || result == 0)
                return;

            if (! library.remove (id))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Could not delete",
                                                        "Part of the pack could not be deleted. Is a file in it open?");
        }));
}

//==============================================================================
void SoundLibraryComponent::paint (juce::Graphics& g)
{
    g.fillAll (background);
}

void SoundLibraryComponent::resized()
{
    auto area = getLocalBounds().reduced (12);

    auto left = area.removeFromLeft (juce::jmin (listWidth, area.getWidth() / 2));
    area.removeFromLeft (14);

    auto top = left.removeFromTop (28);
    refreshButton.setBounds (top.removeFromRight (80).reduced (0, 2));
    listStateLabel.setBounds (top);
    left.removeFromTop (6);
    list.setBounds (left);

    nameLabel.setBounds (area.removeFromTop (30));
    auto byline = area.removeFromTop (22);
    authorLabel.setBounds (byline.removeFromLeft (juce::jmin (byline.getWidth() / 2,
        juce::GlyphArrangement::getStringWidthInt (uiFont (fonts::normal), authorLabel.getText()) + 16)));
    homepageLink.setBounds (byline);
    area.removeFromTop (6);
    statusLabel.setBounds (area.removeFromTop (22));

    if (progressBar.isVisible())
        progressBar.setBounds (area.removeFromTop (10).reduced (0, 2));

    area.removeFromTop (6);

    auto buttons = area.removeFromTop (30);
    const auto place = [&buttons] (juce::Component& c, int width)
    {
        if (c.isVisible())
        {
            c.setBounds (buttons.removeFromLeft (width));
            buttons.removeFromLeft (8);
        }
    };

    place (primaryButton, 190);
    place (useButton, 170);
    place (redownloadButton, 110);
    place (deleteButton, 80);
    place (revealButton, 130);

    area.removeFromTop (8);
    noteLabel.setBounds (area.removeFromTop (34));
    area.removeFromTop (4);
    terms.setBounds (area);
}

//==============================================================================
SoundLibraryWindow::SoundLibraryWindow (SoundLibrary& library,
                                        std::function<void (SoundLibrary::Kind, const juce::File&)> onUse,
                                        std::function<void()> onCloseIn)
    : juce::DocumentWindow ("Sound Library", background, juce::DocumentWindow::closeButton),
      onClose (std::move (onCloseIn)),
      content (library, std::move (onUse))
{
    setContentNonOwned (&content, true);
    useSystemTitleBar (*this);
    setResizable (true, false);
    setResizeLimits (640, 420, 1600, 1400);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    toFront (true);
}

void SoundLibraryWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace carve::app
