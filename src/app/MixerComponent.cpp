#include "MixerComponent.h"

namespace orionish::app
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

    juce::String formatDb (float db)
    {
        if (db <= -100.0f)
            return "-inf";
        return juce::String (db, 1);
    }
} // namespace

//==============================================================================
// One generator's strip. Owns a LevelMeasurer::Client, which has to be
// registered with the track's meter plugin for levels to be measured at all
// (LevelMeasurer::processBuffer bails out when it has no clients).
class MixerComponent::ChannelStrip : public juce::Component
{
public:
    ChannelStrip (model::Generator generatorToShow, juce::UndoManager& um)
        : generator (std::move (generatorToShow)), undoManager (um)
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

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &panSlider, &volumeSlider, &dbLabel, &muteButton, &soloButton })
            addAndMakeVisible (c);

        refresh();
    }

    ~ChannelStrip() override
    {
        detachMeter();
    }

    juce::String getGeneratorId() const  { return generator.getId(); }

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
        attachMeter (track != nullptr ? track->getLevelMeterPlugin() : nullptr);

        for (int channel = 0; channel < numMeterChannels; ++channel)
        {
            const auto peak = measurer != nullptr ? client.getAndClearAudioLevel (channel).dB
                                                  : -100.0f;
            meterDb[channel] = std::max (peak, meterDb[channel] - meterDecayDb);
        }

        repaint (getMeterBounds());
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff2b2b30));
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        auto meterArea = getMeterBounds().toFloat();
        g.setColour (juce::Colour (0xff1c1c20));
        g.fillRect (meterArea);

        const auto channelWidth = meterArea.getWidth() / (float) numMeterChannels;

        for (int channel = 0; channel < numMeterChannels; ++channel)
        {
            auto bar = meterArea.withWidth (channelWidth)
                                .translated (channelWidth * (float) channel, 0.0f)
                                .reduced (1.0f, 0.0f);
            const auto filled = bar.getHeight() * dbToMeterProportion (meterDb[channel]);

            g.setColour (meterDb[channel] > 0.0f ? juce::Colours::orangered
                                                 : juce::Colour (0xff4fc27a));
            g.fillRect (bar.removeFromBottom (filled));
        }

        // 0dB mark
        g.setColour (juce::Colour (0xff707078));
        const auto zeroY = meterArea.getBottom()
                               - meterArea.getHeight() * dbToMeterProportion (0.0f);
        g.drawHorizontalLine ((int) zeroY, meterArea.getX(), meterArea.getRight());
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

        // meter on the left, fader on the right
        meterBounds = area.removeFromLeft (18);
        area.removeFromLeft (6);
        volumeSlider.setBounds (area);
    }

private:
    static constexpr int numMeterChannels = 2;

    juce::Rectangle<int> getMeterBounds() const  { return meterBounds; }

    void attachMeter (te::LevelMeterPlugin* meterPlugin)
    {
        auto* wanted = meterPlugin != nullptr ? &meterPlugin->measurer : nullptr;
        if (wanted == measurer.get())
            return;

        detachMeter();

        if (wanted != nullptr)
        {
            wanted->addClient (client);
            measurer = wanted;
        }
    }

    void detachMeter()
    {
        if (auto* m = measurer.get())
            m->removeClient (client);
        measurer = nullptr;
    }

    model::Generator generator;
    juce::UndoManager& undoManager;

    juce::Label nameLabel, dbLabel;
    juce::Slider volumeSlider, panSlider;
    juce::TextButton muteButton { "M" }, soloButton { "S" };

    juce::Rectangle<int> meterBounds;
    bool isRefreshing = false;

    // WeakReference: EditSync can delete the meter plugin under us when a
    // generator's instrument changes.
    juce::WeakReference<te::LevelMeasurer> measurer;
    te::LevelMeasurer::Client client;
    float meterDb[numMeterChannels] { -100.0f, -100.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStrip)
};

//==============================================================================
MixerComponent::MixerComponent (te::Edit& editToShow, juce::UndoManager& um)
    : edit (editToShow), undoManager (um)
{
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
    song.state.removeListener (this);
    song = std::move (newSong);
    song.state.addListener (this);
    rebuildStrips();
}

void MixerComponent::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
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
}

void MixerComponent::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int)
{
    generatorListChanged (parent);
}

void MixerComponent::valueTreeChildOrderChanged (juce::ValueTree& parent, int, int)
{
    generatorListChanged (parent);
}

void MixerComponent::generatorListChanged (const juce::ValueTree& parent)
{
    // The listener sees the whole song tree, so ignore the note and clip
    // traffic that pattern editing generates.
    if (parent.hasType (model::ids::GENERATORS))
        triggerAsyncUpdate();
}

void MixerComponent::rebuildStrips()
{
    cancelPendingUpdate();
    strips.clear();

    for (const auto& generator : song.getGenerators())
    {
        auto strip = std::make_unique<ChannelStrip> (generator, undoManager);
        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }

    setSize (juce::jmax (stripWidth, (int) strips.size() * stripWidth),
             juce::jmax (minHeight, getHeight()));
    resized();
}

void MixerComponent::timerCallback()
{
    // generator order == track order (EditSync invariant)
    const auto tracks = te::getAudioTracks (edit);

    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->updateMeter (i < (size_t) tracks.size() ? tracks[(int) i] : nullptr);
}

void MixerComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));

    if (strips.empty())
    {
        g.setColour (juce::Colour (0xff707078));
        g.setFont (13.0f);
        g.drawText ("No generators", getLocalBounds(), juce::Justification::centred);
    }
}

void MixerComponent::resized()
{
    for (size_t i = 0; i < strips.size(); ++i)
        strips[i]->setBounds ((int) i * stripWidth, 0, stripWidth, getHeight());
}

} // namespace orionish::app
