#pragma once

#include <map>

#include <tracktion_engine/tracktion_engine.h>

#include "RecentMediaList.h"
#include "model/Capture.h"
#include "model/SongModel.h"

namespace te = tracktion;

namespace carve::app
{

// The selection authority and model-edit hub the generator panel used to be,
// with the panel's UI gone: the playlist's row labels are the generator list
// now, and the generator window's header is the pattern picker. What is left
// here is everything those views call back into -- which generator and
// pattern are selected, making and cloning and deleting patterns, the
// add-generator menu, sample and drum-pad assignment, and pattern MIDI in/out.
class GeneratorController : private juce::ValueTree::Listener,
                            private juce::AsyncUpdater
{
public:
    GeneratorController (te::Engine& engineToUse, model::Song songModel, juce::UndoManager& um);
    ~GeneratorController() override;

    // (generatorId, patternId) — patternId may be empty if none exists
    std::function<void (const juce::String&, const juce::String&)> onSelectionChanged;
    std::function<void()> onManagePlugins;

    // A generator was just made that has nothing to play yet -- a sampler, a
    // SoundFont player, a singer -- and it is selected: its Inst tab is
    // where it gets something, so the owner should open it there. Nothing
    // is asked for on the way: the tab offers a chooser and the history.
    std::function<void()> onOpenInstrumentForSetup;

    // What each kind of instrument was last given, newest first, and only
    // what is still on disk. For the empty Inst tabs' Recent lists.
    std::vector<RecentMediaList::Item> getRecentMedia (MediaKind) const;

    // A Recent entry clicked: loads it into the selected generator, if that
    // is the kind it is for.
    void recentMediaChosen (MediaKind, const juce::File&);

    // Fired after every refresh, which any change to the song tree triggers,
    // so the generator window can restock its pattern picker and recolour its
    // pads without listening to the tree itself.
    std::function<void()> onPatternsChanged;

    void setSong (model::Song newSong);
    void selectGenerator (const juce::String& generatorId);

    // Selects one of the current generator's patterns. Call after
    // selectGenerator: selecting a generator picks the pattern it was last
    // left on (its first, to begin with), so the order matters.
    void selectPattern (const juce::String& patternId);

    juce::String getSelectedGeneratorId() const;
    juce::String getSelectedPatternId() const;
    std::optional<model::Generator> getSelectedGenerator() const;

    // Every generator's current pattern, in generator order: the one its
    // window last showed, or its first until one has been picked. Generators
    // without a pattern (audio, or emptied by an undo) are left out. Pattern
    // mode plays all of these together -- the pattern under each instrument,
    // not just the one under the cursor.
    struct CurrentPattern
    {
        juce::String generatorId, patternId;
    };
    std::vector<CurrentPattern> getCurrentPatterns() const;

    // The picker's New button: a fresh, empty, automatically named pattern on
    // the selected generator, selected on the spot. Nothing to type, so a new
    // pattern costs one click.
    void createPattern();

    // The picker's Clone button (and cmd-D): a copy of the selected pattern,
    // named after it, selected on the spot. The same one click -- cloning is
    // the middle of "make one, clone it, vary it", and a dialog there is what
    // breaks the take.
    void clonePattern();

    // Capture: the last phrase of what was played becomes a new pattern of the
    // selected generator, selected on the spot, as one undoable step. False
    // when there was nothing to capture or nowhere to put it.
    bool capturePattern (const std::vector<model::capture::PlayedNote>& played);

    // The picker's menu button: rename, delete, copy to another generator,
    // MIDI in and out. screenArea is where to hang the menu.
    void showPatternMenu (juce::Rectangle<int> screenArea);

    // The add-generator menu (synth / sampler / drum kit / plugin / audio),
    // hung off screenArea -- the playlist's "+ Generator" button.
    void showAddGeneratorMenu (juce::Rectangle<int> screenArea);

    // Copies what the live plugins hold into the song (EditSync's capture).
    // A duplicate is made from the song, and a knob turned since the last
    // save is only in the plugin until this runs.
    std::function<void()> onCaptureLivePluginState;

    // A generator's own menu -- rename, duplicate, replace its instrument,
    // delete -- hung off screenArea, which is its row label on the playlist.
    void showGeneratorMenu (const juce::String& generatorId, juce::Rectangle<int> screenArea);

    // The plain sampler's "one sample across the whole keyboard" chooser, for
    // the generator window's Load Sample button. Ignored unless the selected
    // generator is a plain sampler -- a drum kit's sounds are its pads.
    void chooseSampleForSelected();

    // The drum pads in the generator window's Inst tab. A click on an empty
    // pad opens the sample chooser; on a filled one, a replace/clear menu hung
    // off screenArea. Drops fill pads from startPad on.
    void padClicked (int pad, juce::Rectangle<int> screenArea);
    void padFilesDropped (int startPad, const juce::StringArray& files);

    // Files dropped on a plain sampler's Inst tab: the first one the engine
    // can read replaces its sample. Ignored for every other kind, whose
    // drops land elsewhere (a kit's on its pads).
    void sampleFilesDropped (const juce::StringArray& files);

    // The browser's double-click. What "load" means depends on what is
    // selected: a plain sampler takes the file as its sample, a drum kit
    // puts it on its first empty pad, and anything else gets a new sampler
    // generator made from it -- the add menu's "Sampler..." without the
    // chooser.
    void loadSampleIntoSelected (const juce::File& sample);

    // The SF2 player's Inst tab: its Load button, an .sf2 dropped on it, and
    // a preset picked from its list. All three are ignored unless the
    // selected generator is a SoundFont one, and each is one undoable step.
    void chooseSoundFontForSelected();
    void soundFontDroppedOnSelected (const juce::File& soundFont);
    void setSoundFontPresetForSelected (int bank, int program);

    // The singer's Inst tab: its Load button and a voicebank folder dropped
    // on it. Ignored unless the selected generator is a UTAU one.
    void chooseVoicebankForSelected();
    void voicebankDroppedOnSelected (const juce::File& folder);

    // The break slicer's Inst tab. A loop comes from the Load button, a drop
    // or the browser; each edit of its cuts, bars or repitch is one undo
    // step. All are ignored unless the selected generator is a slicer.
    void chooseBreakForSelected();
    void breakDroppedOnSelected (const juce::File& loop);
    void setBreakCutsForSelected (const std::vector<double>& cuts);
    void moveBreakCutForSelected (int index, double seconds);
    void addBreakCutForSelected (double seconds);
    void removeBreakCutForSelected (int index);
    void setBreakBarsForSelected (int bars);
    void setBreakRepitchForSelected (bool repitch);

    // One of the singer's voice knobs, let go of: one undo step.
    void setVoiceStyleValueForSelected (const juce::Identifier&, double value);

    // Whether the engine can read any of these as audio, for the pad grid's
    // drag-over test.
    bool canImportAudioFiles (const juce::StringArray& files) const;

private:
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override              { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override       { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override               { triggerAsyncUpdate(); }
    void valueTreeParentChanged (juce::ValueTree&) override                             {}
    void handleAsyncUpdate() override  { refresh(); }

    void refresh();
    void fireSelectionChanged();
    double newPatternLengthBeats() const;
    model::Generator addGenerator (const juce::String& name, const juce::String& type,
                                   const juce::PluginDescription* description);

    // The sampler side. A sample is picked with a file chooser (or a drop);
    // there is no key-zone editor, so one sample covers the whole keyboard and
    // replaces whatever the generator had.
    void launchSampleChooser (std::function<void (const juce::File&)> onChosen);

    // The chooser reopens where the last sample was picked from, so filling a
    // kit pad by pad does not walk back down to the sample library each time.
    juce::File getSampleBrowseDirectory() const;
    void rememberSampleDirectory (const juce::File& sample);
    void addSamplerGenerator (const juce::File& sample);

    // The SoundFont side: the same shape as the sampler's, with a chooser
    // that only offers .sf2 files and remembers its own folder -- fonts and
    // samples rarely live together.
    void launchSoundFontChooser (std::function<void (const juce::File&)> onChosen);
    void addSoundFontGenerator (const juce::File& soundFont);

    // The singer's: a folder chooser that starts in the last voicebank's
    // parent, or OpenUtau's singers folder the first time.
    void launchVoicebankChooser (std::function<void (const juce::File&)> onChosen);
    void addUtauGenerator (const juce::File& folder);
    void assignVoicebank (model::Generator, const juce::File& folder);

    // The slicer's: a loop chooser that remembers its own folder (loops are
    // a library of their own), and the load, which cuts the loop evenly and
    // gives the slicer a pattern that plays it as recorded.
    void launchBreakChooser (std::function<void (const juce::File&)> onChosen);
    void assignBreak (model::Generator, const juce::File& loop);

    // How long the selected slicer's loop is, which every edit of its cuts
    // is bounded by. Read from the file once per file.
    double getBreakLoopSeconds (const model::Generator&);

    // One edit of a slicer's cuts or bars, as one undo step, with every
    // pattern that played the loop as recorded before it rewritten to play
    // it as recorded after (see Generator::playsBreakAsRecorded).
    void editBreak (const std::function<void (model::Generator&, double loopSeconds)>& edit);
    juce::File breakLengthFile;
    juce::Time breakLengthStamp;
    double breakLengthSeconds = 0.0;

    void assignSoundFont (model::Generator, const juce::File& soundFont);
    void assignSample (model::Generator, const juce::File& sample);

    // The drum-kit side. A kit is the same sampler with one sound per pad, so
    // all of this is addSound/removeSound with the key range pinned to the
    // pad's note -- see DrumPadGrid.h for the note layout.
    void assignPadSample (model::Generator, int pad, const juce::File& sample);
    void clearPad (model::Generator, int pad);

    // The model half of the above, without a transaction or a refresh, so a
    // multi-file drop lands as one undoable gesture.
    void setPadSound (model::Generator&, int pad, const juce::File& sample);

    void ensureValidSelection();

    // Adds to a kind's history; every way a file reaches an instrument
    // comes through here.
    void rememberMedia (MediaKind, const juce::File&);

    // A generator made empty is named "Sampler 3", "UTAU Singer 2"...; the
    // first file it is given names it instead, unless the user has named it
    // something else meanwhile.
    void nameAfterFirstMedia (model::Generator&, bool wasEmpty, const juce::String& newName);

    // Everything behind showPatternMenu works on (generatorId, patternId)
    // rather than on a Pattern, because the menu and the file choosers behind
    // it are asynchronous and the song may be replaced while one is open.
    void duplicatePattern (const juce::String& generatorId, const juce::String& patternId,
                           const juce::String& destinationGeneratorId);
    void renamePattern (const juce::String& generatorId, const juce::String& patternId);
    void renameGenerator (const juce::String& generatorId);
    void deleteGenerator (const juce::String& generatorId);

    // Song::duplicateGenerator, with the copy selected.
    void duplicateGenerator (const juce::String& generatorId);

    // Generator::replaceInstrument as one undo step. `stem` is what a new
    // generator of the type is named after, which an app-given name
    // follows; opensSetup takes it to its Inst tab, for a kind that is made
    // empty and filled there.
    void replaceInstrument (const juce::String& generatorId, const juce::String& type, const juce::String& stem,
                            const juce::PluginDescription*, bool opensSetup);
    void deletePattern (const juce::String& generatorId, const juce::String& patternId);
    void exportPatternToMidi (const juce::String& generatorId, const juce::String& patternId);
    void importMidiIntoPattern (const juce::String& generatorId, const juce::String& patternId);

    te::Engine& engine;
    model::Song song;
    juce::UndoManager& undoManager;

    // Have to outlive launchAsync, so they can't be locals. Two of them,
    // because a sample chooser and a MIDI chooser are never open at once but
    // one would otherwise cancel the other's callback on being replaced.
    std::unique_ptr<juce::FileChooser> sampleChooser, midiChooser;

    // Same reason: the rename box is asynchronous too.
    std::unique_ptr<juce::AlertWindow> renameWindow;

    juce::String selectedGeneratorId;
    juce::String selectedPatternId;

    // Each generator's last selected pattern, by generator id, so coming back
    // to a generator lands on the pattern it was left on and Pattern mode can
    // play that one for every generator at once. Session state rather than
    // part of the song: writing it to the tree would dirty the document and
    // resync the Edit on every click of the picker. Entries can name patterns
    // that have since been deleted; readers validate against the song.
    std::map<juce::String, juce::String> lastPatternByGenerator;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratorController)
};

} // namespace carve::app
