#pragma once

#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace carve::app
{

// The Carve mark, as the SVG it is drawn from (carve.svg at the repository
// root). Small enough to carry here as text rather than through a binary
// resource step, and recoloured on load: the source is black on nothing, the
// app is light on dark.
inline const char* const carveLogoSvg = R"svg(
<svg width="100" height="100" viewBox="0 0 100 100" fill="none" xmlns="http://www.w3.org/2000/svg">
<path d="M5 26H27.5H43L50.625 38L58.25 26H72.5H95V75H5V26Z" fill="black"/>
</svg>
)svg";

inline std::unique_ptr<juce::Drawable> createCarveLogo (juce::Colour colour)
{
    auto drawable = juce::Drawable::createFromImageData (carveLogoSvg, std::strlen (carveLogoSvg));

    if (drawable != nullptr)
        drawable->replaceColour (juce::Colours::black, colour);

    return drawable;
}

} // namespace carve::app
