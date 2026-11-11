#!/usr/bin/env bash
#
# Build the Mac App Store package of Carve DAW. It does not upload anything:
# that is Transporter's job, by hand.
#
#   scripts/appstore.sh               build, check, cloud-sign: dist/appstore/CarveDAW-<version>-<build>.pkg
#   scripts/appstore.sh --check-only  build and check only. No certificate, and nothing is sent to Apple
#
# This is not scripts/release.sh. That one makes the AGPL disk image with a
# Developer ID; this one makes the other distribution described in
# LICENSING.md (-DCARVE_APP_STORE=ON: the commercial JUCE and Tracktion
# licences, no plugin hosting, the App Sandbox).
#
# Cloud signing: no distribution certificate has to be on this Mac. With an
# Apple ID of the team signed in to Xcode (Settings > Accounts),
# `xcodebuild -exportArchive -allowProvisioningUpdates` signs the app with the
# certificate Apple keeps (Cloud Managed Apple Distribution), makes and embeds
# the provisioning profile, and signs the installer package. That signing and
# profile request is the only thing here that talks to Apple.
#
# Environment:
#   TEAM_ID        the developer team. Default: 5WLLZHK49R
#   BUILD_NUMBER   CFBundleVersion. It has to go up with every upload of the
#                  same version. Default: the number of commits on HEAD
#   BUILD_DIR      the CMake build directory. Default: build-appstore

set -euo pipefail

cd "$(dirname "$0")/.."

mode=cloud
case "${1-}" in
    "") ;;
    --check-only) mode=check ;;
    *) echo "usage: $0 [--check-only]" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "usage: $0 [--check-only]" >&2; exit 2; }

product="Carve DAW"
bundle_id=com.hashrock.carve
team_id=${TEAM_ID-5WLLZHK49R}
build_dir=${BUILD_DIR-build-appstore}
build_number=${BUILD_NUMBER-$(git rev-list --count HEAD)}
entitlements_file=scripts/Carve-AppStore.entitlements

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

die() { echo "appstore: $*" >&2; exit 1; }
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }

# The entitlements a signed thing carries, as a plist.
entitlements_of() { codesign -d --entitlements - --xml "$1" 2>/dev/null > "$2"; }

# --- build -------------------------------------------------------------------
#
# Universal, because the store takes an Apple-silicon-only app only from
# macOS 12 up and this one runs on 11. Configured every time: the build number
# is a cache entry, and a build directory from last time has last time's.

mkdir -p "$build_dir"
cmake -B "$build_dir" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCARVE_APP_STORE=ON \
      -DCARVE_BUILD_TESTS=OFF \
      "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" \
      -DCARVE_BUILD_NUMBER="$build_number" > "$build_dir/appstore-configure.log" 2>&1 \
    || die "configure failed (log: $build_dir/appstore-configure.log)"

cmake --build "$build_dir" --target carve --parallel "$(sysctl -n hw.ncpu)"

built="$build_dir/carve_artefacts/Release/$product.app"
[[ -d $built ]] || die "no app at $built"

# --- stage -------------------------------------------------------------------
#
# A copy, so the build tree keeps the app the build made. The copy gets the
# keys Xcode writes into an Info.plist when it builds one -- which SDK, which
# Xcode -- because the store's checks read them and a Makefile build has none.

stage="$build_dir/appstore"
app="$stage/$product.app"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$built" "$app"

info="$app/Contents/Info.plist"
sdk_version=$(xcrun --sdk macosx --show-sdk-version)
sdk_build=$(xcrun --sdk macosx --show-sdk-build-version)
xcode_version=$(xcodebuild -version | awk '/^Xcode/ { split($2, v, "."); printf "%02d%d%d", v[1], v[2] + 0, v[3] + 0 }')
xcode_build=$(xcodebuild -version | awk '/^Build version/ { print $3 }')

/usr/libexec/PlistBuddy \
    -c "Add :CFBundleSupportedPlatforms array" \
    -c "Add :CFBundleSupportedPlatforms:0 string MacOSX" \
    -c "Add :CFBundleInfoDictionaryVersion string 6.0" \
    -c "Add :NSPrincipalClass string NSApplication" \
    -c "Add :BuildMachineOSBuild string $(sw_vers -buildVersion)" \
    -c "Add :DTCompiler string com.apple.compilers.llvm.clang.1_0" \
    -c "Add :DTPlatformName string macosx" \
    -c "Add :DTPlatformVersion string $sdk_version" \
    -c "Add :DTPlatformBuild string $sdk_build" \
    -c "Add :DTSDKName string macosx$sdk_version" \
    -c "Add :DTSDKBuild string $sdk_build" \
    -c "Add :DTXcode string $xcode_version" \
    -c "Add :DTXcodeBuild string $xcode_build" "$info" > /dev/null

# Ad hoc, with the sandbox on: this is the app the checks below look at, and
# the one to run by hand to see the sandboxed behaviour before anything is
# sent anywhere. The bundle is one Mach-O, so one codesign covers it.
xattr -cr "$app"
codesign --force --sign - --entitlements "$entitlements_file" "$app"

# --- checks ------------------------------------------------------------------
#
# Everything about an App Store build that can be established without asking
# Apple. Run on the staged app, and again on the app taken back out of the
# finished package.

check_app()
{
    local app=$1
    local bin="$app/Contents/MacOS/$product" info="$app/Contents/Info.plist" res="$app/Contents/Resources"
    local ent="$work/entitlements.plist" arch found key

    [[ -f $bin ]] || die "no executable at $bin"

    for arch in arm64 x86_64; do
        lipo "$bin" -verify_arch "$arch" || die "the executable has no $arch slice"

        # No plugin host. These are the classes and calls that load another
        # vendor's code: JUCE's format classes, the VST3 SDK's host side, and
        # the Audio Unit component calls. (Strings such as ".vst3" do remain:
        # they are the engine's own tables, not a loader.)
        found=$(nm -arch "$arch" "$bin" 2>/dev/null | c++filt \
                    | grep -E "VST3PluginFormat|VSTPluginFormat|AudioUnitPluginFormat|LADSPAPluginFormat|LV2PluginFormat|VST3HostContext|Steinberg::Vst::|AudioComponentFindNext|AudioComponentInstanceNew|AudioComponentInstantiate" \
                    | head -5 || true)
        [[ -z $found ]] || die "the $arch slice still has plugin hosting in it: $found"
    done

    # The minimum system the binary was really built for is the one the
    # Info.plist states.
    local minimum
    minimum=$(plist "$info" LSMinimumSystemVersion)
    [[ -n $minimum ]] || die "Info.plist has no LSMinimumSystemVersion"
    for arch in arm64 x86_64; do
        vtool -arch "$arch" -show-build "$bin" > "$work/build-version.txt"
        grep -qE "minos $minimum\$" "$work/build-version.txt" || die "the $arch slice was not built for macOS $minimum"
    done

    [[ $(plist "$info" CFBundleIdentifier) == "$bundle_id" ]] || die "CFBundleIdentifier is not $bundle_id"
    [[ $(plist "$info" ITSAppUsesNonExemptEncryption) == false ]] || die "ITSAppUsesNonExemptEncryption is not false"
    [[ $(plist "$info" LSApplicationCategoryType) == public.app-category.music ]] || die "LSApplicationCategoryType is not music"
    [[ $(plist "$info" CFBundleVersion) =~ ^[0-9]+(\.[0-9]+){0,2}$ ]] || die "CFBundleVersion is not a build number"

    for key in CFBundleShortVersionString CFBundleDevelopmentRegion CFBundleLocalizations NSHumanReadableCopyright \
               CFBundleSupportedPlatforms DTSDKName DTXcode DTPlatformName; do
        [[ -n $(plist "$info" "$key") ]] || die "Info.plist has no $key"
    done

    # Nothing in it claims a microphone, a camera or anything else the app
    # does not use. An NS...UsageDescription is a claim that it does.
    ! grep -q "UsageDescription" "$info" || die "Info.plist carries a usage description: $(grep UsageDescription "$info")"
    plutil -lint "$info" > /dev/null || die "Info.plist does not parse"

    # The privacy manifest, the notices About shows, the Tracktion lockup
    # the Personal licence asks for, and the factory kit.
    plutil -lint "$res/PrivacyInfo.xcprivacy" > /dev/null || die "PrivacyInfo.xcprivacy is missing or does not parse"
    [[ $(plist "$res/PrivacyInfo.xcprivacy" NSPrivacyTracking) == false ]] || die "PrivacyInfo.xcprivacy does not say NSPrivacyTracking = false"
    [[ -s $res/THIRD_PARTY_NOTICES.txt ]] || die "THIRD_PARTY_NOTICES.txt is not in the bundle"
    [[ -s $res/powered-by-tracktion-engine.png ]] || die "the Tracktion Engine lockup is not in the bundle"
    [[ -s "$res/Content/Drums/Kick.wav" ]] || die "the factory drum kit is not in the bundle"

    # The words the Personal licence asks for, and the App Store wording of
    # About rather than the AGPL offer.
    # (Into a file first: grep -q on a pipe stops reading at the first match,
    # strings dies of the closed pipe, and pipefail calls that a failure.)
    strings -a "$bin" > "$work/strings.txt"
    grep -q "Powered by Tracktion Engine" "$work/strings.txt" || die "the binary does not say Powered by Tracktion Engine"
    grep -q "is not offered under the GNU Affero General" "$work/strings.txt" || die "About is not the App Store text: was this built with -DCARVE_APP_STORE=ON?"

    # The icon, up to 1024 x 1024.
    local iconset="$work/icon.iconset"
    rm -rf "$iconset"
    iconutil -c iconset "$res/$(plist "$info" CFBundleIconFile)" -o "$iconset" || die "the icon does not open"
    [[ $(sips -g pixelWidth "$iconset/icon_512x512@2x.png" 2>/dev/null | awk '/pixelWidth/ { print $2 }') == 1024 ]] \
        || die "the icon has no 1024 x 1024 image"

    # The signature, and the entitlements: the sandbox, the files the user
    # picks, bookmarks to them, and nothing else. No network, no audio input,
    # no exception to library validation.
    codesign --verify --deep --strict "$app" || die "the signature does not verify"
    entitlements_of "$app" "$ent"
    [[ $(plist "$ent" com.apple.security.app-sandbox) == true ]] || die "the App Sandbox is not on"

    local wanted="com.apple.security.app-sandbox com.apple.security.files.user-selected.read-write com.apple.security.files.bookmarks.app-scope"
    local signing="com.apple.application-identifier com.apple.developer.team-identifier"

    for key in $(plutil -convert json -o - "$ent" | /usr/bin/python3 -c 'import json, sys; print("\n".join(json.load(sys.stdin)))'); do
        [[ " $wanted $signing " == *" $key "* ]] || die "an entitlement that should not be there: $key"
    done

    for key in $wanted; do
        [[ $(plist "$ent" "$key") == true ]] || die "entitlement $key is missing"
    done

    # A quarantine mark gets onto a bundle when something downloaded is
    # copied into it, and the store refuses it.
    xattr -lr "$app" > "$work/xattr.txt" 2>/dev/null || true
    ! grep -q com.apple.quarantine "$work/xattr.txt" || die "the app carries a quarantine attribute"
}

# What only the distribution signature can show: Apple's certificate, the
# App ID and team in the signature, and a store profile rather than a
# development one.
check_signed_app()
{
    local app=$1 ent="$work/signed.plist" profile="$work/profile.plist" authority app_id team

    # (Not piped into grep -q: that closes the pipe early and codesign dies
    # of it, which pipefail then reports.)
    authority=$(codesign -dvv "$app" 2>&1 | sed -n 's/^Authority=//p' | head -1)
    [[ $authority =~ ^(Apple\ Distribution|3rd\ Party\ Mac\ Developer\ Application):\  ]] \
        || die "the app is not signed with a distribution certificate: $authority"

    entitlements_of "$app" "$ent"
    app_id=$(plist "$ent" com.apple.application-identifier)
    team=$(plist "$ent" com.apple.developer.team-identifier)
    [[ -n $team ]] || die "the signature carries no team identifier"
    [[ $app_id == "$team.$bundle_id" ]] || die "the signature's App ID is $app_id, not $team.$bundle_id"

    security cms -D -i "$app/Contents/embedded.provisionprofile" > "$profile" 2>/dev/null \
        || die "the app has no provisioning profile, or it does not open"
    [[ $(plist "$profile" "Entitlements:com.apple.application-identifier") == "$app_id" ]] \
        || die "the profile is for another App ID: $(plist "$profile" "Entitlements:com.apple.application-identifier")"
    ! plist "$profile" ProvisionedDevices > /dev/null || die "that is a development profile, not a Mac App Store one"

    echo "appstore: signed by $authority"
    echo "appstore: profile $(plist "$profile" Name) ($app_id, expires $(plist "$profile" ExpirationDate))"
}

check_pkg()
{
    local pkg=$1 out="$work/pkg" apps

    pkgutil --check-signature "$pkg" > "$work/pkg-signature.txt" || die "the package's signature does not verify"
    grep -qE "3rd Party Mac Developer Installer|Mac Installer Distribution" "$work/pkg-signature.txt" \
        || die "the package is not signed with a Mac App Store installer certificate"

    rm -rf "$out"
    pkgutil --expand-full "$pkg" "$out" || die "the package does not expand"
    apps=$(find "$out" -name "*.app" -type d -prune)
    [[ $(grep -c . <<< "$apps") == 1 ]] || die "the package holds something other than one app: $apps"
    [[ $(basename "$apps") == "$product.app" ]] || die "the app in the package is not $product.app: $apps"

    check_app "$apps"
    check_signed_app "$apps"
    echo "appstore: package signed by $(sed -n 's/^ *1\. //p' "$work/pkg-signature.txt" | head -1)"

    # Some terminals mark every file they write with com.apple.provenance,
    # and it travels into the package as "._" files. It cannot be removed.
    local dots
    dots=$(pkgutil --payload-files "$pkg" | grep -c '/\._' || true)
    [[ $dots == 0 ]] || echo "appstore: note: the package carries $dots extended-attribute files (._*). If the upload check objects, build again from Terminal.app"
}

check_app "$app"
echo "appstore: checks passed (ad hoc signature, App Sandbox): $app"
[[ $mode != check ]] || exit 0

# --- sign and package ----------------------------------------------------------
#
# The app goes into the shape of an Xcode archive, and xcodebuild is asked to
# export it for App Store Connect: it re-signs with the cloud certificate,
# keeping the entitlements the app is already signed with, embeds the profile
# it makes, and builds the signed package. `destination = export` is what
# keeps it from uploading.

version=$(plist "$info" CFBundleShortVersionString)
archive="$build_dir/CarveDAW.xcarchive"
export_dir="$work/export"
log="$build_dir/appstore-export.log"
pkg="dist/appstore/CarveDAW-$version-$build_number.pkg"

rm -rf "$archive"
mkdir -p "$archive/Products/Applications"
ditto "$app" "$archive/Products/Applications/$product.app"

archive_info="$archive/Info.plist"
plutil -create xml1 "$archive_info"
/usr/libexec/PlistBuddy \
    -c "Add :ApplicationProperties dict" \
    -c "Add :ApplicationProperties:ApplicationPath string Applications/$product.app" \
    -c "Add :ApplicationProperties:Architectures array" \
    -c "Add :ApplicationProperties:Architectures:0 string arm64" \
    -c "Add :ApplicationProperties:Architectures:1 string x86_64" \
    -c "Add :ApplicationProperties:CFBundleIdentifier string $bundle_id" \
    -c "Add :ApplicationProperties:CFBundleShortVersionString string $version" \
    -c "Add :ApplicationProperties:CFBundleVersion string $build_number" \
    -c "Add :ApplicationProperties:SigningIdentity string -" \
    -c "Add :ApplicationProperties:Team string $team_id" \
    -c "Add :ArchiveVersion integer 2" \
    -c "Add :Name string CarveDAW" \
    -c "Add :SchemeName string CarveDAW" "$archive_info" > /dev/null
plutil -insert CreationDate -date "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" "$archive_info"

options="$work/ExportOptions.plist"
plutil -create xml1 "$options"
/usr/libexec/PlistBuddy \
    -c "Add :method string app-store-connect" \
    -c "Add :destination string export" \
    -c "Add :signingStyle string automatic" \
    -c "Add :teamID string $team_id" \
    -c "Add :manageAppVersionAndBuildNumber bool false" \
    -c "Add :uploadSymbols bool false" "$options" > /dev/null

echo "appstore: cloud signing (this talks to Apple: the signature and the profile, nothing else)..."

if ! xcodebuild -exportArchive -archivePath "$archive" -exportOptionsPlist "$options" \
        -exportPath "$export_dir" -allowProvisioningUpdates > "$log" 2>&1; then
    grep -E "error: " "$log" | head -10 >&2 || true
    die "xcodebuild -exportArchive failed (log: $log). Is an Apple ID of team $team_id signed in under Xcode > Settings > Accounts, and does the App ID $bundle_id exist?"
fi

exported=$(find "$export_dir" -maxdepth 1 -name "*.pkg" | head -1)
[[ -n $exported ]] || die "xcodebuild made no package (log: $log)"

mkdir -p dist/appstore
rm -f "$pkg"
cp "$exported" "$pkg"

# Which certificate, profile and entitlements it was signed with, for the record.
cp "$export_dir/DistributionSummary.plist" "${pkg%.pkg}.DistributionSummary.plist" 2> /dev/null || true

check_pkg "$pkg"
echo "appstore: the app inside the package passed the same checks"
echo "appstore: $pkg ($(du -h "$pkg" | cut -f1))"
echo "appstore: upload it with Transporter. This script does not."
