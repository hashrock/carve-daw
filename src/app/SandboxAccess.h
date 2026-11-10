#pragma once

#include <juce_core/juce_core.h>

namespace carve::sandbox
{

// Keeping hold of the user's files across launches, inside the App Sandbox.
//
// A sandboxed app (the Mac App Store build) can read and write its own
// container and, beyond that, only what the user hands it: a file picked in
// an open or save panel, or dropped on a window. That grant lasts until the
// app quits. A song, though, is a list of paths to samples chosen days ago,
// the recent-songs menu is a list of paths, and so are the sample browser's
// favourite folders -- and on the next launch every one of them would be a
// file the app is no longer allowed to see.
//
// macOS's answer is the security-scoped bookmark: while the app does have
// access to a file it can ask for a token that buys the same access back
// later. So everything the user gives the app is remembered here as it
// arrives, and all of it is claimed back at startup, before anything looks
// at a path. The rest of the app goes on using plain juce::File and never
// knows.
//
// Outside the sandbox -- the Developer ID build, any other platform -- every
// function here does nothing.

// Whether this process is sandboxed at all.
bool isSandboxed();

// Claims back access to everything remembered. Call once, before the first
// window: the recent songs and the browser's folders are read as they are
// built. `store` is where the bookmarks are kept; it has to be somewhere the
// app can always write, which inside the sandbox means its container.
void restoreAccess (const juce::File& store);

// Remembers a file or a folder the app currently has access to, so that it
// still has it next time. A folder covers everything inside it. Cheap to
// call for something already covered, and harmless for something the app
// has no access to (nothing is stored).
void rememberAccess (const juce::File&);

} // namespace carve::sandbox
