#include <cmath>

#include "ClipPropertiesPanel.h"
#include "Fonts.h"

namespace carve::app
{

namespace
{
    void styleHeader (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (uiFont (fonts::small));
        label.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    }

    void styleField (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (uiFont (fonts::normal));
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));
    }

    void styleStepper (juce::Slider& slider, double minimum, double maximum, double interval)
    {
        slider.setSliderStyle (juce::Slider::IncDecButtons);
        slider.setRange (minimum, maximum, interval);
        slider.setIncDecButtonsMode (juce::Slider::incDecButtonsDraggable_Vertical);
        // Wide enough for a bar count with its unit spelled out: a number of
        // bars with no unit beside it is exactly the ambiguity this panel had.
        slider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 76, 20);
    }

    // The colour of something the user should act on: a file that is not
    // where the song says it is.
    const juce::Colour warningColour (0xffe0a060);
} // namespace

ClipPropertiesPanel::ClipPropertiesPanel (juce::UndoManager& um)
    : undoManager (um)
{
    styleHeader (clipHeader, "This clip");
    styleHeader (patternHeader, "Pattern");
    styleHeader (audioHeader, "Audio clip");
    styleField (lengthLabel, "Length");
    styleField (transposeLabel, "Transpose");
    styleField (patternLengthLabel, "Length");
    styleField (startLabel, "Start");
    styleField (audioLengthLabel, "Length");

    titleLabel.setFont (uiFont (fonts::title, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    for (auto* l : { &loopLabel, &patternLengthNote, &audioLengthNote })
    {
        l->setFont (uiFont (fonts::small));
        l->setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
    }

    missingLabel.setFont (uiFont (fonts::small));
    missingLabel.setColour (juce::Label::textColourId, warningColour);

    // Bars, because the playlist grid and its snapping are bar-based. A quarter
    // of a bar is the finest step the playlist can show usefully -- one beat in
    // 4/4, and whatever a quarter of the bar is in anything else.
    styleStepper (lengthSlider, 0.25, 256.0, 0.25);
    styleStepper (patternLengthSlider, 0.25, 64.0, 0.25);
    styleStepper (transposeSlider, -48.0, 48.0, 1.0);

    // An audio placement's start is in beats and its length in seconds, the
    // units the model stores (see AudioClip for why they differ), so what the
    // user types is what gets written. The floor on the length is the model's
    // own; the ceilings are just wide enough never to be hit.
    styleStepper (startSlider, 0.0, 100000.0, 0.25);
    styleStepper (audioLengthSlider, model::AudioClip::minLengthSeconds, 36000.0, 0.01);

    lengthSlider.setTextValueSuffix (" bars");
    patternLengthSlider.setTextValueSuffix (" bars");
    startSlider.setTextValueSuffix (" beats");
    audioLengthSlider.setTextValueSuffix (" s");

    lengthSlider.onValueChange = [this] { applyClipLength(); };
    transposeSlider.onValueChange = [this] { applyTranspose(); };
    patternLengthSlider.onValueChange = [this] { applyPatternLength(); };
    startSlider.onValueChange = [this] { applyAudioStart(); };
    audioLengthSlider.onValueChange = [this] { applyAudioLength(); };

    resetLengthButton.onClick = [this]
    {
        if (isRefreshing)
            return;

        undoManager.beginNewTransaction();

        for (auto& clip : liveSelection())
            clip.clearLength (&undoManager);
    };

    reloadButton.onClick = [this] { reloadAudioFile(); };
    relocateButton.onClick = [this] { relocateAudioFile(); };

    reloadButton.setTooltip ("Read the file from disk again");
    relocateButton.setTooltip ("Point this clip at another file");

    for (auto* c : std::initializer_list<juce::Component*> {
             &titleLabel, &clipHeader, &lengthLabel, &lengthSlider, &loopLabel,
             &transposeLabel, &transposeSlider, &resetLengthButton,
             &patternHeader, &patternLengthLabel, &patternLengthSlider,
             &patternLengthNote,
             &audioHeader, &missingLabel, &startLabel, &startSlider,
             &audioLengthLabel, &audioLengthSlider, &audioLengthNote,
             &reloadButton, &relocateButton })
        addAndMakeVisible (c);

    song.state.addListener (this);
    refresh();
}

ClipPropertiesPanel::~ClipPropertiesPanel()
{
    cancelPendingUpdate();
    song.state.removeListener (this);
}

void ClipPropertiesPanel::setSong (model::Song newSong)
{
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);

    selection.clear();
    audioSelection.clear();
    refresh();
}

void ClipPropertiesPanel::setSelection (const std::vector<model::PlaylistClip>& clips,
                                        const std::vector<model::AudioClip>& audioClips)
{
    selection = clips;
    audioSelection = audioClips;
    refresh();
}

double ClipPropertiesPanel::barPositionOf (double beat) const
{
    const auto position = song.toBarsAndBeats (beat);
    const auto beatsPerBar = song.getTimeSigAt (beat).getBeatsPerBar();

    return (double) position.bar + position.beat / juce::jmax (0.0625, beatsPerBar);
}

double ClipPropertiesPanel::beatAtBarPosition (double barPosition) const
{
    const auto bar = (int) std::floor (barPosition);
    const auto barStart = song.beatOfBar (bar);

    // The signature is read at the bar's own start rather than at barPosition,
    // because a change lands on a bar line: asking mid-bar would step into the
    // next signature for a position that is still inside this bar.
    return barStart + (barPosition - (double) bar) * song.getTimeSigAt (barStart).getBeatsPerBar();
}

model::TimeSignature ClipPropertiesPanel::selectedTimeSig() const
{
    const auto clips = liveSelection();

    return clips.empty() ? model::TimeSignature() : song.getTimeSigAt (clips.front().getStart());
}

bool ClipPropertiesPanel::selectedPatternSpansSignatures() const
{
    const auto clips = liveSelection();

    if (clips.empty() || ! songSignatureVaries (song))
        return false;

    const auto patternId = clips.front().getPatternId();
    const auto sig = selectedTimeSig();

    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getPatternId() == patternId && song.getTimeSigAt (clip.getStart()) != sig)
            return true;

    return false;
}

std::vector<model::PlaylistClip> ClipPropertiesPanel::liveSelection() const
{
    std::vector<model::PlaylistClip> result;

    // A clip removed from the playlist keeps a valid ValueTree, just an orphaned
    // one, so a stale selection would otherwise write into nothing.
    for (const auto& clip : selection)
        if (clip.state.getParent().isValid() && patternFor (clip))
            result.push_back (clip);

    return result;
}

std::vector<model::AudioClip> ClipPropertiesPanel::liveAudioSelection() const
{
    std::vector<model::AudioClip> result;

    for (const auto& clip : audioSelection)
        if (clip.state.getParent().isValid())
            result.push_back (clip);

    return result;
}

std::optional<model::Pattern> ClipPropertiesPanel::patternFor (const model::PlaylistClip& clip) const
{
    if (auto generator = song.findGenerator (clip.getGeneratorId()))
        return generator->findPattern (clip.getPatternId());

    return std::nullopt;
}

int ClipPropertiesPanel::countPlacementsOfSelectedPattern() const
{
    const auto clips = liveSelection();
    if (clips.empty())
        return 0;

    const auto patternId = clips.front().getPatternId();
    int count = 0;

    for (const auto& clip : song.getPlaylist().getClips())
        if (clip.getPatternId() == patternId)
            ++count;

    return count;
}

double ClipPropertiesPanel::fileLengthSeconds (const juce::File& file)
{
    // Keyed on the modification time as well as the path, so a Reload after
    // re-exporting the file shows the new length rather than the remembered
    // one.
    const auto stamp = file.getLastModificationTime();

    if (file != cachedLengthFile || stamp != cachedLengthStamp)
    {
        cachedLengthFile = file;
        cachedLengthStamp = stamp;
        cachedLengthSeconds = model::readAudioFileLengthSeconds (file);
    }

    return cachedLengthSeconds;
}

void ClipPropertiesPanel::refresh()
{
    cancelPendingUpdate();

    const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
    const auto clips = liveSelection();
    const auto audioClips = liveAudioSelection();

    // Pattern clips win a mixed selection: they are what the panel was built
    // for, and an audio placement selected alongside still moves, copies and
    // deletes with them.
    const bool showPattern = ! clips.empty();
    const bool showAudio = ! showPattern && ! audioClips.empty();

    for (auto* c : std::initializer_list<juce::Component*> {
             &clipHeader, &lengthLabel, &lengthSlider, &loopLabel, &transposeLabel,
             &transposeSlider, &resetLengthButton, &patternHeader,
             &patternLengthLabel, &patternLengthSlider, &patternLengthNote })
        c->setVisible (showPattern);

    for (auto* c : std::initializer_list<juce::Component*> {
             &audioHeader, &missingLabel, &startLabel, &startSlider,
             &audioLengthLabel, &audioLengthSlider, &audioLengthNote,
             &reloadButton, &relocateButton })
        c->setVisible (showAudio);

    titleLabel.setTooltip ({});

    if (showPattern)
        refreshPatternClip (clips);
    else if (showAudio)
        refreshAudioClip (audioClips);
    else
    {
        titleLabel.setText ("No clip selected", juce::dontSendNotification);
        titleLabel.setColour (juce::Label::textColourId, juce::Colour (0xff707078));
    }
}

void ClipPropertiesPanel::refreshPatternClip (const std::vector<model::PlaylistClip>& clips)
{
    titleLabel.setColour (juce::Label::textColourId, juce::Colours::white);

    const auto& first = clips.front();
    const auto pattern = *patternFor (first);
    const auto patternLength = pattern.getLengthBeats();

    titleLabel.setText (clips.size() == 1
                            ? pattern.getName()
                            : juce::String ((int) clips.size()) + " clips selected",
                        juce::dontSendNotification);

    const auto clipStart = first.getStart();
    const auto clipLength = first.getLength (patternLength);

    // The clip sits at a known beat, so its length is measured along the song's
    // bars: four bars reads as four even if the signature changes inside it.
    lengthSlider.setValue (barPositionOf (clipStart + clipLength) - barPositionOf (clipStart),
                           juce::dontSendNotification);
    transposeSlider.setValue (first.getTranspose(), juce::dontSendNotification);

    // The pattern does not sit anywhere, so there is no bar map to measure it
    // along -- only the signature wherever the selected clip happens to be.
    const auto sig = selectedTimeSig();
    patternLengthSlider.setValue (patternLength / sig.getBeatsPerBar(), juce::dontSendNotification);

    // ... which is why the beats the model actually stores are printed beside
    // it, and why a pattern also placed under another signature says so: its
    // bar count here is not the one the user would read at that other clip.
    patternLengthNote.setText ("bars of " + timeSigText (sig)
                                   + (selectedPatternSpansSignatures() ? " here, " : ", ")
                                   + trimmedNumber (patternLength) + " beats",
                               juce::dontSendNotification);

    const auto repeats = clipLength / juce::jmax (model::Pattern::minLengthBeats, patternLength);
    loopLabel.setText (repeats > 1.001 ? "plays " + trimmedNumber (repeats) + "x through"
                                       : repeats < 0.999 ? "cut short" : juce::String(),
                       juce::dontSendNotification);

    const auto placements = countPlacementsOfSelectedPattern();
    styleHeader (patternHeader, placements > 1
                                    ? "Pattern - affects all " + juce::String (placements) + " placements"
                                    : "Pattern");
}

void ClipPropertiesPanel::refreshAudioClip (const std::vector<model::AudioClip>& clips)
{
    const auto& first = clips.front();
    const auto file = first.getFile();
    const bool single = clips.size() == 1;
    const bool missing = ! file.existsAsFile();

    // The file's name is the clip's name; the whole path is a tooltip away.
    titleLabel.setColour (juce::Label::textColourId, missing ? warningColour : juce::Colours::white);
    titleLabel.setText (single ? (model::FileRef::hasFile (first.state) ? model::FileRef::getFileName (first.state)
                                                                        : juce::String ("(no file)"))
                               : juce::String ((int) clips.size()) + " audio clips selected",
                        juce::dontSendNotification);
    titleLabel.setTooltip (single ? file.getFullPathName() : juce::String());

    missingLabel.setText (missing ? "File not found" : juce::String(), juce::dontSendNotification);

    startSlider.setValue (first.getStart(), juce::dontSendNotification);
    audioLengthSlider.setValue (first.getLengthSeconds(), juce::dontSendNotification);

    // Several clips can be trimmed to one length or re-read together, but
    // moving them all to one beat would stack them, and pointing them all at
    // one file is not a thing anyone means.
    startSlider.setEnabled (single);
    relocateButton.setEnabled (single);

    // How much of the file there is to play, so a trimmed clip reads as such.
    const auto fileLength = missing ? 0.0 : fileLengthSeconds (file);
    audioLengthNote.setText (fileLength > 0.0 ? "of " + trimmedNumber (std::round (fileLength * 100.0) / 100.0) + " s in file"
                                              : juce::String(),
                             juce::dontSendNotification);
}

void ClipPropertiesPanel::applyClipLength()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();
    const auto bars = lengthSlider.getValue();

    // Converted per clip rather than once: the bar count is what the user asked
    // for, and with several clips selected across a signature change the same
    // number of bars is a different number of beats at each of them.
    for (auto& clip : liveSelection())
    {
        const auto start = clip.getStart();
        clip.setLength (beatAtBarPosition (barPositionOf (start) + bars) - start, &undoManager);
    }
}

void ClipPropertiesPanel::applyTranspose()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();
    const auto semitones = (int) transposeSlider.getValue();

    for (auto& clip : liveSelection())
        clip.setTranspose (semitones, &undoManager);
}

void ClipPropertiesPanel::applyPatternLength()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();

    // The signature of the clip the user is looking at, which is the one the
    // spinner beside it is labelled with.
    const auto beats = patternLengthSlider.getValue() * selectedTimeSig().getBeatsPerBar();

    // Deliberately only the first selection's pattern: a multi-clip selection
    // can span several patterns, and quietly resizing all of them is exactly
    // the surprise this panel replaced. It is also the pattern the spinner was
    // filled in from, and whose signature the bars above were counted in.
    const auto clips = liveSelection();

    if (clips.empty())
        return;

    if (auto pattern = patternFor (clips.front()))
        pattern->setLengthBeats (beats, &undoManager);
}

void ClipPropertiesPanel::applyAudioStart()
{
    if (isRefreshing)
        return;

    // Only ever one clip: the spinner is disabled for more (see refresh).
    const auto clips = liveAudioSelection();

    if (clips.size() != 1)
        return;

    undoManager.beginNewTransaction();
    model::AudioClip (clips.front()).setStart (startSlider.getValue(), &undoManager);
}

void ClipPropertiesPanel::applyAudioLength()
{
    if (isRefreshing)
        return;

    undoManager.beginNewTransaction();
    const auto seconds = audioLengthSlider.getValue();

    // The model clamps to its own floor; the spinner's range already matches
    // it, so what is shown afterwards is what was written.
    for (auto& clip : liveAudioSelection())
        clip.setLengthSeconds (seconds, &undoManager);
}

void ClipPropertiesPanel::reloadAudioFile()
{
    if (isRefreshing || ! onReloadAudioClip)
        return;

    for (const auto& clip : liveAudioSelection())
        onReloadAudioClip (clip.getId());

    // The file may have come back, or changed length; nothing in the model
    // moved, so no listener will ask for this.
    cachedLengthFile = juce::File();
    refresh();
}

void ClipPropertiesPanel::relocateAudioFile()
{
    const auto clips = liveAudioSelection();

    if (isRefreshing || clips.size() != 1)
        return;

    // Same shape as the missing-media dialog's Locate: the chooser opens where
    // the file was and filters on the extension it had, and the placement is
    // repointed through FileRef so the next save stores a fresh relative path.
    const auto node = clips.front().state;
    const auto name = model::FileRef::getFileName (node);
    const auto extension = name.fromLastOccurrenceOf (".", true, false);

    chooser = std::make_shared<juce::FileChooser> ("Relocate " + (name.isNotEmpty() ? name : juce::String ("audio clip")),
                                                   model::FileRef::getFile (node).getParentDirectory(),
                                                   extension.isNotEmpty() ? "*" + extension : "*");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer (this), node] (const juce::FileChooser& fc)
    {
        const auto file = fc.getResult();

        // The clip can have been deleted while the dialog was up.
        if (safe == nullptr || file == juce::File() || ! node.isAChildOf (safe->song.state))
            return;

        // Undoable, unlike the load-time relink: the song was playing a file
        // and the user chose to make it play another.
        safe->undoManager.beginNewTransaction();
        model::AudioClip (node).setFile (file, &safe->undoManager);
    });
}

void ClipPropertiesPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff2b2b30));
    g.setColour (juce::Colour (0xff3a3a40));
    g.drawVerticalLine (0, 0.0f, (float) getHeight());
}

void ClipPropertiesPanel::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    titleLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (8);

    auto row = [&area] (juce::Component& label, juce::Component& control)
    {
        auto line = area.removeFromTop (22);
        label.setBounds (line.removeFromLeft (66));
        control.setBounds (line);
        area.removeFromTop (4);
    };

    // The two layouts share the top of the panel; only one set is visible at
    // a time, so they are laid out over each other from a copy of the same
    // starting area.
    auto audioArea = area;

    clipHeader.setBounds (area.removeFromTop (16));
    area.removeFromTop (2);
    row (lengthLabel, lengthSlider);
    loopLabel.setBounds (area.removeFromTop (14).withTrimmedLeft (66));
    area.removeFromTop (4);
    row (transposeLabel, transposeSlider);
    resetLengthButton.setBounds (area.removeFromTop (22).withTrimmedLeft (66));

    area.removeFromTop (14);
    patternHeader.setBounds (area.removeFromTop (16));
    area.removeFromTop (2);
    row (patternLengthLabel, patternLengthSlider);
    patternLengthNote.setBounds (area.removeFromTop (14));

    std::swap (area, audioArea);

    missingLabel.setBounds (area.removeFromTop (14));
    area.removeFromTop (4);
    audioHeader.setBounds (area.removeFromTop (16));
    area.removeFromTop (2);
    row (startLabel, startSlider);
    row (audioLengthLabel, audioLengthSlider);
    audioLengthNote.setBounds (area.removeFromTop (14).withTrimmedLeft (66));
    area.removeFromTop (10);

    auto buttons = area.removeFromTop (22);
    reloadButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).withTrimmedRight (3));
    relocateButton.setBounds (buttons.withTrimmedLeft (3));
}

} // namespace carve::app
