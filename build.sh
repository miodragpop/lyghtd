#!/bin/bash
# lyghtd build script. Configures + builds via CMake/FetchContent.
#
# Usage:
#   ./build.sh                 # Release
#   ./build.sh Debug           # Debug
#   ./build.sh --tools         # Release + dev/regression tools (tools/)
#   ./build.sh Debug --tools   # Debug + tools
#
# First build is slow: it clones and statically builds gRPC + protobuf + abseil
# + libcurl from source (hundreds of MB, reproducible from CMakeLists.txt).
# Subsequent builds reuse build/_deps and are fast.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

BUILD_TYPE="Release"
TOOLS="OFF"
for arg in "$@"; do
    case "$arg" in
        --tools) TOOLS="ON" ;;
        Release|Debug|RelWithDebInfo|MinSizeRel) BUILD_TYPE="$arg" ;;
        *) echo "Unknown argument: $arg" >&2
           echo "usage: $0 [Release|Debug] [--tools]" >&2; exit 2 ;;
    esac
done

mkdir -p build
cd build

# FETCHCONTENT_QUIET=OFF surfaces git clone progress so the first build isn't a
# silent multi-minute pause.
cmake -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
      -DLYGHTD_BUILD_TOOLS="${TOOLS}" \
      -DFETCHCONTENT_QUIET=OFF ..
make -j"$(nproc)"

echo
echo "Built (${BUILD_TYPE}): ${SCRIPT_DIR}/build/lyghtd"
[ "$TOOLS" = "ON" ] && echo "Tools:           ${SCRIPT_DIR}/build/{rpc_smoke,parse_validate,cache_validate,bench_client,grpc_diff}"
echo "Run with:        ${SCRIPT_DIR}/run.sh"
