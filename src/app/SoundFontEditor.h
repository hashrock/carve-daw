#pragma once

#include <functional>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "plugins/SoundFontPlugin.h"

namespace carve::app
{

// The SF2 player's Inst tab: which font, and which of its presets. A GM font
// has a couple of hundred presets, so the list is the whole of the editor and
// a filter sits over it -- typing "bass" is quicker than scrolling for it.
//
// The view reports gestures and shows the plugin's state; the edits are the
// model's (GeneratorController), and the plugin learns them from the sync.
// That round trip is why the list is refreshed from a timer rather than right
// after a click: the plugin is the one that knows which preset ended up
// playing, a fallback included, and it knows a beat after the model does.
class SoundFontEditor : public juce::Component,
                        public juce::FileDragAndDropTarget,
                        private juce::ListBoxModel,
                        private juce::Timer
{
public:
    std::function<void()> onLoadSoundFont;
    std::function<void (const juce::File&)> onSoundFontDropped;
    std::function<void (int bank, int program)> onPresetChosen;
    std::function<void (int note, int velocity)> onPreviewNote;

    explicit SoundFontEditor (plugins::SoundFontPlugin& player) : plugin (&player)
    {
        setSize (560, 440);

        loadButton.onClick = [this] { if (onLoadSoundFont) onLoadSoundFont(); };
        addAndMakeVisible (loadButton);

        fileLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8de));
        fileLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (fileLabel);

        filter.setTextToShowWhenEmpty ("Filter presets", juce::Colour (0xff5e5e68));
        filter.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff1c1c20));
        filter.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff3a3a40));
        filter.onTextChange = [this] { rebuildRows(); };
        filter.onReturnKey = [this] { list.grabKeyboardFocus(); };
        addAndMakeVisible (filter);

        list.setModel (this);
        list.setRowHeight (22);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xff1c1c20));
        list.setColour (juce::ListBox::outlineColourId, juce::Colour (0xff3a3a40));
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        hint.setText ("Click a preset to hear it.  Drop an .sf2 anywhere here to load it.",
                      juce::dontSendNotification);
        hint.setColour (juce::Label::textColourId, juce::Colour (0xff5e5e68));
        hint.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (hint);

        refreshFromPlugin();
        startTimerHz (8);
    }

    ~SoundFontEditor() override
    {
        list.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff232327));

        if (dragOver)
        {
            g.setColour (juce::Colours::white);
            g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2.0f), 6.0f, 2.0f);
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto top = area.removeFromTop (26);
        loadButton.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (10);
        fileLabel.setBounds (top);

        area.removeFromTop (8);
        filter.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);

        hint.setBounds (area.removeFromBottom (20));
        area.removeFromBottom (4);
        list.setBounds (area);
    }

    // Only .sf2 files: an audio file dropped here is a sampler's, and taking
    // it would mean silently failing to load it as a font.
    static bool isSoundFontFile (const juce::File& file)
    {
        return file.hasFileExtension ("sf2");
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (const auto& path : files)
            if (isSoundFontFile (juce::File (path)))
                return true;

        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override  { setDragOver (true); }
    void fileDragExit (const juce::StringArray&) override             { setDragOver (false); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        setDragOver (false);

        for (const auto& path : files)
            if (const juce::File file (path); isSoundFontFile (file))
            {
                if (onSoundFontDropped)
                    onSoundFontDropped (file);

                return;
            }
    }

private:
    //==============================================================================
    int getNumRows() override  { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;

        const auto& preset = rows[(size_t) row];

        if (selected)
            g.fillAll (juce::Colour (0xff3d5a80));
        else if (row % 2 == 1)
            g.fillAll (juce::Colour (0xff202024));

        g.setFont (juce::FontOptions (12.0f));
        g.setColour (juce::Colour (selected ? 0xffe8eef6 : 0xff7a7a84));
        g.drawText (juce::String::formatted ("%03d:%03d", preset.bank, preset.program),
                    8, 0, 64, height, juce::Justification::centredLeft);

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (juce::Colour (selected ? 0xffffffff : 0xffd0d0d6));
        g.drawText (preset.name.isNotEmpty() ? preset.name : juce::String ("(unnamed)"),
                    76, 0, width - 84, height, juce::Justification::centredLeft);
    }

    // Arrow keys move the selection, which is choosing too: stepping down a
    // list hearing each preset is how a font gets browsed.
    void selectedRowsChanged (int row) override
    {
        if (updatingSelection || row < 0 || row >= (int) rows.size())
            return;

        choose (rows[(size_t) row]);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override
    {
        // A click on the row already playing selects nothing new, so
        // selectedRowsChanged stays quiet; it should still sound.
        if (row >= 0 && row < (int) rows.size())
            if (rows[(size_t) row].bank == shownBank && rows[(size_t) row].program == shownProgram)
                preview (rows[(size_t) row]);
    }

    void choose (const plugins::SoundFontPlugin::Preset& preset)
    {
        if (onPresetChosen)
            onPresetChosen (preset.bank, preset.program);

        preview (preset);
    }

    // After a beat: the preset reaches the plugin through the model and the
    // sync, and a note sent at once would play the one being left.
    void preview (const plugins::SoundFontPlugin::Preset& preset)
    {
        const int note = preset.bank == 128 ? 38 : 60;   // a kit's snare, or middle C

        juce::Timer::callAfterDelay (120, [safe = juce::Component::SafePointer<SoundFontEditor> (this), note]
        {
            if (safe != nullptr && safe->onPreviewNote)
                safe->onPreviewNote (note, 100);
        });
    }

    //==============================================================================
    void timerCallback() override  { refreshFromPlugin(); }

    void refreshFromPlugin()
    {
        auto* sf = player();

        if (sf == nullptr)
        {
            setEnabled (false);
            stopTimer();
            return;
        }

        const auto file = sf->getLoadedFile();
        const auto error = sf->getLoadError();

        if (file != shownFile || error != shownError)
        {
            shownFile = file;
            shownError = error;

            if (error.isNotEmpty())
                fileLabel.setText (error, juce::dontSendNotification);
            else if (file == juce::File())
                fileLabel.setText ("No SoundFont loaded", juce::dontSendNotification);
            else
                fileLabel.setText (file.getFileName() + "   (" + juce::String ((int) sf->getPresets().size())
                                       + " presets)", juce::dontSendNotification);

            fileLabel.setColour (juce::Label::textColourId,
                                 juce::Colour (error.isNotEmpty() ? 0xffe0a060 : 0xffd8d8de));
            rebuildRows();
        }

        if (sf->getPlayingBank() != shownBank || sf->getPlayingProgram() != shownProgram)
        {
            shownBank = sf->getPlayingBank();
            shownProgram = sf->getPlayingProgram();
            syncSelection (true);
        }
    }

    void rebuildRows()
    {
        rows.clear();

        if (auto* sf = player())
        {
            const auto words = juce::StringArray::fromTokens (filter.getText(), true);

            for (const auto& preset : sf->getPresets())
            {
                bool matches = true;

                for (const auto& word : words)
                    if (! preset.name.containsIgnoreCase (word))
                        matches = false;

                if (matches)
                    rows.push_back (preset);
            }
        }

        list.updateContent();
        syncSelection (false);
        list.repaint();
    }

    void syncSelection (bool scroll)
    {
        const juce::ScopedValueSetter<bool> guard (updatingSelection, true);

        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(size_t) i].bank == shownBank && rows[(size_t) i].program == shownProgram)
            {
                list.selectRow (i, ! scroll, true);
                return;
            }

        list.deselectAllRows();
    }

    void setDragOver (bool over)
    {
        if (dragOver != over)
        {
            dragOver = over;
            repaint();
        }
    }

    // Gone with its track, which an undo of the generator's creation does.
    te::SafeSelectable<te::Plugin> plugin;

    plugins::SoundFontPlugin* player() const  { return dynamic_cast<plugins::SoundFontPlugin*> (plugin.get()); }

    std::vector<plugins::SoundFontPlugin::Preset> rows;   // the filtered list
    juce::File shownFile;
    juce::String shownError;
    int shownBank = -1, shownProgram = -1;
    bool updatingSelection = false;
    bool dragOver = false;

    juce::TextButton loadButton { "Load SF2..." };
    juce::Label fileLabel, hint;
    juce::TextEditor filter;
    juce::ListBox list;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundFontEditor)
};

} // namespace carve::app
