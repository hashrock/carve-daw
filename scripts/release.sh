#!/usr/bin/env bash
#
# Sign, notarise and package "Carve DAW.app" for distribution outside the
# App Store. Output lands in dist/.
#
#   scripts/release.sh                  build, sign, notarise, staple, make a DMG
#   scripts/release.sh --no-notarize    sign only (a quick check that signing works)
#
# One-time setup, both on the Mac that runs this:
#
#   1. A "Developer ID Application" certificate in the login keychain. Made in
#      Xcode > Settings > Accounts > Manage Certificates (needs an Apple
#      Developer Program membership), or on developer.apple.com.
#
#   2. Credentials for notarytool, stored under a keychain profile name:
#        xcrun notarytool store-credentials carve-notary \
#            --apple-id you@example.com --team-id TEAMID
#      It asks for an app-specific password (appleid.apple.com > Sign-In and
#      Security > App-Specific Passwords), not the account password.
#
# Environment:
#   CARVE_SIGN_IDENTITY     the certificate's name, e.g.
#                           "Developer ID Application: Your Name (TEAMID)".
#                           Default: the first Developer ID Application found.
#   CARVE_NOTARY_PROFILE    the notarytool profile name. Default: carve-notary.
#   CARVE_NOTARY_SUBMISSION a submission id from an earlier run of this
#                           script, to pick up where a wait was cut short
#                           rather than submit the app again. The app in
#                           dist/ must be the one that was submitted.

set -euo pipefail

cd "$(dirname "$0")/.."

notarize=1
[[ ${1:-} == --no-notarize ]] && notarize=0

die() { echo "release: $*" >&2; exit 1; }

# Submits a file and waits for Apple's verdict, printing the log if it was
# refused. `notarytool submit --wait` polls over one HTTP session and gives up
# with a timeout when the queue is slow (a first submission can sit in it for
# a good while), so the wait is done separately and retried.
notarize_file()
{
    local file=$1 id=${2:-}

    if [[ -z $id ]]; then
        id=$(xcrun notarytool submit "$file" --keychain-profile "$profile"                  | sed -nE 's/^ *id: ([0-9a-f-]+)$/\1/p' | head -1)
        [[ -n $id ]] || die "submission of $file returned no id"
        echo "release: submitted $file as $id"
    else
        echo "release: waiting on earlier submission $id"
    fi

    local status="" attempt
    for attempt in 1 2 3 4 5 6; do
        xcrun notarytool wait "$id" --keychain-profile "$profile" && break
        echo "release: wait dropped (attempt $attempt); asking again"
        sleep 30
    done

    status=$(xcrun notarytool info "$id" --keychain-profile "$profile"                  | sed -nE 's/^ *status: (.*)$/\1/p')

    if [[ $status != Accepted ]]; then
        xcrun notarytool log "$id" --keychain-profile "$profile" || true
        die "notarisation of $file ended as: ${status:-unknown}"
    fi
}

# --- build -------------------------------------------------------------------

[[ -d build ]] || cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target carve --parallel

built="build/carve_artefacts/Release/Carve DAW.app"
[[ -d $built ]] || die "no app at $built"

version=$(plutil -extract CFBundleShortVersionString raw "$built/Contents/Info.plist")

# --- identity ----------------------------------------------------------------

identity=${CARVE_SIGN_IDENTITY:-$(security find-identity -v -p codesigning \
    | grep "Developer ID Application" | head -1 | sed -E 's/.*"(.*)".*/\1/')}

[[ -n $identity ]] || die "no Developer ID Application certificate in the keychain (see the setup notes at the top of this script)"

profile=${CARVE_NOTARY_PROFILE:-carve-notary}

echo "release: signing as $identity"

# --- sign --------------------------------------------------------------------

mkdir -p dist
app="dist/Carve DAW.app"
resume=${CARVE_NOTARY_SUBMISSION:-}

if [[ -z $resume ]]; then
    # Work on a copy: the build tree keeps its ad-hoc signature for local runs.
    rm -rf "$app"
    ditto "$built" "$app"

    # The bundle is one Mach-O and a nib -- no nested frameworks or helpers
    # to sign inside-out first, so a single codesign covers it. The plugin
    # scanner is this same executable re-launched, so it is signed by the
    # same call.
    codesign --force --options runtime --timestamp \
             --entitlements scripts/Carve.entitlements \
             --sign "$identity" "$app"
else
    # Resuming: the signed app in dist/ is the one Apple has, so it stays.
    [[ -d $app ]] || die "nothing to resume: $app is missing"
fi

codesign --verify --deep --strict --verbose=2 "$app"

# --- notarise ----------------------------------------------------------------

if (( notarize )); then
    zip="dist/Carve DAW.zip"

    if [[ -z $resume ]]; then
        ditto -c -k --keepParent "$app" "$zip"
    fi

    notarize_file "$zip" "$resume"
    rm -f "$zip"

    # The ticket goes into the bundle, so Gatekeeper passes it offline too.
    xcrun stapler staple "$app"
fi

# --- package -----------------------------------------------------------------

dmg="dist/CarveDAW-$version.dmg"
staging=$(mktemp -d)
ditto "$app" "$staging/Carve DAW.app"
ln -s /Applications "$staging/Applications"

# The licence travels with the binary: the AGPL asks that every recipient of
# the program gets a copy of it, and a link in an About box is not that. The
# source it was built from is the repository the notice points at.
cp LICENSE "$staging/LICENSE.txt"
cp NOTICE.md "$staging/NOTICE.txt"

rm -f "$dmg"
hdiutil create -volname "Carve DAW" -srcfolder "$staging" -ov -format UDZO -quiet "$dmg"
rm -rf "$staging"

codesign --force --timestamp --sign "$identity" "$dmg"

if (( notarize )); then
    notarize_file "$dmg"
    xcrun stapler staple "$dmg"

    # What a user's Mac will decide.
    spctl --assess --type open --context context:primary-signature -v "$dmg"
fi

echo "release: $dmg"
