#include "FourOscEditor.h"

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
} // namespace

//==============================================================================
FourOscEditor::FourOscEditor (te::FourOscPlugin& synth)
    : plugin (synth), presetBar (synth)
{
    // The plugin can be deleted under an open window -- EditSync rebuilds the
    // track when a generator changes, and the window is only closed afterwards.
    auto isAlive = [this] { return plugin != nullptr; };
    auto state = synth.state;

    // The signal path. Two oscillators, not four: four made a page you had to
    // hunt through for the two anyone actually reaches for, and a second osc
    // is already enough for the detuned-pair and octave-stack sounds that are
    // the point of having more than one.
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

    // Modulation, and how voices are handled: things done *to* a sound that
    // already works.
    page.newRow();
    {
        auto voice = std::make_unique<KnobSection> ("VOICE");
        voice->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, te::IDs::voiceMode, voiceModes, isAlive, 2));
        voice->addKnob (std::make_unique<PropertyKnob> (state, te::IDs::voices, "Poly", S::small,
                                                        1.0, 32.0, 1.0, juce::String(), isAlive, 32.0));
        voice->addKnob (knob (synth.legato, "Glide", S::small, isAlive));
        page.add (std::move (voice));

        for (int i = 1; i <= 2; ++i)
        {
            auto* lfo = synth.lfoParams[i - 1];

            if (lfo == nullptr)
                continue;

            auto section = std::make_unique<KnobSection> ("LFO " + juce::String (i));
            section->addHeaderControl (std::make_unique<PropertyChoiceBox> (state, indexedId (te::IDs::lfoWaveShape, i),
                                                                            lfoWaveShapes, isAlive, i == 1 ? 1 : 0));
            section->addHeaderControl (std::make_unique<PropertyToggleButton> (state, indexedId (te::IDs::lfoSync, i),
                                                                               "Sync", isAlive));
            section->addKnob (knob (lfo->rate, "Rate", S::medium, isAlive));
            section->addKnob (knob (lfo->depth, "Depth", S::medium, isAlive));
            // Only used while Sync is on, where the rate comes from the tempo.
            section->addKnob (std::make_unique<PropertyKnob> (state, indexedId (te::IDs::lfoBeat, i), "Beats",
                                                              S::small, 0.25, 8.0, 0.25, juce::String(),
                                                              isAlive, 1.0));
            page.add (std::move (section));
        }

        for (int i = 1; i <= 2; ++i)
        {
            auto* env = synth.modEnvParams[i - 1];

            if (env == nullptr)
                continue;

            auto section = std::make_unique<KnobSection> ("ENV " + juce::String (i));
            // A D S R: at this size the full names are wider than the knobs.
            section->addKnob (knob (env->modAttack, "A", S::small, isAlive));
            section->addKnob (knob (env->modDecay, "D", S::small, isAlive));
            section->addKnob (knob (env->modSustain, "S", S::small, isAlive));
            section->addKnob (knob (env->modRelease, "R", S::small, isAlive));
            page.add (std::move (section));
        }
    }

    // The effects. Each has its switch in the title band and its mix as the
    // one medium knob.
    page.newRow();
    {
        auto distortion = std::make_unique<KnobSection> ("DIST");
        distortion->addHeaderControl (std::make_unique<PropertyToggleButton> (state, te::IDs::distortionOn, "On", isAlive));
        distortion->addKnob (knob (synth.distortion, "Amount", S::medium, isAlive));
        page.add (std::move (distortion));

        auto chorus = std::make_unique<KnobSection> ("CHORUS");
        chorus->addHeaderControl (std::make_unique<PropertyToggleButton> (state, te::IDs::chorusOn, "On", isAlive));
        chorus->addKnob (knob (synth.chorusSpeed, "Speed", S::small, isAlive));
        chorus->addKnob (knob (synth.chorusDepth, "Depth", S::small, isAlive));
        chorus->addKnob (knob (synth.chorusWidth, "Width", S::small, isAlive));
        chorus->addKnob (knob (synth.chorusMix, "Mix", S::medium, isAlive));
        page.add (std::move (chorus));

        auto delay = std::make_unique<KnobSection> ("DELAY");
        delay->addHeaderControl (std::make_unique<PropertyToggleButton> (state, te::IDs::delayOn, "On", isAlive));
        // The delay time is in beats, and the only one of its settings that is
        // not an automatable parameter.
        delay->addKnob (std::make_unique<PropertyKnob> (state, te::IDs::delay, "Beats", S::small,
                                                        0.125, 4.0, 0.125, juce::String(), isAlive, 1.0));
        delay->addKnob (knob (synth.delayFeedback, "Feedback", S::small, isAlive));
        delay->addKnob (knob (synth.delayCrossfeed, "Crossfeed", S::small, isAlive));
        delay->addKnob (knob (synth.delayMix, "Mix", S::medium, isAlive));
        page.add (std::move (delay));

        auto reverb = std::make_unique<KnobSection> ("REVERB");
        reverb->addHeaderControl (std::make_unique<PropertyToggleButton> (state, te::IDs::reverbOn, "On", isAlive));
        reverb->addKnob (knob (synth.reverbSize, "Size", S::small, isAlive));
        reverb->addKnob (knob (synth.reverbDamping, "Damping", S::small, isAlive));
        reverb->addKnob (knob (synth.reverbWidth, "Width", S::small, isAlive));
        reverb->addKnob (knob (synth.reverbMix, "Mix", S::medium, isAlive));
        page.add (std::move (reverb));
    }

    addAndMakeVisible (presetBar);
    addAndMakeVisible (page);

    setSize (page.getPreferredWidth(), PresetBar::height + page.getPreferredHeight());
    startTimerHz (15);
}

void FourOscEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));
}

void FourOscEditor::resized()
{
    auto area = getLocalBounds();
    presetBar.setBounds (area.removeFromTop (PresetBar::height));
    page.setBounds (area);
}

void FourOscEditor::timerCallback()
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
