#!/usr/bin/env bash
#
# Build and run the app. Output goes to this terminal (which is the point of
# using this rather than double-clicking the .app); Ctrl-C quits.
#
#   ./run.sh                          the GUI, Release
#   ./run.sh --asan                   the GUI, AddressSanitizer build
#   ./run.sh --render --demo out.wav  the headless renderer, arguments passed through
#   ./run.sh --test                   the property tests, arguments passed through
#   ./run.sh --sync-test              the property tests that need the engine
#
# The first configure on a machine downloads tracktion_engine and JUCE (~500MB)
# and takes a while. It goes to a directory every checkout and build directory
# shares (CARVE_DEPS_DIR in CMakeLists.txt), so --asan, --test and worktrees
# do not fetch it again.

set -euo pipefail

cd "$(dirname "$0")"

usage() { sed -n '3,10p' "$0" | cut -c3-; }

mode=release
target=carve

while [[ $# -gt 0 ]]; do
    case "$1" in
        --asan)     mode=asan; shift ;;
        --render)   target=carve-render; shift; break ;;
        --test)     mode=test; target=carve-tests; shift; break ;;
        --sync-test) mode=sync-test; target=carve-sync-tests; shift; break ;;
        -h|--help)  usage; exit 0 ;;
        *)          break ;;
    esac
done

if [[ $mode == test ]]; then
    # Its own directory at Debug, so the assertions the properties rely on are
    # live and the app's Release build is left alone.
    build_dir=build-tests
    config=Debug
    configure_args=(-DCMAKE_BUILD_TYPE=Debug)
elif [[ $mode == sync-test ]]; then
    # These link the engine, so they live in the app's own build directory:
    # tracktion is compiled there already and this target reuses it rather
    # than paying for a second copy the way a Debug test build would.
    build_dir=build
    config=Release
    configure_args=(-DCMAKE_BUILD_TYPE=Release -DCARVE_BUILD_TESTS=ON)

    # The option is sticky in the cache, and a build directory configured
    # before this target existed will have it off.
    cmake -B "$build_dir" "${configure_args[@]}" > /dev/null
elif [[ $mode == asan ]]; then
    build_dir=build-asan
    config=Debug
    configure_args=(
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
        -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"
        -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
    )
    export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:print_stacktrace=1}
else
    build_dir=build
    config=Release
    configure_args=(-DCMAKE_BUILD_TYPE=Release)
fi

[[ -d $build_dir ]] || cmake -B "$build_dir" "${configure_args[@]}"
cmake --build "$build_dir" --target "$target" --parallel

if [[ $target == carve-sync-tests ]]; then
    binary="$build_dir/carve-sync-tests_artefacts/$config/carve-sync-tests"
elif [[ $target == carve ]]; then
    binary="$build_dir/carve_artefacts/$config/Carve DAW.app/Contents/MacOS/Carve DAW"
elif [[ $target == carve-tests ]]; then
    binary="$build_dir/carve-tests_artefacts/$config/carve-tests"
else
    binary="$build_dir/carve-render_artefacts/$config/carve-render"
fi

exec "$binary" "$@"
