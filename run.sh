#!/usr/bin/env bash
#
# Build and run the app. Output goes to this terminal (which is the point of
# using this rather than double-clicking the .app); Ctrl-C quits.
#
#   ./run.sh                          the GUI, Release
#   ./run.sh --asan                   the GUI, AddressSanitizer build
#   ./run.sh --render --demo out.wav  the headless renderer, arguments passed through
#
# The first run configures the build directory, which downloads tracktion_engine
# and JUCE (~500MB) and takes a while. --asan reuses that checkout rather than
# fetching a second copy.

set -euo pipefail

cd "$(dirname "$0")"

usage() { sed -n '3,9p' "$0" | cut -c3-; }

mode=release
target=carve

while [[ $# -gt 0 ]]; do
    case "$1" in
        --asan)     mode=asan; shift ;;
        --render)   target=carve-render; shift; break ;;
        -h|--help)  usage; exit 0 ;;
        *)          break ;;
    esac
done

if [[ $mode == asan ]]; then
    build_dir=build-asan
    config=Debug
    configure_args=(
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
    )
    # Reuse the checkout the Release build already fetched.
    if [[ -d build/_deps/tracktion-src ]]; then
        configure_args+=(-DFETCHCONTENT_SOURCE_DIR_TRACKTION="$PWD/build/_deps/tracktion-src")
    fi
    export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:print_stacktrace=1}
else
    build_dir=build
    config=Release
    configure_args=(-DCMAKE_BUILD_TYPE=Release)
fi

[[ -d $build_dir ]] || cmake -B "$build_dir" "${configure_args[@]}"
cmake --build "$build_dir" --target "$target" --parallel

if [[ $target == carve ]]; then
    binary="$build_dir/carve_artefacts/$config/Carve DAW.app/Contents/MacOS/Carve DAW"
else
    binary="$build_dir/carve-render_artefacts/$config/carve-render"
fi

exec "$binary" "$@"
