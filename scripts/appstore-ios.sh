#!/usr/bin/env bash
#
# Build the App Store package of the iPad app. Like scripts/appstore.sh, its
# Mac counterpart, it does not upload anything: Transporter does that, by hand.
#
#   scripts/appstore-ios.sh               build, check, cloud-sign: dist/appstore/CarveDAW-iPad-<version>-<build>.ipa
#   scripts/appstore-ios.sh --check-only  build and check only. Nothing is sent to Apple but
#                                         the development signing a device build needs
#
# The iPad build is always the store's licences (see LICENSING.md): there is
# no AGPL iPad binary, and configuring for iOS turns CARVE_APP_STORE on.
#
# How it is made: the Xcode generator builds and signs the app for a device
# with the team's development certificate (Automatic signing, profiles made
# as needed), the app is put into the shape of an Xcode archive, and
# `xcodebuild -exportArchive` re-signs it with the distribution certificate
# Apple keeps (cloud signing) and makes the .ipa. Building through
# `xcodebuild archive` is not used: the artefact folders JUCE's CMake sets up
# leave an archive of a CMake project empty.
#
# Needs an Apple ID of the team signed in to Xcode (Settings > Accounts).
#
# Environment:
#   TEAM_ID        the developer team. Default: 5WLLZHK49R
#   BUILD_NUMBER   CFBundleVersion. It has to go up with every upload of the
#                  same version. Default: the number of commits on HEAD
#   BUILD_DIR      the CMake build directory. Default: build-appstore-ios

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
build_dir=${BUILD_DIR-build-appstore-ios}
build_number=${BUILD_NUMBER-$(git rev-list --count HEAD)}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

die() { echo "appstore-ios: $*" >&2; exit 1; }
plist() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }
entitlements_of() { codesign -d --entitlements - --xml "$1" 2>/dev/null > "$2"; }

# --- build -------------------------------------------------------------------
#
# Configured every time, as on the Mac: the build number is a cache entry.
# The engine's sources are taken from the desktop build's download when there
# is one, rather than fetched a second time.

cmake_args=(
    -G Xcode
    -DCMAKE_SYSTEM_NAME=iOS
    -DCMAKE_OSX_SYSROOT=iphoneos
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCARVE_BUILD_NUMBER="$build_number"
    -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="$team_id"
    -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic
)
[[ -d build/_deps/tracktion-src ]] && cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_TRACKTION=$PWD/build/_deps/tracktion-src")

mkdir -p "$build_dir"
cmake -B "$build_dir" "${cmake_args[@]}" > "$build_dir/appstore-configure.log" 2>&1 \
    || die "configure failed (log: $build_dir/appstore-configure.log)"

if ! cmake --build "$build_dir" --config Release --target carve -- \
        -sdk iphoneos -allowProvisioningUpdates > "$build_dir/appstore-build.log" 2>&1; then
    grep -E "error: " "$build_dir/appstore-build.log" | head -10 >&2 || true
    die "the build failed (log: $build_dir/appstore-build.log)"
fi

built="$build_dir/carve_artefacts/Release/$product.app"
[[ -d $built ]] || die "no app at $built"

stage="$build_dir/appstore"
app="$stage/$product.app"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$built" "$app"

# --- checks ------------------------------------------------------------------
#
# What can be established without asking Apple, on the built app and again on
# the one taken back out of the finished .ipa. An iOS bundle is flat: the
# Info.plist, the executable and the resources all at its top.

check_app()
{
    local app=$1
    local bin="$app/$product" info="$app/Info.plist"
    local found minimum key

    [[ -f $bin ]] || die "no executable at $bin"
    lipo "$bin" -verify_arch arm64 || die "the executable has no arm64 slice"

    # No plugin host, as on the Mac -- by JUCE's format classes and the VST3
    # SDK only: the AudioComponent calls the Mac check also looks for are how
    # iOS opens its own audio output (RemoteIO), so they are here regardless.
    found=$(nm "$bin" 2>/dev/null | c++filt \
                | grep -E "VST3PluginFormat|VSTPluginFormat|AudioUnitPluginFormat|VST3HostContext|Steinberg::Vst::" \
                | head -5 || true)
    [[ -z $found ]] || die "the app still has plugin hosting in it: $found"

    minimum=$(plist "$info" MinimumOSVersion)
    [[ -n $minimum ]] || die "Info.plist has no MinimumOSVersion"
    vtool -show-build "$bin" > "$work/build-version.txt"
    grep -qE "minos $minimum(\.0)?\$" "$work/build-version.txt" || die "the executable was not built for iOS $minimum"

    [[ $(plist "$info" CFBundleIdentifier) == "$bundle_id" ]] || die "CFBundleIdentifier is not $bundle_id"
    [[ $(plist "$info" ITSAppUsesNonExemptEncryption) == false ]] || die "ITSAppUsesNonExemptEncryption is not false"
    [[ $(plist "$info" CFBundleVersion) == "$build_number" ]] || die "CFBundleVersion is $(plist "$info" CFBundleVersion), not $build_number"
    [[ $(plist "$info" UIDeviceFamily:0) == 2 && -z $(plist "$info" UIDeviceFamily:1) ]] || die "UIDeviceFamily is not the iPad alone"

    for key in CFBundleShortVersionString CFBundleDisplayName NSHumanReadableCopyright UILaunchStoryboardName \
               DTSDKName DTXcode DTPlatformName; do
        [[ -n $(plist "$info" "$key") ]] || die "Info.plist has no $key"
    done

    # The app records nothing, so it asks for nothing: no microphone, no
    # Bluetooth, no camera.
    ! grep -q "UsageDescription" "$info" || die "Info.plist carries a usage description: $(grep UsageDescription "$info")"

    # Background audio is declared, and is only worth declaring for audio:
    # the playing song goes on when the app is switched away from.
    [[ $(plist "$info" UIBackgroundModes:0) == audio && -z $(plist "$info" UIBackgroundModes:1) ]] \
        || die "UIBackgroundModes is not just audio"

    plutil -lint "$info" > /dev/null || die "Info.plist does not parse"

    plutil -lint "$app/PrivacyInfo.xcprivacy" > /dev/null || die "PrivacyInfo.xcprivacy is missing or does not parse"
    [[ $(plist "$app/PrivacyInfo.xcprivacy" NSPrivacyTracking) == false ]] || die "PrivacyInfo.xcprivacy does not say NSPrivacyTracking = false"
    [[ -s $app/THIRD_PARTY_NOTICES.txt ]] || die "THIRD_PARTY_NOTICES.txt is not in the bundle"
    [[ -s $app/powered-by-tracktion-engine.png ]] || die "the Tracktion Engine lockup is not in the bundle"
    [[ -s "$app/Content/Drums/Kick.wav" ]] || die "the factory drum kit is not in the bundle"

    strings -a "$bin" > "$work/strings.txt"
    grep -q "Powered by Tracktion Engine" "$work/strings.txt" || die "the binary does not say Powered by Tracktion Engine"
    grep -q "is not offered under the GNU Affero General" "$work/strings.txt" || die "About is not the App Store text"

    # The icons the store's checks ask an iPad app for: the iPad Pro's
    # 167 x 167 and the store's 1024 x 1024.
    xcrun assetutil --info "$app/Assets.car" > "$work/assets.json" 2>/dev/null || die "Assets.car does not open"
    grep -q '"PixelWidth" : 167' "$work/assets.json" || die "the asset catalogue has no 167 x 167 icon"
    grep -q '"PixelWidth" : 1024' "$work/assets.json" || die "the asset catalogue has no 1024 x 1024 icon"

    codesign --verify --deep --strict "$app" || die "the signature does not verify"

    xattr -lr "$app" > "$work/xattr.txt" 2>/dev/null || true
    ! grep -q com.apple.quarantine "$work/xattr.txt" || die "the app carries a quarantine attribute"
}

# What only the distribution signature shows: Apple's certificate, the App
# ID, a store profile, and no debugger entitlement.
check_signed_app()
{
    local app=$1 ent="$work/signed.plist" profile="$work/profile.plist" authority app_id key

    authority=$(codesign -dvv "$app" 2>&1 | sed -n 's/^Authority=//p' | head -1)
    [[ $authority =~ ^(Apple|iPhone)\ Distribution ]] || die "the app is not signed with a distribution certificate: $authority"

    entitlements_of "$app" "$ent"
    app_id=$(plist "$ent" application-identifier)
    [[ $app_id == *".$bundle_id" ]] || die "the signature's App ID is $app_id"
    [[ $(plist "$ent" get-task-allow) != true ]] || die "the app is signed with get-task-allow: a development signature"

    # Nothing but what every store app is signed with.
    for key in $(plutil -convert json -o - "$ent" | /usr/bin/python3 -c 'import json, sys; print("\n".join(json.load(sys.stdin)))'); do
        case $key in
            application-identifier|com.apple.developer.team-identifier|get-task-allow|keychain-access-groups|beta-reports-active) ;;
            *) die "an entitlement that should not be there: $key" ;;
        esac
    done

    security cms -D -i "$app/embedded.mobileprovision" > "$profile" 2>/dev/null \
        || die "the app has no provisioning profile, or it does not open"
    ! plist "$profile" ProvisionedDevices > /dev/null || die "that is a development or ad hoc profile, not an App Store one"

    echo "appstore-ios: signed by $authority"
    echo "appstore-ios: profile $(plist "$profile" Name) ($app_id, expires $(plist "$profile" ExpirationDate))"
}

check_app "$app"
echo "appstore-ios: checks passed (development signature): $app"
[[ $mode != check ]] || exit 0

# --- sign and package ----------------------------------------------------------

info="$app/Info.plist"
version=$(plist "$info" CFBundleShortVersionString)
archive="$build_dir/CarveDAW-iPad.xcarchive"
export_dir="$work/export"
log="$build_dir/appstore-export.log"
ipa="dist/appstore/CarveDAW-iPad-$version-$build_number.ipa"

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
    -c "Add :ApplicationProperties:CFBundleIdentifier string $bundle_id" \
    -c "Add :ApplicationProperties:CFBundleShortVersionString string $version" \
    -c "Add :ApplicationProperties:CFBundleVersion string $build_number" \
    -c "Add :ApplicationProperties:SigningIdentity string Apple Development" \
    -c "Add :ApplicationProperties:Team string $team_id" \
    -c "Add :ArchiveVersion integer 2" \
    -c "Add :Name string CarveDAW-iPad" \
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

echo "appstore-ios: cloud signing (this talks to Apple: the signature and the profile, nothing else)..."

if ! xcodebuild -exportArchive -archivePath "$archive" -exportOptionsPlist "$options" \
        -exportPath "$export_dir" -allowProvisioningUpdates > "$log" 2>&1; then
    grep -E "error: " "$log" | head -10 >&2 || true
    die "xcodebuild -exportArchive failed (log: $log)"
fi

exported=$(find "$export_dir" -maxdepth 1 -name "*.ipa" | head -1)
[[ -n $exported ]] || die "xcodebuild made no .ipa (log: $log)"

mkdir -p dist/appstore
rm -f "$ipa"
cp "$exported" "$ipa"
cp "$export_dir/DistributionSummary.plist" "${ipa%.ipa}.DistributionSummary.plist" 2> /dev/null || true

# The app as it will be uploaded: out of the .ipa, through the same checks.
rm -rf "$work/ipa"
mkdir -p "$work/ipa"
ditto -x -k "$ipa" "$work/ipa" || die "the .ipa does not open"
check_app "$work/ipa/Payload/$product.app"
check_signed_app "$work/ipa/Payload/$product.app"

echo "appstore-ios: the app inside the .ipa passed the same checks"
echo "appstore-ios: $ipa ($(du -h "$ipa" | cut -f1))"
echo "appstore-ios: upload it with Transporter. This script does not."
