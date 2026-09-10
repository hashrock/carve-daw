# Licensing

Carve DAW is licensed under the **GNU Affero General Public License, version 3
or later** (AGPL-3.0-or-later). The full text is in [LICENSE](LICENSE).

## Why AGPL, and not something more permissive

The choice is not really a choice: it follows from what the app is built on.

| Dependency | Version here | Licence |
| --- | --- | --- |
| [tracktion_engine](https://github.com/Tracktion/tracktion_engine) | 3.2.0 | GPL-3.0-or-later, or a commercial licence from Tracktion |
| [JUCE](https://juce.com) (inside tracktion_engine) | 8.0.6 | AGPL-3.0, or a commercial JUCE licence |
| [SoundTouch](https://www.surina.net/soundtouch/) (inside tracktion_engine) | bundled | LGPL-2.1-or-later |
| [VST3 SDK](https://steinbergmedia.github.io/vst3_dev_portal/) (inside JUCE, for hosting) | bundled | Steinberg's proprietary licence, or GPL-3.0 |
| AudioUnit SDK, FLAC, Ogg Vorbis, zlib, HarfBuzz, and the rest inside JUCE | bundled | Apache-2.0 / BSD / zlib / MIT |

No commercial licence has been bought for tracktion_engine or JUCE, so both are
used under their copyleft arms. JUCE's is the AGPL, which is the strictest of
them, and a work combining all of the above is distributed under the AGPL.

The network clause the AGPL is known for (section 13) is about software users
interact with over a network. Carve DAW is a desktop application and does not
talk to anyone, so in practice the obligation is the ordinary GPL one: whoever
receives the program can have the source it was built from, which is what this
repository is.

## Why the releases are not on the Mac App Store

The App Store's terms add restrictions -- DRM, a limit on the number of devices,
no redistribution -- that the AGPL and the GPL forbid adding: their whole point
is that whoever receives the program keeps the same freedoms. That conflict is
not about money, and giving the app away for free does not resolve it. VLC was
removed from the App Store over exactly this in 2011 and only returned after
being relicensed.

So the releases are notarised disk images, built by `scripts/release.sh` and
published on the GitHub releases page. They are signed with a Developer ID and
stapled, so macOS opens them without complaint; they simply do not go through
Apple's store.

Shipping on the App Store instead would mean buying commercial licences for both
tracktion_engine and JUCE -- and, separately from any licence, giving up VST3
hosting, which the App Store sandbox does not allow.
