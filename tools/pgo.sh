#!/bin/bash
# Profile-guided optimized build (GCC), in build-pgo.
#
#   tools/pgo.sh generate   configure and build build-pgo instrumented (-fprofile-generate)
#   (run one or more titles relinked with build-pgo/core/relinker/relinker; closing the window or
#    SIGTERM writes the profile into build-pgo/profile, and further runs add to it)
#   tools/pgo.sh use        rebuild build-pgo from that profile (-fprofile-use)
#
# The same build directory is used for both steps: the profile files are named after the object
# paths, so the instrumented and the optimized objects must have the same paths.
# Not combined with link-time optimization: GCC's LTO gives C++ frames of the HLE libraries and the
# AGC driver the C personality, so exceptions thrown through them abort (and it measured no gain).
# Environment: JOBS (default 10), TESTING (ON: also build the tests).
set -e
cd "$(dirname "$0")/.."
SOURCE=$PWD
JOBS=${JOBS:-10}
BUILD=$SOURCE/build-pgo
PROFILE=$BUILD/profile

configure() {
    local flags=$1 linkFlags=$2
    cmake -S "$SOURCE" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="${TESTING:-ON}" \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="-D__stdcall= $flags" \
        -DCMAKE_SHARED_LINKER_FLAGS="$linkFlags" -DCMAKE_EXE_LINKER_FLAGS="$linkFlags" -DCMAKE_MODULE_LINKER_FLAGS="$linkFlags"
    cmake --build "$BUILD" -j"$JOBS" --target all libs
}

case "$1" in
    generate)
        rm -rf "$PROFILE"
        configure "-fprofile-generate=$PROFILE -fprofile-update=atomic -DAPS5_PGO_GENERATE" "-fprofile-generate=$PROFILE"
        ;;
    use)
        [ -d "$PROFILE" ] || { echo "no profile in $PROFILE: run titles with the generate build first"; exit 1; }
        # Partial training keeps code the runs never reached optimized for speed, not for size; the
        # quit path differs between the two builds (APS5_PGO_GENERATE), so its profile is dropped.
        configure "-fprofile-use=$PROFILE -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch" ""
        ;;
    *)
        echo "usage: $0 generate|use"; exit 1
        ;;
esac
