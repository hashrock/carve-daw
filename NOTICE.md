# Licensing

Carve DAW is licensed under the **GNU Affero General Public License, version 3
or later** (AGPL-3.0-or-later). The full text is in [LICENSE](LICENSE).

## Why AGPL, and not something more permissive

The choice is not really a choice: it follows from what the app is built on.

| Dependency | Version here | Licence |
| --- | --- | --- |
| [tracktion_engine](https://github.com/Tracktion/tracktion_engine) | 3.2.0 | GPL-3.0-or-later, or a commercial licence from Tracktion |
| [JUCE](https://juce.com) (inside tracktion_engine) | 8.0.6 | AGPL-3.0, or a commercial JUCE licence |
| [VST3 SDK](https://steinbergmedia.github.io/vst3_dev_portal/) (inside JUCE, for hosting) | bundled | Steinberg's proprietary licence, or GPL-3.0 |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) (the SF2 player) | commit 853a0a1 | MIT |
| [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (the pitch shifter) | 1.4.0 | MIT |
| [WORLD](https://github.com/mmorise/World) (the UTAU singer's vocoder) | commit c794856 | modified BSD |
| AudioUnit SDK, FLAC, Ogg Vorbis, zlib, HarfBuzz, and the rest inside JUCE | bundled | Apache-2.0 / BSD / zlib / MIT |

The source tree and the GitHub Releases disk images use the copyleft arm of
both. JUCE's is the AGPL, which is the strictest of them, and that combined
work is distributed under the AGPL. A separate Mac App Store build uses the
commercial arms instead (JUCE 8 Starter and Tracktion Engine Personal). It is
not a relicensing of this repository. The caps, the required
"Powered by Tracktion Engine" line, and the CMake switch are in
[LICENSING.md](LICENSING.md). The text the app shows is
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

The network clause the AGPL is known for (section 13) is about software users
interact with over a network. Carve DAW is a desktop application and does not
talk to anyone, so in practice the obligation is the ordinary GPL one: whoever
receives the program can have the source it was built from, which is what this
repository is.

## The GitHub releases and the App Store build

The App Store's terms add restrictions -- DRM, a limit on the number of devices,
no redistribution -- that the AGPL and the GPL forbid adding: their whole point
is that whoever receives the program keeps the same freedoms. That conflict is
not about money, and giving the app away for free does not resolve it. VLC was
removed from the App Store over exactly this in 2011 and only returned after
being relicensed.

So the GitHub releases stay notarised disk images, built by `scripts/release.sh`
and published on the releases page. They are signed with a Developer ID and
stapled, and they stay under the AGPL. They do not go through Apple's store.

The App Store binary is a different compile (`-DCARVE_APP_STORE=ON`). It is
built under the commercial licences named above, it does not offer AGPL
redistribution of that binary, and it hosts no third-party plugins: the VST3
SDK is not linked, and Audio Unit hosting is switched off, because the App
Store sandbox does not load other vendors' code. Details, and the questions
that are still the owner's to answer, are in [LICENSING.md](LICENSING.md).
