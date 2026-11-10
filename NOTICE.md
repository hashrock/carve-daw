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

## Third-party notices

The permissive licences above ask for little, but they do ask that their
copyright notice and terms travel with every copy of the program, binaries
included. `scripts/release.sh` ships this file beside the app, so the texts are
reproduced here in full.

### WORLD

Used at commit c794856. Its FFT is Takuya Ooura's, wrapped by M. Morise; Ooura's
code may be used, copied, modified and distributed for any purpose.

```
Copyright (c) 2010  M. Morise

All rights reserved.

Redistribution and use in source and binary forms, with or
without modification, are permitted provided that the following
conditions are met:

- Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.
- Neither the name of the M. Morise nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
```

### TinySoundFont

Used at commit 853a0a1. The notice is the one in `tsf.h`, the file that is
compiled in.

```
Copyright (C) 2017-2025 Bernhard Schelling
Based on SFZero, Copyright (C) 2012 Steve Folta (https://github.com/stevefolta/SFZero)

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.
```

### Signalsmith Stretch

The phase vocoder behind the Pitch Shifter, with Signalsmith Linear for its
FFT. Both are under the same MIT text.
<https://github.com/Signalsmith-Audio/signalsmith-stretch>
<https://github.com/Signalsmith-Audio/linear>

```
MIT License

Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
Copyright (c) 2025 Signalsmith Audio (Signalsmith Linear)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
