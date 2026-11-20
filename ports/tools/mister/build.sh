#!/bin/sh
# usage: tools/mister/build.sh VERSION OUTDIR   (Linux, or Git Bash with Docker Desktop)
# The MiSTer release, as CI makes it: the game in Dockerfile's toolchain (build-mister), MiSTer_MVC2 with
# build-hps.sh, then package.sh into OUTDIR. MVC2_DECOMP: the decompilation (default: the tree around ports/).
set -eu
v=${1:?usage: build.sh VERSION OUTDIR} out=${2:?usage: build.sh VERSION OUTDIR}
port=$(cd "$(dirname "$0")/../.." && pwd)
dec=$(cd "${MVC2_DECOMP:-$port/..}" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
host() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }

docker build -q -t mvc2-mister-gcc "$(host "$port/tools/mister")" > /dev/null
MSYS_NO_PATHCONV=1 docker run --rm -v "$(host "$port"):/port" -v "$(host "$dec"):/decomp:ro" -w /port mvc2-mister-gcc sh -c \
    "cmake -S . -B build-mister -G Ninja -DCMAKE_TOOLCHAIN_FILE=port/cmake/mister.cmake -DCMAKE_BUILD_TYPE=Release \
        -DMVC2_DECOMP=/decomp -DMVC2_VERSION=$v > /dev/null &&
     ninja -C build-mister mvc2 && arm-linux-gnueabihf-strip -o build-mister/mvc2-pinedra build-mister/mvc2"
MISTER_BUILD="$port/build/mister" sh "$port/tools/mister/build-hps.sh"
PND_EXE="$port/build-mister/mvc2-pinedra" WRAPPER="$port/build/mister/MiSTer_MVC2" sh "$port/tools/mister/package.sh" "$v" "$out"
