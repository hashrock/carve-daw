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
binary under the AGPL. It hosts no third-party plugins.

| | GitHub source and Developer ID disk image | Mac App Store build |
| --- | --- | --- |
| Carve DAW | AGPL-3.0-or-later | Not AGPL. App Store terms, built with the commercial licences below |
| tracktion_engine 3.2.0 | GPL-3.0-or-later | Tracktion Engine Personal (free, individual) |
| JUCE 8.0.6 (inside tracktion_engine) | AGPL-3.0 | JUCE 8 Starter (free, individual) |
| Third-party plugins | VST3 and Audio Unit hosting | None |
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
2. Turns plugin hosting off: `JUCE_PLUGINHOST_VST3=0`,
   `JUCE_PLUGINHOST_AU=0`, and `CARVE_PLUGIN_HOSTING=0`, which takes the
   plugin lists and "Scan / manage plugins..." out of the menus and stops
   the app relaunching itself as a scanner process.
   - VST3: JUCE 8.0.6 carries the VST3 SDK under Steinberg's proprietary
     licence or GPL-3.0
     (`modules/juce_audio_processors/format_types/VST3_SDK/LICENSE.txt`).
     The App Store binary is not GPL, and this repository has no signed
     Steinberg proprietary SDK agreement, so that binary does not link the
     SDK.
   - Audio Units: no licence problem, but the App Sandbox does not load
     other vendors' code into the process, and the entitlement that would
     allow it (`com.apple.security.cs.disable-library-validation`) is not
     one the store accepts.

   A song that uses an external plugin still opens in this build; the
   plugin's slot is there and makes no sound. The default (AGPL) build is
   unchanged and hosts both formats.
3. Leaves `JUCE_DISPLAY_SPLASH_SCREEN` and `JUCE_REPORT_APP_USAGE` unset.
   See above: the first is ignored in JUCE 8.0.6, and the second is not a
   macro here.

### The iPad build (prototype)

An iOS app has no route to its users but the App Store, so there is no AGPL
iPad binary: configuring for iOS forces `CARVE_APP_STORE` on, and everything
above about the commercial licences and their notices applies to it as it
does to the Mac one. About and Third-Party Notices are in the logo menu
there, since iOS has no application menu.

```sh
cmake -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS \
      -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-ios --config Debug --target carve -- \
      -sdk iphonesimulator CODE_SIGNING_ALLOWED=NO
xcrun simctl install booted "build-ios/carve_artefacts/Debug/Carve DAW.app"
xcrun simctl launch booted com.hashrock.carve
```

It has only been run in the simulator. A build for a real iPad needs a
development team and a provisioning profile, which this tree does not set.

### Signing

There are two entitlements files, one per distribution, and they are not
interchangeable.

- `scripts/Carve.entitlements` is the Developer ID / notarisation one, used
  by `scripts/release.sh` for the AGPL disk image. Its single entitlement is
  `com.apple.security.cs.disable-library-validation`, which is what lets a
  plugin host load other people's AU/VST3 bundles.
- `scripts/Carve-AppStore.entitlements` is the App Store one: the App
  Sandbox, read/write access to the files the user picks, and app-scoped
  bookmarks so that access survives a relaunch (`src/app/SandboxAccess.h`).
  No library validation exception, because that build loads nobody else's
  code.

To run the App Store build under the sandbox without a certificate, sign a
copy ad hoc with that file:

```sh
codesign --force --deep -s - --entitlements scripts/Carve-AppStore.entitlements "Carve DAW.app"
```

It then keeps its settings in `~/Library/Containers/com.hashrock.carve`,
not beside the Developer ID build's.

`scripts/release.sh` is not an App Store upload. Do not point it at an App
Store build.

`scripts/appstore.sh` builds the App Store bundle (universal, with the
build number the store wants), checks it -- the sandbox and nothing beyond
it, no plugin host left in the binary, the About wording, the notices --
and has `xcodebuild -exportArchive` sign it with Apple's cloud-managed
distribution certificate, embed the provisioning profile and make the
signed installer package in `dist/appstore/`. `--check-only` stops before
anything is sent to Apple. The upload itself is not scripted: that is
Transporter, by hand.

## Notices that ship with the binary

[THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt) is the list shown in the
app and copied into the bundle. [NOTICE.md](NOTICE.md) is the shorter history
of why the source is AGPL. The AGPL disk image also copies `LICENSE` and
`NOTICE.md` next to the app, because the AGPL asks that a recipient of the
program get the licence, not only a link.

## Decided by the owner

**One developer, one seat.** The owner's position (2026-11-10): Carve DAW has
one developer, hashrock. `git shortlog -sn` also lists Claude
(`noreply@anthropic.com`) and Cursor Agent (`cursoragent@cursor.com`); those
commits were made by coding assistants at the owner's direction, on the
owner's machine and accounts, and are not a second contributor. No one else
has contributed source. On that basis the single JUCE Starter seat and the
single Tracktion Engine Personal seat cover the product, and no contributor
licence agreement is in place because there are no outside contributors. If
a pull request from someone else is ever merged, both of those change: that
person needs a seat under each licence, and the App Store build needs their
permission to ship their code outside the AGPL.

## Open questions, and ones that have been closed

**SoundTouch** is not an open question any more: it is not in either
binary. The engine's own pitch shifter needed it, so the Pitch Shifter effect
is now `src/plugins/PitchShifterPlugin`, built on Signalsmith Stretch (MIT),
and `patches/tracktion-soundtouch-optional.patch` stops the engine compiling
SoundTouch's tempo detector, which it otherwise did whatever the build asked
for. The engine's tempo detection reports "no tempo" as a result. Carve does
not use it.

**The JUCE patch** is gone as well. JUCE is built unmodified. The loop points
a game-loop export needs (`LOOPSTART` / `LOOPLENGTH`) are written into the
finished Ogg file by `src/sync/VorbisComments`, which is Carve's own code and
uses no JUCE internals. The question of whether Starter permits modifying the
framework no longer applies to this tree.

**Tracktion's own patches** (`patches/tracktion-*.patch`) are modifications
of the engine. Clause 5.1 of the Tracktion agreement says the licensee owns
the modifications they make, and Tracktion keeps the engine. That reading is
not a lawyer's.

**AirWindows** is off. `TRACKTION_AIR_WINDOWS` is unset because the bundled
copy has no licence text. Leave it unset for both builds until there is one.

**Plugins on the store** are off altogether in the App Store configuration:
VST3 for the licence reason above, Audio Units because of the sandbox. If
hosting ever comes back to that build it will be Audio Units loaded out of
process (AUv3), which is a different piece of work.
