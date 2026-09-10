#include "SlateEditor.h"

namespace carve::app
{

namespace
{
    // 4OSC numbers its oscillators, LFOs and mod envelopes from 1, and stores
    // each one's settings under a property whose name carries that number --
    // "waveShape1", "lfoRate2". The plugin builds those names the same way.
    juce::Identifier indexedId (const juce::Identifier& base, int index)
    {
        return juce::Identifier (base.toString() + juce::String (index));
    }

    // Oscillator::Waves, in the order the plugin casts the property to.
    const juce::StringArray oscWaveShapes { "Off", "Sine", "Square", "Saw", "Triangle", "Noise" };

    // SimpleLFO::WaveShape -- a different set, and a different order.
    const juce::StringArray lfoWaveShapes { "Off", "Sine", "Triangle", "Saw Up", "Saw Down",
                                            "Square", "Random" };

    const juce::StringArray filterTypes { "Off", "Low Pass", "High Pass", "Band Pass", "Notch" };
    const juce::StringArray voiceModes { "Mono", "Legato", "Poly" };

    // The slope is stored as the number of dB per octave rather than as an index.
    const juce::StringArray filterSlopes { "12 dB", "24 dB" };
    const juce::Array<int> filterSlopeValues { 12, 24 };

    using S = KnobSize;

    // Names inside a titled box do not need to repeat what the box says, so
    // the knobs here are labelled by hand rather than from getParameterName(),
    // which would give "Tune 1" inside a section already called "OSC 1".
    std::unique_ptr<Knob> knob (te::AutomatableParameter* parameter, const juce::String& name,
                                KnobSize size, std::function<bool()> isAlive)
    {
        jassert (parameter != nullptr);
        return std::make_unique<ParameterKnob> (*parameter, name, size, std::move (isAlive));
    }

    //==============================================================================
    // Where LFO 1 goes.
    //
    // 4OSC has no wiring of its own between a modulator and a parameter: every
    // route lives in a matrix, kept in the plugin's state as a MODMATRIX child
    // holding one MODMATRIXITEM per (parameter, source) pair. Nothing in this
    // editor showed that matrix, which meant the LFO knobs moved a modulator
    // that reached nothing -- so this box writes the matrix instead of showing
    // it, one destination at a time. The plugin reloads it on the spot: every
    // change to that subtree triggers its own loadModMatrix.
    //
    // Depths are in the destination parameter's own 0..1 range, and are chosen
    // so that the Depth knob all the way up is still musical rather than
    // absurd. They are per destination because "as far as it goes" means a
    // different amount for a filter than for a tuning.
    struct LfoDestination
    {
        const char* name;
        juce::StringArray parameterIDs;
        float depth;
    };

    const std::vector<LfoDestination> lfoDestinations
    {
        { "Off",    {},                                   0.0f  },
        // Fine tune, not tune: the plugin rounds Tune to whole semitones, so
        // vibrato through it would step rather than glide. +-50 cents here.
        { "Pitch",  { "fineTune1", "fineTune2" },          0.25f },
        // The cutoff is stored as a note number over a 135-semitone range, so
        // a quarter of it is a little under three octaves of sweep.
        { "Filter", { "filterFreq" },                      0.25f },
        { "Width",  { "pulseWidth1", "pulseWidth2" },      0.35f },
    };

    const juce::String lfo1SourceID { "lfo1" };

    class LfoDestinationBox : public juce::ComboBox,
                              public HeaderControl
    {
    public:
        LfoDestinationBox (juce::ValueTree stateToEdit, std::function<bool()> isAliveCheck)
            : state (std::move (stateToEdit)), isAlive (std::move (isAliveCheck))
        {
            for (const auto& destination : lfoDestinations)
                addItem (destination.name, getNumItems() + 1);

            setWantsKeyboardFocus (false);
            setTooltip ("What LFO 1 moves");

            onChange = [this]
            {
                if (isRefreshing || ! isAlive())
                    return;

                apply (getSelectedItemIndex());
            };

            refresh();
        }

        juce::Component& getComponent() override  { return *this; }
        int getPreferredWidth() const override    { return 78; }

        void refresh() override
        {
            if (! isAlive() || isPopupActive())
                return;

            const juce::ScopedValueSetter<bool> svs (isRefreshing, true);
            setSelectedItemIndex (findCurrentDestination(), juce::dontSendNotification);
        }

    private:
        // The first destination whose parameters the matrix already points
        // LFO 1 at, or "Off" if it points at nothing this box knows about --
        // a preset built elsewhere can hold routes with no name here.
        int findCurrentDestination() const
        {
            const auto matrix = state.getChildWithName (te::IDs::MODMATRIX);

            if (! matrix.isValid())
                return 0;

            for (int i = 1; i < (int) lfoDestinations.size(); ++i)
            {
                for (const auto& item : matrix)
                {
                    if (item[te::IDs::modItem].toString() != lfo1SourceID)
                        continue;

                    if (lfoDestinations[(size_t) i].parameterIDs.contains (item[te::IDs::modParam].toString()))
                        return i;
                }
            }

            return 0;
        }

        void apply (int index)
        {
            if (! juce::isPositiveAndBelow (index, (int) lfoDestinations.size()))
                return;

            auto matrix = state.getOrCreateChildWithName (te::IDs::MODMATRIX, nullptr);

            // Whatever LFO 1 drove before goes first, so switching destination
            // moves the LFO rather than adding a second thing for it to do.
            for (int i = matrix.getNumChildren(); --i >= 0;)
                if (matrix.getChild (i)[te::IDs::modItem].toString() == lfo1SourceID)
                    matrix.removeChild (i, nullptr);

            const auto& destination = lfoDestinations[(size_t) index];

            for (const auto& parameterID : destination.parameterIDs)
            {
                juce::ValueTree item (te::IDs::MODMATRIXITEM);
                item.setProperty (te::IDs::modParam, parameterID, nullptr);
                item.setProperty (te::IDs::modItem, lfo1SourceID, nullptr);
                item.setProperty (te::IDs::modDepth, destination.depth, nullptr);
                matrix.appendChild (item, nullptr);
            }
        }

        juce::ValueTree state;
        std::function<bool()> isAlive;
        bool isRefreshing = false;
    };
} // namespace

//==============================================================================
SlateEditor::SlateEditor (te::FourOscPlugin& synth)
    : plugin (synth), presetBar (synth)
{
    // The plugin can be deleted under an open window -- EditSync rebuilds the
    // track when a generator changes, and the window is only closed afterwards.
    auto isAlive = [this] { return plugin != nullptr; };
    auto state = synth.state;

    // The signal path.
    for (int i = 1; i <= 2; ++i)
    {
        auto* osc = synth.oscParams[i - 1];

        if (osc == nullptr)
            continue;

        auto section = std::make_unique<KnobSection> ("OSC " + juce::String (i));
        // The default values are the engine's own (FourOscPlugin's referTo
        // calls): a fresh synth has no properties in its tree yet, and
        // reading them raw showed osc 1 as "Off" while it played a sine.
        section->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, indexedId (te::IDs::waveShape, i),
                                                                        oscWaveShapes, isAlive, i == 1 ? 1 : 0));
        // Level first: the mix between the two oscillators is the sound.
        section->addKnob (knob (osc->level, "Level", S::large, isAlive));
        section->addKnob (knob (osc->tune, "Tune", S::medium, isAlive));
        section->addKnob (knob (osc->fineTune, "Fine", S::medium, isAlive));
        section->newRow();
        // Unison is a count, stored as a property; a stepped knob reads it
        // better than a menu of "1" to "8".
        section->addKnob (std::make_unique<PropertyKnob> (state, indexedId (te::IDs::voices, i), "Unison",
                                                          S::small, 1.0, 8.0, 1.0, juce::String(), isAlive, 1.0));
        section->addKnob (knob (osc->detune, "Detune", S::small, isAlive));
        section->addKnob (knob (osc->spread, "Spread", S::small, isAlive));
        section->newRow();
        section->addKnob (knob (osc->pulseWidth, "Pulse W", S::small, isAlive));
        section->addKnob (knob (osc->pan, "Pan", S::small, isAlive));
        page.add (std::move (section));
    }

    {
        auto filter = std::make_unique<KnobSection> ("FILTER");
        filter->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, te::IDs::filterType, filterTypes, isAlive));
        filter->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, te::IDs::filterSlope, filterSlopes,
                                                                       filterSlopeValues, isAlive, 12));
        filter->addKnob (knob (synth.filterFreq, "Cutoff", S::large, isAlive));
        filter->addKnob (knob (synth.filterResonance, "Reso", S::medium, isAlive));
        filter->addKnob (knob (synth.filterAmount, "Env Amt", S::medium, isAlive));
        filter->newRow();
        filter->addKnob (knob (synth.filterKey, "Key Trk", S::small, isAlive));
        filter->addKnob (knob (synth.filterVelocity, "Velocity", S::small, isAlive));
        filter->newRow();
        // The filter's own envelope, small: it only does anything through
        // Env Amt, which is the knob to reach for first.
        filter->addKnob (knob (synth.filterAttack, "Env A", S::small, isAlive));
        filter->addKnob (knob (synth.filterDecay, "Env D", S::small, isAlive));
        filter->addKnob (knob (synth.filterSustain, "Env S", S::small, isAlive));
        filter->addKnob (knob (synth.filterRelease, "Env R", S::small, isAlive));
        page.add (std::move (filter));

        auto amp = std::make_unique<KnobSection> ("AMP");
        amp->addHeaderControl (std::make_unique<PropertyToggleButton> (state, te::IDs::ampAnalog, "Analog", isAlive, true));
        amp->addKnob (knob (synth.ampAttack, "Attack", S::medium, isAlive));
        amp->addKnob (knob (synth.ampDecay, "Decay", S::medium, isAlive));
        amp->addKnob (knob (synth.ampSustain, "Sustain", S::medium, isAlive));
        amp->addKnob (knob (synth.ampRelease, "Release", S::medium, isAlive));
        amp->newRow();
        amp->addKnob (knob (synth.ampVelocity, "Velocity", S::small, isAlive));
        page.add (std::move (amp));

        // The master level, on its own and the biggest knob on the panel.
        auto out = std::make_unique<KnobSection> ("OUT");
        out->setCentred (true);
        out->addKnob (knob (synth.masterLevel, "Level", S::xlarge, isAlive));
        page.add (std::move (out));
    }

    // How voices are handled, and the one modulator: things done *to* a sound
    // that already works.
    page.newRow();
    {
        auto voice = std::make_unique<KnobSection> ("VOICE");
        voice->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, te::IDs::voiceMode, voiceModes, isAlive, 2));
        voice->addKnob (std::make_unique<PropertyKnob> (state, te::IDs::voices, "Poly", S::small,
                                                        1.0, 32.0, 1.0, juce::String(), isAlive, 32.0));
        voice->addKnob (knob (synth.legato, "Glide", S::small, isAlive));
        page.add (std::move (voice));

        if (auto* lfo = synth.lfoParams[0])
        {
            auto section = std::make_unique<KnobSection> ("LFO");
            section->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, indexedId (te::IDs::lfoWaveShape, 1),
                                                                            lfoWaveShapes, isAlive, 1));
            // Where it goes, without which none of the rest of this section
            // does anything at all -- see LfoDestinationBox.
            section->addHeaderControl (std::make_unique<LfoDestinationBox> (state, isAlive));
            section->addHeaderControl (std::make_unique<PropertyToggleButton> (state, indexedId (te::IDs::lfoSync, 1),
                                                                               "Sync", isAlive));
            section->addKnob (knob (lfo->rate, "Rate", S::medium, isAlive));
            section->addKnob (knob (lfo->depth, "Depth", S::medium, isAlive));
            // Only used while Sync is on, where the rate comes from the tempo.
            section->addKnob (std::make_unique<PropertyKnob> (state, indexedId (te::IDs::lfoBeat, 1), "Beats",
                                                              S::small, 0.25, 8.0, 0.25, juce::String(),
                                                              isAlive, 1.0));
            page.add (std::move (section));
        }
    }

    addAndMakeVisible (presetBar);
    addAndMakeVisible (page);

    setSize (page.getPreferredWidth(), PresetBar::height + page.getPreferredHeight());
    startTimerHz (15);
}

void SlateEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));
}

void SlateEditor::resized()
{
    auto area = getLocalBounds();
    presetBar.setBounds (area.removeFromTop (PresetBar::height));
    page.setBounds (area);
}

void SlateEditor::timerCallback()
{
    if (plugin == nullptr)
    {
        // EditSync deleted the plugin under us; the window is about to be
        // closed, so stop touching anything until it is.
        setEnabled (false);
        stopTimer();
        return;
    }

    page.refresh();
}

} // namespace carve::app
