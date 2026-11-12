#include "SandboxAccess.h"

// Everywhere but macOS there is no App Sandbox to keep access across; the
// real thing is SandboxAccess.mm.

namespace carve::sandbox
{

bool isSandboxed()                        { return false; }
void restoreAccess (const juce::File&)    {}
void rememberAccess (const juce::File&)   {}

juce::File claimChosen (juce::URL url)
{
    return url.isLocalFile() ? url.getLocalFile() : juce::File();
}

} // namespace carve::sandbox
