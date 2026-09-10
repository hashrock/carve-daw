#pragma once

namespace carve::plugins
{

// Something that pulls the level down and can say by how much, so the effect
// editor can put a reduction meter under its sliders (see
// EffectParameterWindow's GainReductionMeter). The compressor and the limiter
// both do; the editor asks for this rather than for either of them by name.
//
// The value is read on the message thread and written on the audio thread once
// a block, so an implementation publishes it through an atomic.
struct GainReductionSource
{
    virtual ~GainReductionSource() = default;

    // Positive dB of reduction, 0 when it is doing nothing.
    virtual float getGainReductionDb() const noexcept = 0;
};

} // namespace carve::plugins
