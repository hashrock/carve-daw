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
<path d="M100 22L84.5777 77.0685H48.3387L46.0712 65.4625H44.0626L37.4125 77.0685H3V22L100 22ZM13.276 66.7925H31.4564L38.1065 55.1853H54.5348L56.8023 66.7925H76.7833L86.4492 32.276H13.276V66.7925Z" fill="black"/>
<path d="M18.717 57.7855V41.4643H37.7595L29.5984 57.7855H18.717Z" fill="black"/>
<path d="M59.5226 57.9668V41.6456H78.565L70.8574 57.9668H59.5226Z" fill="black"/>
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
