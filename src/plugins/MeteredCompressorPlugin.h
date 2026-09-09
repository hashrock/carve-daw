#pragma once

#include <atomic>

#include <tracktion_engine/tracktion_engine.h>

namespace te = tracktion;

namespace carve::plugins
{

// tracktion's compressor with a gain-reduction reading.
//
// CompressorPlugin keeps its detector private and publishes nothing about
// what it is doing, and the plugin registry will not take a second type under
// the name "compressor". So this is the same plugin under its own type name,
// and the reduction is measured rather than read: the energy of the block
// going in against the energy coming out, with the makeup gain taken back
// off. Within a block that is the power-weighted mean of the gain the
// compressor applied, which is what a meter wants to show anyway.
//
// EditSync builds one of these wherever the model says "compressor", so the
// .carve keeps tracktion's name and the sidechain machinery, which works on
// the base class, is untouched.
class MeteredCompressorPlugin : public te::CompressorPlugin
{
public:
    explicit MeteredCompressorPlugin (te::PluginCreationInfo);
    ~MeteredCompressorPlugin() override;

    static const char* xmlTypeName;

    juce::String getPluginType() override  { return xmlTypeName; }

    void applyToBuffer (const te::PluginRenderContext&) override;

    // Negative or zero, in dB; zero while there is nothing to compress.
    float getGainReductionDb() const noexcept  { return gainReductionDb.load (std::memory_order_relaxed); }

private:
    std::atomic<float> gainReductionDb { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeteredCompressorPlugin)
};

} // namespace carve::plugins
