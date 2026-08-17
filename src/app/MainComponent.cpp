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
    generatorPanel->onOpenPatternEditor = [this] { openPatternEditor(); };

    playlist.onSelectGenerator = [this] (const juce::String& generatorId)
    {
        generatorPanel->selectGenerator (generatorId);
    };

    playlistViewport.setViewedComponent (&playlist, false);

    addAndMakeVisible (*transportBar);
    addAndMakeVisible (*generatorPanel);
    addAndMakeVisible (playlistViewport);

    loadSong (model::buildDemoSong());

    setWantsKeyboardFocus (true);
    startTimerHz (30);
    setSize (1100, 620);
}

MainComponent::~MainComponent()
{
    edit->getTransport().stop (false, false);
}

void MainComponent::loadSong (model::Song newSong)
{
    edit->getTransport().stop (false, false);
    pluginEditorWindows.clear();
    pianoRollWindow.reset();
    undoManager.clearUndoHistory();

    song = std::move (newSong);
    editSync = std::make_unique<sync::EditSync> (song, *edit);

    transportBar->setSong (song);
    playlist.setSong (song);
    generatorPanel->setSong (song);   // fires onSelectionChanged -> updates piano roll
}

void MainComponent::selectionChanged (const juce::String& generatorId, const juce::String& patternId)
{
    selectedGeneratorId = generatorId;
    selectedPatternId = patternId;
    playlist.setSelection (generatorId, patternId);

    if (pianoRollWindow != nullptr)
        openPatternEditor();   // retarget the open editor to the new selection
}

void MainComponent::openPatternEditor()
{
    std::optional<model::Pattern> pattern;
    juce::String title;

    if (auto generator = song.findGenerator (selectedGeneratorId))
        if ((pattern = generator->findPattern (selectedPatternId)))
            title = generator->getName() + " / " + pattern->getName();

    if (pianoRollWindow == nullptr)
    {
        auto onClose = [safe = juce::Component::SafePointer (this)]
        {
            juce::MessageManager::callAsync ([safe]
            {
                if (safe != nullptr)
                    safe->pianoRollWindow.reset();
            });
        };
        auto keyHandler = [safe = juce::Component::SafePointer (this)] (const juce::KeyPress& key)
        {
            return safe != nullptr && safe->handleGlobalKey (key);
        };
        pianoRollWindow = std::make_unique<PianoRollWindow> (undoManager, std::move (onClose),
                                                             std::move (keyHandler));
    }

    pianoRollWindow->setPattern (std::move (pattern), title);
    pianoRollWindow->toFront (true);
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
    return handleGlobalKey (key);
}

bool MainComponent::handleGlobalKey (const juce::KeyPress& key)
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
    generatorPanel->setBounds (area.removeFromLeft (220));
    playlistViewport.setBounds (area);
}

} // namespace orionish::app
