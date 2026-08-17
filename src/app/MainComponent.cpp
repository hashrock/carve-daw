#include "MainComponent.h"

#include "model/DemoSong.h"

namespace orionish::app
{

MainComponent::MainComponent (te::Engine& engineToUse)
    : engine (engineToUse)
{
    edit = te::Edit::createSingleTrackEdit (engine);

    transportBar = std::make_unique<TransportBar> (*edit, song, undoManager);
    transportBar->onSave = [this] { saveSong(); };
    transportBar->onOpen = [this] { openSong(); };

    generatorPanel = std::make_unique<GeneratorPanel> (engine, song, undoManager);
    generatorPanel->onSelectionChanged = [this] (const juce::String& generatorId,
                                                 const juce::String& patternId)
    {
        selectionChanged (generatorId, patternId);
    };
    generatorPanel->onOpenPluginEditor = [this] (const juce::String& generatorId)
    {
        openPluginEditor (generatorId);
    };
    generatorPanel->onManagePlugins = [this] { openPluginManager(); };

    playlist.onSelectGenerator = [this] (const juce::String& generatorId)
    {
        generatorPanel->selectGenerator (generatorId);
    };

    pianoRollViewport.setViewedComponent (&pianoRoll, false);
    playlistViewport.setViewedComponent (&playlist, false);

    addAndMakeVisible (*transportBar);
    addAndMakeVisible (*generatorPanel);
    addAndMakeVisible (pianoRollViewport);
    addAndMakeVisible (playlistViewport);

    loadSong (model::buildDemoSong());

    setWantsKeyboardFocus (true);
    startTimerHz (30);
    setSize (1200, 760);
}

MainComponent::~MainComponent()
{
    edit->getTransport().stop (false, false);
}

void MainComponent::loadSong (model::Song newSong)
{
    edit->getTransport().stop (false, false);
    pluginEditorWindows.clear();
    undoManager.clearUndoHistory();

    song = std::move (newSong);
    editSync = std::make_unique<sync::EditSync> (song, *edit);

    transportBar->setSong (song);
    playlist.setSong (song);
    generatorPanel->setSong (song);   // fires onSelectionChanged -> updates piano roll
}

void MainComponent::selectionChanged (const juce::String& generatorId, const juce::String& patternId)
{
    std::optional<model::Pattern> pattern;
    if (auto generator = song.findGenerator (generatorId))
        pattern = generator->findPattern (patternId);

    pianoRoll.setPattern (pattern);
    playlist.setSelection (generatorId, patternId);
}

void MainComponent::openPluginEditor (const juce::String& generatorId)
{
    if (auto existing = pluginEditorWindows.find (generatorId);
        existing != pluginEditorWindows.end())
    {
        existing->second->toFront (true);
        return;
    }

    // generator order == track order (EditSync invariant)
    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (*edit);

    for (int i = 0; i < (int) generators.size() && i < tracks.size(); ++i)
    {
        if (generators[(size_t) i].getId() != generatorId)
            continue;

        if (auto external = tracks[i]->pluginList.findFirstPluginOfType<te::ExternalPlugin>())
        {
            if (auto instance = external->getAudioPluginInstance())
            {
                // destruction is deferred: the close callback runs inside the
                // window's own member function
                auto onClose = [safe = juce::Component::SafePointer (this), generatorId]
                {
                    juce::MessageManager::callAsync ([safe, generatorId]
                    {
                        if (safe != nullptr)
                            safe->pluginEditorWindows.erase (generatorId);
                    });
                };
                pluginEditorWindows[generatorId] =
                    std::make_unique<PluginEditorWindow> (*instance, std::move (onClose));
            }
        }
        return;   // internal 4OSC has no editor window yet
    }
}

void MainComponent::openPluginManager()
{
    if (pluginScanWindow != nullptr)
    {
        pluginScanWindow->toFront (true);
        return;
    }
    auto onClose = [safe = juce::Component::SafePointer (this)]
    {
        juce::MessageManager::callAsync ([safe]
        {
            if (safe != nullptr)
                safe->pluginScanWindow.reset();
        });
    };
    pluginScanWindow = std::make_unique<PluginScanWindow> (engine, std::move (onClose));
}

void MainComponent::saveSong()
{
    if (editSync != nullptr)
        editSync->captureLivePluginState();

    fileChooser = std::make_shared<juce::FileChooser> ("Save song", juce::File(), "*.orion");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        song.saveToFile (file.withFileExtension ("orion"));
    });
}

void MainComponent::openSong()
{
    fileChooser = std::make_shared<juce::FileChooser> ("Open song", juce::File(), "*.orion");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& chooser)
    {
        auto file = chooser.getResult();
        if (file == juce::File())
            return;
        if (auto loaded = model::Song::loadFromFile (file))
            loadSong (*loaded);
    });
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        transportBar->togglePlay();
        return true;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
    {
        undoManager.undo();
        return true;
    }
    if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0))
    {
        undoManager.redo();
        return true;
    }
    return false;
}

void MainComponent::timerCallback()
{
    const auto position = edit->getTransport().getPosition();
    playlist.setPlayheadBeats (edit->tempoSequence.toBeats (position).inBeats());
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    transportBar->setBounds (area.removeFromTop (44));

    auto bottom = area.removeFromBottom (190);
    generatorPanel->setBounds (area.removeFromLeft (220));
    pianoRollViewport.setBounds (area);

    bottom.removeFromTop (4);
    playlistViewport.setBounds (bottom);
}

} // namespace orionish::app
