#include "MixerComponent.h"

#include "../sync/EffectChains.h"

#include "LevelMeterView.h"

#include "sync/EngineIds.h"

namespace carve::app
{

namespace
{
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
// A label that passes its double clicks on. juce::Label handles them itself --
// to start editing, which a non-editable one cannot do -- so they otherwise go
// nowhere.
class DoubleClickLabel : public juce::Label
{
public:
    std::function<void()> onDoubleClick;

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && onDoubleClick != nullptr)
        {
            onDoubleClick();
            return;
        }

        juce::Label::mouseDoubleClick (e);
    }
};

//==============================================================================
// One generator's strip.
//
// The insert slots sit above the fader, in a Viewport: a long chain scrolls
// rather than squeezing the fader down to nothing.
class MixerComponent::ChannelStrip : public juce::Component
{
public:
    ChannelStrip (model::Generator generatorToShow, te::Engine& engine, juce::UndoManager& um,
                  const std::vector<model::Return>& returns)
        : generator (generatorToShow), undoManager (um),
          effectSlots (makeGeneratorEffectChain (generatorToShow, um), engine)
    {
        // One small send knob per return bus. All the way down reads as "off"
        // and removes the SEND node, so an untouched send stays out of the
        // file.
        for (const auto& ret : returns)
        {
            auto row = std::make_unique<SendRow>();
            row->returnId = ret.getId();

            row->label.setText (ret.getName(), juce::dontSendNotification);
            row->label.setFont (juce::FontOptions (11.0f));
            row->label.setColour (juce::Label::textColourId, juce::Colour (0xff8a8a94));
            row->label.setInterceptsMouseClicks (false, false);

            auto& slider = row->slider;
            slider.setSliderStyle (juce::Slider::LinearHorizontal);
            slider.setRange (sendOffDb, 6.0);
            slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            slider.setDoubleClickReturnValue (true, sendOffDb);

            const auto returnId = ret.getId();
            slider.onDragStart = [this] { undoManager.beginNewTransaction(); };
            slider.onValueChange = [this, returnId, sliderPtr = &slider]
            {
                if (isRefreshing)
                    return;

                const auto db = (float) sliderPtr->getValue();

                if (db <= sendOffDb + 0.5)
                    generator.removeSend (returnId, &undoManager);
                else
                    generator.setSendGain (returnId, db, &undoManager);
            };

            addAndMakeVisible (row->label);
            addAndMakeVisible (slider);
            sendRows.push_back (std::move (row));
        }

        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (13.0f));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd8d8dc));

        // Nothing on the strip says the name is a door, so the cursor does --
        // there is no tooltip window over the mixer, and the window's help bar
        // names the gesture instead.
        nameLabel.setMouseCursor (juce::MouseCursor::PointingHandCursor);
        nameLabel.onDoubleClick = [this]
        {
            if (onOpenGenerator)
                onOpenGenerator();
        };

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (12.0f));
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

        for (auto& row : sendRows)
        {
            const auto send = generator.findSend (row->returnId);

            if (! row->slider.isMouseButtonDown())
                row->slider.setValue (send ? send->getGainDb() : sendOffDb,
                                      juce::dontSendNotification);
        }

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

        for (auto it = sendRows.rbegin(); it != sendRows.rend(); ++it)
        {
            auto row = area.removeFromBottom (14);
            (*it)->label.setBounds (row.removeFromLeft (34));
            (*it)->slider.setBounds (row);
        }

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

    // Set by the mixer, which is the one that knows what opening a generator
    // means; the strip only knows its name was double-clicked.
    std::function<void()> onOpenGenerator;

private:
    // Below this the fader stops being aimable, so the slots scroll instead.
    static constexpr int minFaderHeight = 110;

    model::Generator generator;
    juce::UndoManager& undoManager;

    DoubleClickLabel nameLabel;
    juce::Label dbLabel;
    juce::Slider volumeSlider, panSlider;
    juce::TextButton muteButton { "M" }, soloButton { "S" };

    static constexpr double sendOffDb = -60.0;

    struct SendRow
    {
        juce::String returnId;
        juce::Label label;
        juce::Slider slider;
    };

    std::vector<std::unique_ptr<SendRow>> sendRows;
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
    MasterStrip (te::Edit& editToShow, model::MasterBus busToShow, juce::UndoManager& um)
        : edit (editToShow),
          bus (std::move (busToShow)),
          undoManager (um),
          masterPluginsState (bus.state),
          effectSlots (makeMasterEffectChain (bus, um), editToShow.engine)
    {
        masterPluginsState.addListener (this);

        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0a24f));
        nameLabel.setText ("MASTER", juce::dontSendNotification);

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (12.0f));
        dbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

        volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
        volumeSlider.setRange (0.0, 1.0);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setDoubleClickReturnValue (
            true, te::decibelsToVolumeFaderPosition (model::MasterBus::defaultVolumeDb));
        volumeSlider.onDragStart = [this] { undoManager.beginNewTransaction(); };
        volumeSlider.onValueChange = [this]
        {
            if (isRefreshing)
                return;

            // Through the model, like every other fader, so it saves and
            // undoes; EditSync pushes it into the Edit's master volume.
            bus.setVolumeDb (te::volumeFaderPositionToDB ((float) volumeSlider.getValue()),
                             &undoManager);
        };

        effectSlots.onPreferredHeightChanged = [this] { resized(); };
        effectViewport.setViewedComponent (&effectSlots, false);
        effectViewport.setScrollBarsShown (true, false);
        effectViewport.setScrollBarThickness (6);

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &effectViewport, &meter, &volumeSlider, &dbLabel })
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
        const auto db = bus.getVolumeDb();
        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);

        if (! volumeSlider.isMouseButtonDown())
            volumeSlider.setValue (te::decibelsToVolumeFaderPosition (db), juce::dontSendNotification);

        dbLabel.setText (formatDb (db) + " dB", juce::dontSendNotification);
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
    model::MasterBus bus;
    juce::UndoManager& undoManager;
    juce::ValueTree masterPluginsState;

    juce::Label nameLabel, dbLabel;
    juce::Slider volumeSlider;
    juce::Viewport effectViewport;
    EffectSlotList effectSlots;
    LevelMeterView meter;

    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MasterStrip)
};

//==============================================================================
// One return bus: its shared effects, its fader, and mute. Meterless -- the
// return's level is audible in the master meter, and a per-return meter would
// need a meter plugin on a track the model deliberately keeps minimal.
class MixerComponent::ReturnStrip : public juce::Component
{
public:
    ReturnStrip (te::Engine& engine, model::Return returnToShow, juce::UndoManager& um)
        : ret (std::move (returnToShow)),
          undoManager (um),
          effectSlots (makeReturnEffectChain (ret, um), engine)
    {
        nameLabel.setJustificationType (juce::Justification::centred);
        nameLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        nameLabel.setColour (juce::Label::textColourId, juce::Colour (0xff6fb7c9));
        nameLabel.setEditable (false, true, false);
        nameLabel.onTextChange = [this]
        {
            if (nameLabel.getText().isNotEmpty())
            {
                undoManager.beginNewTransaction();
                ret.setName (nameLabel.getText(), &undoManager);
            }
        };

        dbLabel.setJustificationType (juce::Justification::centred);
        dbLabel.setFont (juce::FontOptions (12.0f));
        dbLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb8b8c0));

        volumeSlider.setSliderStyle (juce::Slider::LinearVertical);
        volumeSlider.setRange (0.0, 1.0);
        volumeSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volumeSlider.setDoubleClickReturnValue (true, te::decibelsToVolumeFaderPosition (0.0f));
        volumeSlider.onDragStart = [this] { undoManager.beginNewTransaction(); };
        volumeSlider.onValueChange = [this]
        {
            if (isRefreshing)
                return;

            ret.setVolumeDb (te::volumeFaderPositionToDB ((float) volumeSlider.getValue()),
                             &undoManager);
        };

        muteButton.setClickingTogglesState (true);
        muteButton.onClick = [this]
        {
            if (isRefreshing)
                return;

            undoManager.beginNewTransaction();
            ret.setMuted (muteButton.getToggleState(), &undoManager);
        };

        removeButton.onClick = [this] { if (onRemove) onRemove(); };

        effectSlots.onPreferredHeightChanged = [this] { resized(); };
        effectViewport.setViewedComponent (&effectSlots, false);
        effectViewport.setScrollBarsShown (true, false);
        effectViewport.setScrollBarThickness (6);

        for (auto* c : std::initializer_list<juce::Component*> {
                 &nameLabel, &effectViewport, &volumeSlider, &dbLabel,
                 &muteButton, &removeButton })
            addAndMakeVisible (c);

        refresh();
    }

    juce::String getReturnId() const  { return ret.getId(); }
    EffectSlotList& getEffectSlots()  { return effectSlots; }

    std::function<void()> onRemove;

    void refresh()
    {
        const juce::ScopedValueSetter<bool> svs (isRefreshing, true);

        nameLabel.setText (ret.getName(), juce::dontSendNotification);

        if (! volumeSlider.isMouseButtonDown())
            volumeSlider.setValue (te::decibelsToVolumeFaderPosition (ret.getVolumeDb()),
                                   juce::dontSendNotification);

        dbLabel.setText (formatDb (ret.getVolumeDb()) + " dB", juce::dontSendNotification);
        muteButton.setToggleState (ret.isMuted(), juce::dontSendNotification);
        effectSlots.refreshFromChain();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff26262c));
        g.setColour (juce::Colour (0xff3a3a40));
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (4, 4);
        nameLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);

        auto buttons = area.removeFromBottom (22);
        muteButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 - 2));
        buttons.removeFromLeft (4);
        removeButton.setBounds (buttons);

        dbLabel.setBounds (area.removeFromBottom (16));
        area.removeFromBottom (4);

        const auto slotHeight = juce::jmin (area.getHeight() - 110,
                                            effectSlots.getPreferredHeight());
        if (slotHeight > 0)
        {
            auto slotArea = area.removeFromTop (slotHeight);
            effectViewport.setBounds (slotArea);
            effectSlots.setSize (slotArea.getWidth() - (effectSlots.getPreferredHeight() > slotHeight ? 6 : 0),
                                 effectSlots.getPreferredHeight());
            area.removeFromTop (4);
        }

        volumeSlider.setBounds (area);
    }

private:
    model::Return ret;
    juce::UndoManager& undoManager;

    juce::Label nameLabel, dbLabel;
    juce::Slider volumeSlider;
    juce::TextButton muteButton { "M" }, removeButton { "X" };
    juce::Viewport effectViewport;
    EffectSlotList effectSlots;
    bool isRefreshing = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReturnStrip)
};

//==============================================================================
MixerComponent::MixerComponent (te::Edit& editToShow, juce::UndoManager& um)
    : edit (editToShow), undoManager (um)
{
    addReturnButton.onClick = [this]
    {
        undoManager.beginNewTransaction();
        song.addReturn ("Return " + juce::String ((int) song.getReturns().size() + 1),
                        &undoManager);
    };
    addAndMakeVisible (addReturnButton);

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

    // A send level or return property arrives continuously while dragged;
    // update the affected strips in place, never rebuild mid-drag.
    if (tree.hasType (model::ids::SEND))
    {
        const auto generatorId = tree.getParent().getParent()[model::ids::id].toString();

        for (auto& strip : strips)
            if (strip->getGeneratorId() == generatorId)
                strip->refresh();

        return;
    }

    if (tree.hasType (model::ids::RETURN))
    {
        const auto returnId = tree[model::ids::id].toString();

        for (auto& strip : returnStrips)
            if (strip->getReturnId() == returnId)
                strip->refresh();

        // The send rows carry the return's name, and it is baked in at strip
        // construction. A rename is a discrete edit, never a drag, so a
        // rebuild is safe here.
        if (property == model::ids::name)
            triggerAsyncUpdate();

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
    // traffic that pattern editing generates. RETURNS changes rebuild too:
    // every generator strip carries one send row per return.
    if (parent.hasType (model::ids::GENERATORS) || parent.hasType (model::ids::RETURNS))
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

    // The master strip binds to the song's master bus at construction the way
    // a channel strip binds to its generator, so a new song -- and setSong
    // always follows construction -- needs a new strip. Building it against
    // the placeholder song left the fader writing into a tree EditSync never
    // saw, which is why it used to do nothing.
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

    strips.clear();

    for (const auto& generator : song.getGenerators())
    {
        auto strip = std::make_unique<ChannelStrip> (generator, edit.engine, undoManager,
                                                     song.getReturns());

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

        strip->onOpenGenerator = [this, generatorId]
        {
            if (onOpenGenerator)
                onOpenGenerator (generatorId);
        };

        addAndMakeVisible (*strip);
        strips.push_back (std::move (strip));
    }

    returnStrips.clear();

    for (const auto& ret : song.getReturns())
    {
        auto strip = std::make_unique<ReturnStrip> (edit.engine, ret, undoManager);
        const auto returnId = ret.getId();

        auto& slots = strip->getEffectSlots();
        slots.onOpenEffectEditor = [this, returnId] (const juce::String& effectId)
        {
            openEffectEditor (returnId, effectId);
        };
        slots.onEffectAboutToBeRemoved = [this] (const juce::String& effectId)
        {
            closeEffectWindow (effectId);
        };

        strip->onRemove = [this, returnId]
        {
            if (auto ret2 = song.findReturn (returnId))
            {
                undoManager.beginNewTransaction();
                song.removeReturn (*ret2, &undoManager);
            }
        };

        addAndMakeVisible (*strip);
        returnStrips.push_back (std::move (strip));
    }

    const auto previousWidth = getWidth();

    // Generator strips, then return strips, then the add button's column,
    // then the master -- always last.
    setSize (((int) strips.size() + (int) returnStrips.size()) * stripWidth
                 + masterGap + stripWidth,
             juce::jmax (minHeight, getHeight()));
    resized();

    if (getWidth() != previousWidth && onContentWidthChanged != nullptr)
        onContentWidthChanged();
}



void MixerComponent::openEffectEditor (const juce::String& generatorId, const juce::String& effectId)
{
    if (auto existing = effectWindows.find (effectId); existing != effectWindows.end())
    {
        existing->second.window->toFront (true);
        return;
    }

    // "generatorId" may name a return bus instead; both resolve to a track.
    auto* plugin = sync::findEffectPlugin (song, edit, generatorId, effectId);

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
        if (sync::findEffectPlugin (song, edit, open.generatorId, effectId) != open.plugin.get())
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

    const auto returnsLeft = (int) strips.size() * stripWidth;

    for (size_t i = 0; i < returnStrips.size(); ++i)
        returnStrips[i]->setBounds (returnsLeft + (int) i * stripWidth, 0, stripWidth, getHeight());

    // The add button lives in the gap that already separates the busses from
    // the master.
    addReturnButton.setBounds (returnsLeft + (int) returnStrips.size() * stripWidth + 2, 4,
                               masterGap - 6, 22);

    masterStrip->setBounds (getWidth() - stripWidth, 0, stripWidth, getHeight());
}

} // namespace carve::app
