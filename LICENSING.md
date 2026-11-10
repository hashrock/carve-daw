# Licensing

Carve DAW has two distributions. They are built from this same tree, and they
are not the same licence.

The **source**, and the **GitHub Releases** disk images produced by
`scripts/release.sh`, stay under the **GNU Affero General Public License,
version 3 or later**. That text is [LICENSE](LICENSE). Nothing in this
document changes it, and this document is not a relicensing.

The **Mac App Store** binary is a separate build. It is compiled with
`-DCARVE_APP_STORE=ON` and is offered under the commercial licences below,
plus Apple's App Store terms. It is not offered under the AGPL. Its About
window says so, and it does not tell the recipient they may redistribute that
binary under the AGPL.

| | GitHub source and Developer ID disk image | Mac App Store build |
| --- | --- | --- |
| Carve DAW | AGPL-3.0-or-later | Not AGPL. App Store terms, built with the commercial licences below |
| tracktion_engine 3.2.0 | GPL-3.0-or-later | Tracktion Engine Personal (free, individual) |
| JUCE 8.0.6 (inside tracktion_engine) | AGPL-3.0 | JUCE 8 Starter (free, individual) |
| Configure | default | `-DCARVE_APP_STORE=ON` |

JUCE arrives only as the submodule pin inside tracktion_engine 3.2.0, which is
JUCE 8.0.6. The Starter seat is the JUCE 8 licence described at
<https://juce.com/legal/juce-8-licence/>. This tree does not contain JUCE 9.
If a later engine pin moves JUCE forward, read that version's licence again
before shipping another App Store build.

## What has to be visible

### Tracktion Engine Personal

The Personal column of the licence chart at
<https://engine.tracktion.com/agreement> says the branding is **Powered by
Tracktion Engine**. Clause 1.8 requires that branding for Personal, Indie and
Education. The agreement does not state a minimum size, a colour, a clear
space, or a required file.

Tracktion staff (dave96, 10 June 2026,
<https://forum.juce.com/t/what-exactly-does-branding-mean/68947>) said the
words belong in the about or credits page, and the logo belongs on the
website where it can be seen. That is what this tree does:

- The app's About window (Apple menu) shows the words **Powered by Tracktion
  Engine**, and the lockup Tracktion publishes at
  `tutorials/images/tracktion_engine_powered.png` in tracktion_engine v3.2.0.
  The same window has **Third-Party Notices**.
- The website footer (`website/index.html`) shows that same lockup, with the
  alt text `Powered by Tracktion Engine`, linking to
  <https://www.tracktion.com/develop/tracktion-engine>.

No separate logo-usage guide was found on engine.tracktion.com or
tracktion.com. The lockup is copied here so the mark they publish is the mark
we show. Confirm with Tracktion if they want a different file.

The words are shown on the AGPL build as well as the App Store build. On the
AGPL build the About text still says the engine is used under GPL-3.0-or-later,
so the mark is attribution and not a claim that the Personal licence applies
to that binary.

### JUCE Starter

JUCE 8 removed the splash screen. In the 8.0.6 sources,
`JUCE_DISPLAY_SPLASH_SCREEN` and `JUCE_USE_DARK_SPLASH_SCREEN` are ignored,
and defining either one only produces a compiler warning
(`juce_gui_basics.cpp`). `JUCE_REPORT_APP_USAGE` is not a macro in this JUCE.
The build does not define them. There is no splash to turn off, and turning
the old flags on would not satisfy anything.

JUCE 8 Starter does not require a splash or a logo. The About window still
names JUCE, Raw Material Software Limited, <https://juce.com> and
<https://juce.com/legal/juce-8-licence/>, and
[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) carries the third-party
notices. That file is copied into `Carve DAW.app/Contents/Resources/` and is
what **Third-Party Notices** displays.

## Revenue limits, and what to do when one is crossed

The two caps are independent. Crossing either one means that binary cannot
keep shipping on that tier. Moving the App Store binary onto the AGPL (or
onto the GPL) does not make it an App Store binary: the store's terms are the
reason this split exists. The step that keeps the app on the store is to buy
the next commercial tier, or to stop distributing that build.

### JUCE 8 Starter — 20,000 USD a year

From <https://juce.com/legal/juce-8-licence/> (page read 2026-11-10):

- Starter is free and perpetual, for one user, up to **20,000 USD** annual
  revenue or funding, measured over the previous 12 months.
- For an individual (or an entity with no legal identity separate from that
  individual) the cap is revenue or funding **from use of the JUCE
  framework**, including donations, sponsorship, advertising and other
  indirect revenue from that use. It is not, on the wording of that clause,
  the person's unrelated income.
- For a company, the cap is all revenue or funding of the company and its
  affiliates, from any source.
- If the cap is crossed: buy a licence type that covers the new figure, or
  immediately stop developing and distributing products (clause 1.14). The
  next type is Indie (up to 300,000 USD; 40 USD a month with a one-month
  minimum, or 800 USD perpetual). Pro has no revenue cap.
- Every person who contributes copyrightable work that depends on JUCE needs
  a seat, and the product owner is the one who provides it. Starter is one
  individual seat. See the open question at the bottom.

### Tracktion Engine Personal — 50,000 USD

From <https://engine.tracktion.com/agreement> (page read 2026-11-10):

- Personal is free, one seat, no minimum commitment, revenue or funding limit
  **50,000 USD**.
- The Personal section measures **gross revenue generated or raised by you**,
  with no offsets, based on overall monies collected. That wording is wider
  than JUCE's individual "from use of the framework" wording. Treat 50,000 USD
  of money taken in, not of profit, as the line unless Tracktion says
  otherwise.
- If that line is crossed, the agreement says you may keep releasing the
  application only by (a) licensing it under the GNU General Public License
  version 3, or (b) upgrading. The upgrade it names is Indie or Enterprise,
  and elsewhere Indie, Pro or Enterprise. Indie is 35 USD, a 12-month
  commitment, a 200,000 USD cap, and the same **Powered by Tracktion Engine**
  branding. Branding becomes optional at Pro 1 (50 USD, 400,000 USD cap).
- The same section says that once the cap is crossed you must buy an eligible
  different licence, or stop distributing, or license under GPL v3.
- GPL v3, which is the licence Tracktion names, is not this repository's
  AGPL-3.0. JUCE's copyleft arm is the AGPL. Neither copyleft licence is the
  App Store build.
- One seat is one user. Tracktion's published FAQ says each developer who
  contributes source to software containing Tracktion Engine needs their own
  seat. Personal does not cover a second developer.

Whichever cap is hit first wins. JUCE's 20,000 USD, on the individual
wording, can arrive before Tracktion's 50,000 USD.

## Building the App Store configuration

```sh
cmake -B build-appstore -DCMAKE_BUILD_TYPE=Release -DCARVE_APP_STORE=ON
cmake --build build-appstore --target carve --parallel
```

`CARVE_APP_STORE` does three things:

1. Defines `CARVE_APP_STORE=1`, so the About text is the App Store text
   rather than the AGPL offer.
2. Sets `JUCE_PLUGINHOST_VST3=0`. JUCE 8.0.6 carries the VST3 SDK under
   Steinberg's proprietary licence or GPL-3.0
   (`modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt`). The
   App Store binary is not GPL, and this repository has no signed Steinberg
   proprietary SDK agreement, so that binary does not link the SDK.
   `JUCE_PLUGINHOST_AU` stays on. The default (AGPL) build is unchanged and
   still hosts VST3.
3. Leaves `JUCE_DISPLAY_SPLASH_SCREEN` and `JUCE_REPORT_APP_USAGE` unset.
   See above: the first is ignored in JUCE 8.0.6, and the second is not a
   macro here.

`scripts/release.sh` is the Developer ID / notarisation path for the AGPL
disk image. It is not an App Store upload, and it still signs with
`scripts/Carve.entitlements`. Do not point that script at an App Store build
without a separate entitlements file. The hardened-runtime entitlement
`com.apple.security.cs.disable-library-validation` is what lets a plugin host
load other people's AU/VST3 bundles. App Store apps are sandboxed and that
entitlement is not the way through review.

The App Store bundle still needs signing with the Apple Distribution
certificate, a sandbox entitlements plist, and `altool` / Transporter. Those
steps are not in this repository.

## Notices that ship with the binary

[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) is the list shown in the
app and copied into the bundle. [NOTICE.md](NOTICE.md) is the shorter history
of why the source is AGPL. The AGPL disk image also copies `LICENSE` and
`NOTICE.md` next to the app, because the AGPL asks that a recipient of the
program get the licence, not only a link.

## Open questions the owner has to decide

These are not resolved by this file.

**Other authors in git history.** `git shortlog -sn` on this repository is
hashrock (219), Claude `noreply@anthropic.com` (11 commits, 2026-10-15 and
2026-11-09) and Cursor Agent `cursoragent@cursor.com` (1 commit, 2026-10-23,
including `src/app/Main.cpp`). There is no contributor licence agreement.
Shipping a non-AGPL binary needs the right to do that with every part of the
program. Whether those commits are a copyright interest the owner does not
solely hold is a legal question, and so is the status of code an assistant
produced. This file does not answer it.

**One commercial seat.** JUCE Starter and Tracktion Engine Personal are each
one individual seat, and both licences talk about every developer who works
on the product. A single personal seat may not cover code committed under
another identity. Confirm with JUCE and Tracktion before the App Store build
ships, if those commits are treated as someone else contributing.

**SoundTouch is still LGPL-2.1-or-later.** The pitch shifter is compiled in
(`TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH=1`). The header in tracktion_engine
v3.2.0 states LGPL-2.1-or-later, copyright Olli Parviainen. Tracktion's
commercial agreement does not say it relicences that library. An App Store
binary links it statically, and a reviewer or a recipient cannot relink.
Removing it was left undone: with no stretcher compiled in, the pitch shifter
passes audio through unchanged. Decide whether Personal's grant covers
SoundTouch, whether to ship object files and a relink offer outside the
store, or whether the App Store build must drop that effect.

**The JUCE patch.** `patches/juce-ogg-loop-comments.patch` modifies JUCE.
The JUCE 8 grant includes the right to modify the framework, and clause 2.2
of the same agreement says not to modify it. Those two sentences do not sit
together. The patch is what makes `LOOPSTART` / `LOOPLENGTH` survive an Ogg
round trip, so it was not reverted. Confirm Starter allows it.

**Tracktion's own patches** (`patches/tracktion-*.patch`) are modifications
of the engine. Clause 5.1 of the Tracktion agreement says the licensee owns
the modifications they make, and Tracktion keeps the engine. That reading is
not a lawyer's.

**AirWindows** is off. `TRACKTION_AIR_WINDOWS` is unset because the bundled
copy has no licence text. Leave it unset for both builds until there is one.

**VST3 on the store** is off in the App Store configuration for the licence
reason above. The sandbox would also refuse arbitrary third-party plugins
even if the SDK licence were sorted out. Audio Unit hosting is still
compiled in; loading third-party AU bundles inside the App Store sandbox is
the same entitlements problem as VST3, and is not solved here.
