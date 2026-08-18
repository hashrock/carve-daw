#include "MixerComponent.h"

#include "sync/EngineIds.h"

namespace carve::app
{

namespace
{
    constexpr float meterMinDb = -60.0f;
    constexpr float meterMaxDb = 6.0f;

    // dB per timer tick. At 30Hz this is a ~20dB/second fall-off, slow enough
    // to read a transient but fast enough to look live.
    constexpr float meterDecayDb = 0.7f;

    float dbToMeterProportion (float db)
    {
        return juce::jlimit (0.0f, 1.0f, (db - meterMinDb) / (meterMaxDb - meterMinDb));
    }

    // Stamped on to the tracktion plugin by EditSync, and the only way back
    // from a model Effect to the live plugin that plays it. EditSync keeps its
    // own copy of the name; there is no shared header to put it in yet.
    using sync::effectIdProperty;

    juce::String formatDb (float db)
    {
        if (db <= -100.0f)
            return "-inf";
        return juce::String (db, 1);
    }
} // namespace

//==============================================================================
// A stereo peak meter reading one LevelMeasurer.
//
// Owns the LevelMeasurer::Client, which has to be registered with the measurer
// for levels to be measured at all (LevelMeasurer::processBuffer bails out when
// it has no clients). The measurer it watches is passed in on every tick rather
// than cached: a track's meter plugin is destroyed and rebuilt by EditSync when
// the generator's instrument changes, and the master's measurer lives in the
// playback context, which comes and goes with the transport.
class LevelMeterView : public juce::Component
{
public:
    LevelMeterView()  { setInterceptsMouseClicks (false, false); }
    ~LevelMeterView() override  { attach (nullptr); }

    void update (te::LevelMeasurer* wanted)
    {
        attach (wanted);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto peak = measurer != nullptr ? client.getAndClearAudioLevel (channel).dB
                                                  : -100.0f;
            levelDb[channel] = std::max (peak, levelDb[channel] - meterDecayDb);
        }

        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();

        g.setColour (juce::Colour (0xff1c1c20));
        g.fillRect (area);

        const auto channelWidth = area.getWidth() / (float) numChannels;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto bar = area.withWidth (channelWidth)
                           .translated (channelWidth * (float) channel, 0.0f)
                           .reduced (1.0f, 0.0f);
            const auto filled = bar.getHeight() * dbToMeterProportion (levelDb[channel]);

            g.setColour (levelDb[channel] > 0.0f ? juce::Colours::orangered
                                                 : juce::Colour (0xff4fc27a));
            g.fillRect (bar.removeFromBottom (filled));
        }

        // 0dB mark
        g.setColour (juce::Colour (0xff707078));
        const auto zeroY = area.getBottom() - area.getHeight() * dbToMeterProportion (0.0f);
        g.drawHorizontalLine ((int) zeroY, area.getX(), area.getRight());
    }

private:
    static constexpr int numChannels = 2;

    void attach (te::LevelMeasurer* wanted)
    {
        if (wanted == measurer.get())
            return;

        if (auto* m = measurer.get())
            m->removeClient (client);

        measurer = wanted;

        if (wanted != nullptr)
            wanted->addClient (client);
    }

    // WeakReference: the measurer can be destroyed under us.
    juce::WeakReference<te::LevelMeasurer> measurer;
    te::LevelMeasurer::Client client;
    float levelDb[numChannels] { -100.0f, -100.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeterView)
};

//==============================================================================
// One generator's strip.
//
// The insert slots sit above the fader, in a Viewport: a long chain scrolls
// rather than squeezing the fader down to nothing.
class MixerComponent::ChannelStrip : public juce::Component
{
public:
    ChannelStrip (model::Generator generatorToShow, te::Engine& engine, juce::UndoManager& um)
        : generator (generatorToShow), undoManager (um),
          effectSlots (makeGeneratorEffectChain (generatorToShow, um), engine)
    {
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (12.0f));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (11.0f));
        dbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

        // The fader works in tracktion's fader-position space (0..1) so it gets
        // the engine's own taper; the model stores the resulting dB.
        volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
        volumeSlider.setRange (0.0, 1.0);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setDoubleClickReturnValue (
            true, te::decibelsToVolumeFaderPosition (model::Generator::defaultVolumeDb));
        volumeSlider.onDragStart = [this] { undoManager.beginNewTransaction(); };
        volumeSlider.onValueChange = [this]
        {
            if (isRefreshing)
                return;
            generator.setVolumeDb (te::volumeFaderPositionToDB ((float) volumeSlider.getValue()),
                                   &undoManager);
        };

        panSlider.setSliderStyle (juce::Slider::LinearHorizontal);
        panSlider.setRange (-1.0, 1.0);
        panSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        panSlider.setDoubleClickReturnValue (true, 0.0);
        panSlider.onDragStart = [this] { undoManager.beginNewTransaction(); };
        panSlider.onValueChange = [this]
        {
            if (isRefreshing)
                return;
            generator.setPan ((float) panSlider.getValue(), &undoManager);
        };

        muteButton.setClickingTogglesState (true);
        muteButton.onClick = [this]
        {
            if (isRefreshing)
                return;
            undoManager.beginNewTransaction();
            generator.setMuted (muteButton.getToggleState(), &undoManager);
        };

        soloButton.setClickingTogglesState (true);
        soloButton.onClick = [this]
        {
            if (isRefreshing)
                return;
            undoManager.beginNewTransaction();
            generator.setSoloed (soloButton.getToggleState(), &undoManager);
        };

        // The slot list is as tall as its chain; the viewport gives it whatever
        // the strip can spare.
        effectSlots.onPreferredHeightChanged = [this] { resized(); };
        effectViewport.setViewedComponent (&effectSlots, false);
        effectViewport.setScrollBarsShown (true, false);
        effectViewport.setScrollBarThickness (6);

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &panSlider, &effectViewport, &meter, &volumeSlider, &dbLabel,
                 &muteButton, &soloButton })
            addAndMakeVisible (c);

        refresh();
    }

    juce::String getGeneratorId() const  { return generator.getId(); }

    EffectSlotList& getEffectSlots()  { return effectSlots; }

    // Only the chain: kept apart from refresh() because mixer moves arrive
    // continuously while a fader is dragged and must not touch the slots.
    void refreshEffects()  { effectSlots.refreshFromChain(); }

    // Pulls the model back into the controls. Never fires the callbacks above,
    // so a change made elsewhere (undo, another view) cannot bounce back into
    // the model.
    void refresh()
    {
        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);

        nameLabel.setText (generator.getName(), juce::dontSendNotification);
        volumeSlider.setValue (te::decibelsToVolumeFaderPosition (generator.getVolumeDb()),
                               juce::dontSendNotification);
        panSlider.setValue (generator.getPan(), juce::dontSendNotification);
        muteButton.setToggleState (generator.isMuted(), juce::dontSendNotification);
        soloButton.setToggleState (generator.isSoloed(), juce::dontSendNotification);

        dbLabel.setText (formatDb (generator.getVolumeDb()) + " dB", juce::dontSendNotification);
    }

    // Called every tick with the track this strip currently maps to; the track
    // can be destroyed and recreated by EditSync, so re-resolve rather than
    // caching it.
    void updateMeter (te::AudioTrack* track)
    {
        auto* meterPlugin = track != nullptr ? track->getLevelMeterPlugin() : nullptr;
        meter.update (meterPlugin != nullptr ? &meterPlugin->measurer : nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff2b2b30));
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 4);

        nameLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);
        panSlider.setBounds (area.removeFromTop (18));
        area.removeFromTop (4);

        auto buttons = area.removeFromBottom (22);
        muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        soloButton.setBounds (buttons);

        dbLabel.setBounds (area.removeFromBottom (16));
        area.removeFromBottom (4);

        // The chain gets the height it asks for, but never at the price of a
        // fader too short to aim at: past that the slot list scrolls.
        const auto slotsHeight = juce::jlimit (0,
                                               juce::jmax (0, area.getHeight() - minFaderHeight),
                                               effectSlots.getPreferredHeight());

        if (slotsHeight > 0)
        {
            effectViewport.setVisible (true);
            effectViewport.setBounds (area.removeFromTop (slotsHeight));
            effectSlots.setSize (effectViewport.getMaximumVisibleWidth(),
                                 effectSlots.getPreferredHeight());
            area.removeFromTop (4);
        }
        else
        {
            effectViewport.setVisible (false);
        }

        // meter on the left, fader on the right
        meter.setBounds (area.removeFromLeft (18));
        area.removeFromLeft (6);
        volumeSlider.setBounds (area);
    }

private:
    // Below this the fader stops being aimable, so the slots scroll instead.
    static constexpr int minFaderHeight = 110;

    model::Generator generator;
    juce::UndoManager& undoManager;

    juce::Label nameLabel, dbLabel;
    juce::Slider volumeSlider, panSlider;
    juce::TextButton muteButton { "M" }, soloButton { "S" };
    juce::Viewport effectViewport;
    EffectSlotList effectSlots;
    LevelMeterView meter;

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

//==============================================================================
// The master bus: the Edit's master fader, the level at the very end of the
// graph, and the master insert chain -- which is where a limiter across the
// mix goes.
//
// Unlike every other strip this one edits the tracktion Edit directly, because
// the song model has nowhere to put it. That means master moves are not
// undoable and, more importantly, are NOT saved in the .carve: the Edit is
// rebuilt from the model on load, so anything only the Edit knows is gone.
//
// Fixing that is a model change plus an EditSync change, neither of which this
// round owns:
//   - SONG gains a MASTER child holding a volumeDb property and an EFFECTS list
//     of the same EFFECT nodes a generator already uses;
//   - EditSync reconciles edit.getMasterPluginList() against MASTER/EFFECTS
//     exactly as syncEffects() does for a track, and pushes volumeDb into
//     edit.getMasterVolumePlugin().
// Once that exists this strip only has to swap makeMasterEffectChain() for
// makeGeneratorEffectChain()'s model-backed equivalent.
class MixerComponent::MasterStrip : public juce::Component,
                                    private juce::ValueTree::Listener
{
public:
    MasterStrip (te::Edit& editToShow, model::MasterBus bus, juce::UndoManager& undoManager)
        : edit (editToShow),
          masterPluginsState (bus.state),
          effectSlots (makeMasterEffectChain (bus, undoManager), editToShow.engine)
    {
        masterPluginsState.addListener (this);

        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0a24f));
        nameLabel.setText ("MASTER", juce::dontSendNotification);

        // No undo and no song file behind it, so this says so rather than
        // letting a limiter quietly disappear on the next load.
        noticeLabel.setJustificationType (juce::Justification::centred);
        noticeLabel.setFont (juce::FontOptions (9.0f));
        noticeLabel.setColour (juce::Label::textColourId, juce::Colour (0xff86868e));
        noticeLabel.setText ("not saved", juce::dontSendNotification);

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (11.0f));
        dbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

        volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
        volumeSlider.setRange (0.0, 1.0);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setDoubleClickReturnValue (true, te::decibelsToVolumeFaderPosition (0.0f));
        volumeSlider.onValueChange = [this]
        {
            if (isRefreshing)
                return;

            if (auto plugin = edit.getMasterVolumePlugin())
                plugin->setSliderPos ((float) volumeSlider.getValue());
        };

        effectSlots.onPreferredHeightChanged = [this] { resized(); };
        effectViewport.setViewedComponent (&effectSlots, false);
        effectViewport.setScrollBarsShown (true, false);
        effectViewport.setScrollBarThickness (6);

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &noticeLabel, &effectViewport, &meter, &volumeSlider, &dbLabel })
            addAndMakeVisible (c);

        refresh();
    }

    ~MasterStrip() override
    {
        masterPluginsState.removeListener (this);
    }

    EffectSlotList& getEffectSlots()  { return effectSlots; }

    // Nothing tells us when the master volume moves -- there is no model
    // property to listen to -- so it is polled, and left alone while the user
    // is on it.
    void refresh()
    {
        auto plugin = edit.getMasterVolumePlugin();
        const auto position = plugin != nullptr ? plugin->getSliderPos() : 0.0f;

        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);

        if (! volumeSlider.isMouseButtonDown())
            volumeSlider.setValue (position, juce::dontSendNotification);

        dbLabel.setText (formatDb (te::volumeFaderPositionToDB (position)) + " dB",
                         juce::dontSendNotification);
    }

    // The level at the end of the graph, which only exists while there is a
    // playback context to render it.
    void updateMeter()
    {
        auto* context = edit.getCurrentPlaybackContext();
        meter.update (context != nullptr ? &context->masterLevels : nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff33333a));
        g.setColour (juce::Colour (0xff45454e));
        g.drawRect (getLocalBounds(), 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 4);

        nameLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);

        // Where a channel strip has its pan slider. Kept as a gap of the same
        // height so the master fader lines up with the others.
        noticeLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (4);

        // ...and the same again for the mute/solo row.
        area.removeFromBottom (22);

        dbLabel.setBounds (area.removeFromBottom (16));
        area.removeFromBottom (4);

        const auto slotsHeight = juce::jlimit (0,
                                               juce::jmax (0, area.getHeight() - minFaderHeight),
                                               effectSlots.getPreferredHeight());

        if (slotsHeight > 0)
        {
            effectViewport.setVisible (true);
            effectViewport.setBounds (area.removeFromTop (slotsHeight));
            effectSlots.setSize (effectViewport.getMaximumVisibleWidth(),
                                 effectSlots.getPreferredHeight());
            area.removeFromTop (4);
        }
        else
        {
            effectViewport.setVisible (false);
        }

        meter.setBounds (area.removeFromLeft (18));
        area.removeFromLeft (6);
        volumeSlider.setBounds (area);
    }

private:
    static constexpr int minFaderHeight = 110;

    // The plugin list is a ValueTree like everything else, so the chain can be
    // watched rather than polled -- including changes made from somewhere other
    // than these slots.
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override         { effectSlots.refreshFromChain(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override  { effectSlots.refreshFromChain(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override          { effectSlots.refreshFromChain(); }
    void valueTreeParentChanged (juce::ValueTree&) override                        {}

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier& property) override
    {
        // A plugin writes to its own state constantly while a knob is moved;
        // only the bypass flag changes what a slot row looks like.
        if (property == te::IDs::enabled)
            effectSlots.refreshFromChain();
    }

    te::Edit& edit;
    juce::ValueTree masterPluginsState;

    juce::Label nameLabel, noticeLabel, dbLabel;
    juce::Slider volumeSlider;
    juce::Viewport effectViewport;
    EffectSlotList effectSlots;
    LevelMeterView meter;

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};

//==============================================================================
MixerComponent::MixerComponent (te::Edit& editToShow, juce::UndoManager& um)
    : edit (editToShow), undoManager (um)
{
    masterStrip = std::make_unique<MasterStrip> (edit, song.getMasterBus(), undoManager);

    // The slot list knows nothing about where its plugins live, so the mixer
    // hands it the things that need to know.
    auto& masterSlots = masterStrip->getEffectSlots();
    masterSlots.onOpenEffectEditor = [this] (const juce::String& effectId)
    {
        openEffectEditor ({}, effectId);
    };
    masterSlots.onEffectAboutToBeRemoved = [this] (const juce::String& effectId)
    {
        closeEffectWindow (effectId);
    };

    addAndMakeVisible (*masterStrip);

    song.state.addListener (this);
    rebuildStrips();
    startTimerHz (30);
}

MixerComponent::~MixerComponent()
{
    song.state.removeListener (this);
}

void MixerComponent::setSong (model::Song newSong)
{
    // The plugins these were editing belong to the song being replaced.
    effectWindows.clear();

    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);
    rebuildStrips();
}

void MixerComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType (model::ids::EFFECT))
    {
        effectListChanged (tree.getParent());
        return;
    }

    if (! tree.hasType (model::ids::GENERATOR))
        return;

    if (! model::Generator::isMixerProperty (property) && property != model::ids::name)
        return;

    // Update in place: rebuilding here would destroy the fader mid-drag.
    const juce::String generatorId = tree[model::ids::id];

    for (auto& strip : strips)
        if (strip->getGeneratorId() == generatorId)
            strip->refresh();
}

void MixerComponent::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&)
{
    generatorListChanged (parent);
    effectListChanged (parent);
}

void MixerComponent::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    generatorListChanged (parent);
    effectListChanged (parent);
}

void MixerComponent::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    generatorListChanged (parent);
    effectListChanged (parent);
}

void MixerComponent::generatorListChanged (const juce::ValueTree& parent)
{
    // The listener sees the whole song tree, so ignore the note and clip
    // traffic that pattern editing generates.
    if (parent.hasType (model::ids::GENERATORS))
        triggerAsyncUpdate();
}

void MixerComponent::effectListChanged (const juce::ValueTree& parent)
{
    if (! parent.hasType (model::ids::EFFECTS))
        return;

    // Updated in place, not rebuilt: the slot list paints its rows instead of
    // owning components, so it can be re-read from inside this callback.
    const juce::String generatorId = parent.getParent()[model::ids::id];

    for (auto& strip : strips)
        if (strip->getGeneratorId() == generatorId)
            strip->refreshEffects();
}

void MixerComponent::rebuildStrips()
{
    cancelPendingUpdate();
    strips.clear();

    for (const auto& generator : song.getGenerators())
    {
        auto strip = std::make_unique<ChannelStrip> (generator, edit.engine, undoManager);

        // The slot list knows nothing about tracks, so the mixer -- which
        // does -- hands it the things that need one.
        const auto generatorId = generator.getId();
        auto& slots = strip->getEffectSlots();

        slots.onOpenEffectEditor = [this, generatorId] (const juce::String& effectId)
        {
            openEffectEditor (generatorId, effectId);
        };
        slots.onEffectAboutToBeRemoved = [this] (const juce::String& effectId)
        {
            closeEffectWindow (effectId);
        };

        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }

    const auto previousWidth = getWidth();

    // The master strip is always there, and always last.
    setSize ((int) strips.size() * stripWidth + masterGap + stripWidth,
             juce::jmax (minHeight, getHeight()));
    resized();

    if (getWidth() != previousWidth && onContentWidthChanged != nullptr)
        onContentWidthChanged();
}

te::Plugin* MixerComponent::findEffectPlugin (const juce::String& generatorId,
                                              const juce::String& effectId) const
{
    if (effectId.isEmpty())
        return nullptr;

    // No generator means the master chain, where a plugin is addressed by its
    // own EditItemID: there is no model Effect to carry an id of ours.
    if (generatorId.isEmpty())
    {
        for (auto plugin : edit.getMasterPluginList().getPlugins())
            if (plugin->itemID.toString() == effectId)
                return plugin;

        return nullptr;
    }

    const auto generators = song.getGenerators();
    const auto tracks = te::getAudioTracks (edit);

    for (size_t i = 0; i < generators.size(); ++i)
    {
        if (generators[i].getId() != generatorId)
            continue;

        if ((int) i >= tracks.size())
            return nullptr;

        for (auto plugin : tracks[(int) i]->pluginList.getPlugins())
            if (plugin->state.getProperty (effectIdProperty).toString() == effectId)
                return plugin;

        return nullptr;
    }

    return nullptr;
}

void MixerComponent::openEffectEditor (const juce::String& generatorId, const juce::String& effectId)
{
    if (auto existing = effectWindows.find (effectId); existing != effectWindows.end())
    {
        existing->second.window->toFront (true);
        return;
    }

    auto* plugin = findEffectPlugin (generatorId, effectId);

    if (plugin == nullptr)
        return;   // EditSync has not built this one yet

    // destruction is deferred: the close callback runs inside the window's own
    // member function
    auto onClose = [safe = juce::Component::SafePointer (this), effectId]
    {
        juce::MessageManager::callAsync ([safe, effectId]
        {
            if (safe != nullptr)
                safe->effectWindows.erase (effectId);
        });
    };

    std::unique_ptr<juce::DocumentWindow> window;

    // An external effect brings its own UI; an internal one gets the generic
    // parameter list, which is all a tracktion plugin can offer.
    if (auto* external = dynamic_cast<te::ExternalPlugin*> (plugin))
    {
        if (auto* instance = external->getAudioPluginInstance())
            window = std::make_unique<PluginEditorWindow> (*external, std::move (onClose));
    }
    else
    {
        // The compressor has a sidechain input; the picker lets the user feed
        // another generator into it. The choice is written to the model, so it
        // saves and undoes like everything else; EditSync routes the audio.
        std::optional<SidechainPicker> picker;

        if (dynamic_cast<te::CompressorPlugin*> (plugin) != nullptr)
        {
            if (auto generator = song.findGenerator (generatorId))
            {
                if (auto effect = generator->findEffect (effectId))
                {
                    SidechainPicker built;
                    built.currentSourceId = effect->getSidechainSourceId();

                    // Every generator but the one this compressor sits on: a
                    // track feeding its own compressor's trigger is just
                    // ordinary compression with extra steps.
                    for (const auto& source : song.getGenerators())
                        if (source.getId() != generatorId)
                            built.sources.push_back ({ source.getId(), source.getName() });

                    built.onSourceChanged = [this, generatorId, effectId] (const juce::String& sourceId)
                    {
                        if (auto g = song.findGenerator (generatorId))
                        {
                            if (auto e = g->findEffect (effectId))
                            {
                                undoManager.beginNewTransaction();
                                e->setSidechainSourceId (sourceId, &undoManager);
                            }
                        }
                    };

                    picker = std::move (built);
                }
            }
        }

        window = std::make_unique<EffectParameterWindow> (*plugin, std::move (onClose), std::move (picker));
    }

    if (window != nullptr)
        effectWindows[effectId] = { generatorId, te::Plugin::Ptr (plugin), std::move (window) };
}

void MixerComponent::closeEffectWindow (const juce::String& effectId)
{
    effectWindows.erase (effectId);
}

void MixerComponent::closeStaleEffectWindows()
{
    if (effectWindows.empty())
        return;

    juce::StringArray stale;

    for (const auto& [effectId, open] : effectWindows)
        if (findEffectPlugin (open.generatorId, effectId) != open.plugin.get())
            stale.add (effectId);

    // Erasing inside the loop above would invalidate it.
    for (const auto& effectId : stale)
        effectWindows.erase (effectId);
}

void MixerComponent::timerCallback()
{
    closeStaleEffectWindows();

    // generator order == track order (EditSync invariant)
    const auto tracks = te::getAudioTracks (edit);

    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->updateMeter (i < (size_t) tracks.size() ? tracks[(int) i] : nullptr);

    masterStrip->updateMeter();
    masterStrip->refresh();
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    if (strips.empty())
    {
        g.setColour (juce::Colour (0xff707078));
        g.setFont (13.0f);
        g.drawText ("No generators",
                    getLocalBounds().withWidth (juce::jmax (stripWidth, getWidth() - stripWidth)),
                    juce::Justification::centred);
    }
}

void MixerComponent::resized()
{
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * stripWidth, 0, stripWidth, getHeight());

    masterStrip->setBounds (getWidth() - stripWidth, 0, stripWidth, getHeight());
}

} // namespace carve::app
