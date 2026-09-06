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

    juce::StringArray getVoiceCountNames()
    {
        juce::StringArray names;

        // MultiVoiceOscillator tops out at 8 unison voices.
        for (int i = 1; i <= 8; ++i)
            names.add (juce::String (i));

        return names;
    }

    juce::Array<int> getVoiceCountValues()
    {
        juce::Array<int> values;

        for (int i = 1; i <= 8; ++i)
            values.add (i);

        return values;
    }

    // Names inside a titled box do not need to repeat what the box says, so the
    // rows here are labelled by hand rather than from getParameterName(), which
    // would give "Tune 1" inside a section already called "Osc 1".
    constexpr int nameColumnWidth = 80;
} // namespace

//==============================================================================
EditorSection& EditorSectionPage::add (std::unique_ptr<EditorSection> section)
{
    auto& added = *section;
    addAndMakeVisible (added);
    sections.push_back (std::move (section));
    return added;
}

void EditorSectionPage::refresh()
{
    for (auto& section : sections)
        section->refresh();
}

int EditorSectionPage::getPreferredHeight() const
{
    int total = 8;

    for (size_t i = 0; i < sections.size(); i += (size_t) columns)
    {
        int rowHeight = 0;

        for (size_t j = i; j < sections.size() && j < i + (size_t) columns; ++j)
            rowHeight = juce::jmax (rowHeight, sections[j]->getPreferredHeight());

        total += rowHeight + 4;
    }

    return total;
}

void EditorSectionPage::resized()
{
    auto area = getLocalBounds().reduced (4);

    for (size_t i = 0; i < sections.size(); i += (size_t) columns)
    {
        int rowHeight = 0;

        for (size_t j = i; j < sections.size() && j < i + (size_t) columns; ++j)
            rowHeight = juce::jmax (rowHeight, sections[j]->getPreferredHeight());

        auto rowArea = area.removeFromTop (rowHeight + 4);
        const int columnWidth = rowArea.getWidth() / columns;

        for (size_t j = i; j < sections.size() && j < i + (size_t) columns; ++j)
            sections[j]->setBounds (rowArea.removeFromLeft (columnWidth).reduced (2));
    }
}

//==============================================================================
FourOscEditor::FourOscEditor (te::FourOscPlugin& synth)
    : plugin (synth), presetBar (synth)
{
    // The plugin can be deleted under an open window -- EditSync rebuilds the
    // track when a generator changes, and the window is only closed afterwards.
    auto isAlive = [this] { return plugin != nullptr; };
    auto state = synth.state;

    // Osc, filter and amp on one page. They are the signal path -- what the
    // wave is, what the filter does to it, how it is shaped in time -- and
    // dialling in a sound means moving between all three, which paging back
    // and forth turned into three separate views of one instrument. Mod and FX
    // stay their own pages: they are things done *to* a sound that already
    // works.
    {
        auto& page = addPage ("Main", 3);

        // Two oscillators, not four. Four made a page you had to hunt through
        // for the two anyone actually reaches for, and a second osc is already
        // enough for the detuned-pair and octave-stack sounds that are the
        // point of having more than one.
        for (int i = 1; i <= 2; ++i)
        {
            auto* osc = synth.oscParams[i - 1];

            if (osc == nullptr)
                continue;

            auto section = std::make_unique<EditorSection> ("Osc " + juce::String (i), nameColumnWidth);
            // The default values are the engine's own (FourOscPlugin's referTo
            // calls): a fresh synth has no properties in its tree yet, and
            // reading them raw showed osc 1 as "Off" while it played a sine.
            section->add (std::make_unique<PropertyChoiceRow> (state, indexedId (te::IDs::waveShape, i),
                                                               "Wave", oscWaveShapes, isAlive,
                                                               i == 1 ? 1 : 0));
            section->add (std::make_unique<PropertyChoiceRow> (state, indexedId (te::IDs::voices, i),
                                                               "Unison", getVoiceCountNames(),
                                                               getVoiceCountValues(), isAlive, 1));
            section->add (std::make_unique<ParameterSliderRow> (*osc->tune, "Tune", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->fineTune, "Fine", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->level, "Level", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->pulseWidth, "Pulse W", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->detune, "Detune", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->spread, "Spread", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*osc->pan, "Pan", isAlive));
            page.add (std::move (section));
        }

        auto filter = std::make_unique<EditorSection> ("Filter", nameColumnWidth);
        filter->add (std::make_unique<PropertyChoiceRow> (state, te::IDs::filterType, "Type",
                                                          filterTypes, isAlive));
        filter->add (std::make_unique<PropertyChoiceRow> (state, te::IDs::filterSlope, "Slope",
                                                          filterSlopes, filterSlopeValues, isAlive, 12));
        filter->add (std::make_unique<ParameterSliderRow> (*synth.filterFreq, "Freq", isAlive));
        filter->add (std::make_unique<ParameterSliderRow> (*synth.filterResonance, "Resonance", isAlive));
        filter->add (std::make_unique<ParameterSliderRow> (*synth.filterAmount, "Env Amount", isAlive));
        filter->add (std::make_unique<ParameterSliderRow> (*synth.filterKey, "Key Track", isAlive));
        filter->add (std::make_unique<ParameterSliderRow> (*synth.filterVelocity, "Velocity", isAlive));
        page.add (std::move (filter));

        auto envelope = std::make_unique<EditorSection> ("Filter Envelope", nameColumnWidth);
        envelope->add (std::make_unique<ParameterSliderRow> (*synth.filterAttack, "Attack", isAlive));
        envelope->add (std::make_unique<ParameterSliderRow> (*synth.filterDecay, "Decay", isAlive));
        envelope->add (std::make_unique<ParameterSliderRow> (*synth.filterSustain, "Sustain", isAlive));
        envelope->add (std::make_unique<ParameterSliderRow> (*synth.filterRelease, "Release", isAlive));
        page.add (std::move (envelope));

        auto amp = std::make_unique<EditorSection> ("Amp Envelope", nameColumnWidth);
        amp->add (std::make_unique<ParameterSliderRow> (*synth.ampAttack, "Attack", isAlive));
        amp->add (std::make_unique<ParameterSliderRow> (*synth.ampDecay, "Decay", isAlive));
        amp->add (std::make_unique<ParameterSliderRow> (*synth.ampSustain, "Sustain", isAlive));
        amp->add (std::make_unique<ParameterSliderRow> (*synth.ampRelease, "Release", isAlive));
        amp->add (std::make_unique<ParameterSliderRow> (*synth.ampVelocity, "Velocity", isAlive));
        amp->add (std::make_unique<PropertyToggleRow> (state, te::IDs::ampAnalog, "Analog", isAlive,
                                                       true));
        page.add (std::move (amp));

        auto voice = std::make_unique<EditorSection> ("Voice", nameColumnWidth);
        voice->add (std::make_unique<PropertyChoiceRow> (state, te::IDs::voiceMode, "Mode",
                                                         voiceModes, isAlive, 2));
        voice->add (std::make_unique<PropertySliderRow> (state, te::IDs::voices, "Polyphony",
                                                         1.0, 32.0, 1.0, juce::String(), isAlive, 32.0));
        voice->add (std::make_unique<ParameterSliderRow> (*synth.legato, "Glide", isAlive));
        voice->add (std::make_unique<ParameterSliderRow> (*synth.masterLevel, "Level", isAlive));
        page.add (std::move (voice));
    }

    {
        auto& page = addPage ("Mod", 2);

        for (int i = 1; i <= 2; ++i)
        {
            auto* lfo = synth.lfoParams[i - 1];

            if (lfo == nullptr)
                continue;

            auto section = std::make_unique<EditorSection> ("LFO " + juce::String (i), nameColumnWidth);
            section->add (std::make_unique<PropertyChoiceRow> (state, indexedId (te::IDs::lfoWaveShape, i),
                                                               "Wave", lfoWaveShapes, isAlive,
                                                               i == 1 ? 1 : 0));
            section->add (std::make_unique<PropertyToggleRow> (state, indexedId (te::IDs::lfoSync, i),
                                                               "Sync", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*lfo->rate, "Rate", isAlive));
            // Only used while Sync is on, where the rate comes from the tempo.
            section->add (std::make_unique<PropertySliderRow> (state, indexedId (te::IDs::lfoBeat, i),
                                                               "Beats", 0.25, 8.0, 0.25, juce::String(),
                                                               isAlive, 1.0));
            section->add (std::make_unique<ParameterSliderRow> (*lfo->depth, "Depth", isAlive));
            page.add (std::move (section));
        }

        for (int i = 1; i <= 2; ++i)
        {
            auto* env = synth.modEnvParams[i - 1];

            if (env == nullptr)
                continue;

            auto section = std::make_unique<EditorSection> ("Mod Env " + juce::String (i), nameColumnWidth);
            section->add (std::make_unique<ParameterSliderRow> (*env->modAttack, "Attack", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*env->modDecay, "Decay", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*env->modSustain, "Sustain", isAlive));
            section->add (std::make_unique<ParameterSliderRow> (*env->modRelease, "Release", isAlive));
            page.add (std::move (section));
        }
    }

    {
        auto& page = addPage ("FX", 2);

        auto distortion = std::make_unique<EditorSection> ("Distortion", nameColumnWidth);
        distortion->add (std::make_unique<PropertyToggleRow> (state, te::IDs::distortionOn, "On", isAlive));
        distortion->add (std::make_unique<ParameterSliderRow> (*synth.distortion, "Amount", isAlive));
        page.add (std::move (distortion));

        auto reverb = std::make_unique<EditorSection> ("Reverb", nameColumnWidth);
        reverb->add (std::make_unique<PropertyToggleRow> (state, te::IDs::reverbOn, "On", isAlive));
        reverb->add (std::make_unique<ParameterSliderRow> (*synth.reverbSize, "Size", isAlive));
        reverb->add (std::make_unique<ParameterSliderRow> (*synth.reverbDamping, "Damping", isAlive));
        reverb->add (std::make_unique<ParameterSliderRow> (*synth.reverbWidth, "Width", isAlive));
        reverb->add (std::make_unique<ParameterSliderRow> (*synth.reverbMix, "Mix", isAlive));
        page.add (std::move (reverb));

        auto delay = std::make_unique<EditorSection> ("Delay", nameColumnWidth);
        delay->add (std::make_unique<PropertyToggleRow> (state, te::IDs::delayOn, "On", isAlive));
        // The delay time is in beats, and the only one of its settings that is
        // not an automatable parameter.
        delay->add (std::make_unique<PropertySliderRow> (state, te::IDs::delay, "Beats",
                                                         0.125, 4.0, 0.125, juce::String(), isAlive, 1.0));
        delay->add (std::make_unique<ParameterSliderRow> (*synth.delayFeedback, "Feedback", isAlive));
        delay->add (std::make_unique<ParameterSliderRow> (*synth.delayCrossfeed, "Crossfeed", isAlive));
        delay->add (std::make_unique<ParameterSliderRow> (*synth.delayMix, "Mix", isAlive));
        page.add (std::move (delay));

        auto chorus = std::make_unique<EditorSection> ("Chorus", nameColumnWidth);
        chorus->add (std::make_unique<PropertyToggleRow> (state, te::IDs::chorusOn, "On", isAlive));
        chorus->add (std::make_unique<ParameterSliderRow> (*synth.chorusSpeed, "Speed", isAlive));
        chorus->add (std::make_unique<ParameterSliderRow> (*synth.chorusDepth, "Depth", isAlive));
        chorus->add (std::make_unique<ParameterSliderRow> (*synth.chorusWidth, "Width", isAlive));
        chorus->add (std::make_unique<ParameterSliderRow> (*synth.chorusMix, "Mix", isAlive));
        page.add (std::move (chorus));
    }

    tabs.setOutline (0);
    tabs.setTabBarDepth (24);
    addAndMakeVisible (presetBar);
    addAndMakeVisible (tabs);

    setSize (width, height);
    startTimerHz (15);
}

EditorSectionPage& FourOscEditor::addPage (const juce::String& name, int columns)
{
    auto page = std::make_unique<EditorSectionPage> (columns);
    auto* raw = page.get();

    tabs.addTab (name, juce::Colour (0xff232327), page.release(), true);
    pages.push_back (raw);

    return *raw;
}

void FourOscEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff232327));
}

void FourOscEditor::resized()
{
    auto area = getLocalBounds();
    presetBar.setBounds (area.removeFromTop (PresetBar::height));
    tabs.setBounds (area);
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

    // Only the visible page: the rest will catch up when it is shown, and
    // polling five pages of sliders 15 times a second is work for nothing.
    if (const int index = tabs.getCurrentTabIndex(); juce::isPositiveAndBelow (index, (int) pages.size()))
        pages[(size_t) index]->refresh();
}

} // namespace carve::app
