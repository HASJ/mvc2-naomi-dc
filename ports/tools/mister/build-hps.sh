#!/bin/sh
# Builds MiSTer_MVC2: Main_MiSTer at a pinned commit, patched, plus the
# wrapper loop, cross-compiled in upstream's own toolchain container.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${MISTER_BUILD:-$here/../../build/mister}
url=https://github.com/MiSTer-devel/Main_MiSTer.git
commit=3380931329b8acb442bd3d35a24d89f88641b7cf
img=mvc2-mister-hps

mkdir -p "$out"
out=$(cd "$out" && pwd)
src=$out/hps
host() { (cd "$1" && { pwd -W 2>/dev/null || pwd; }); }

if [ ! -d "$src/.git" ]; then
	git clone -q --filter=blob:none --no-checkout "$url" "$src"
fi
git -C "$src" checkout -q -f "$commit"
git -C "$src" clean -q -fdx -e bin
git -C "$src" apply "$here/agpl/wrapper/main-mister.patch"
cp "$here/agpl/wrapper/mvc2_wrapper.cpp" "$here/agpl/wrapper/mvc2_joy_shm.h" "$here/agpl/wrapper/Makefile.mvc2" "$src/"

if ! docker image inspect "$img" > /dev/null 2>&1; then
	docker build -q -t "$img" "$(host "$src/.devcontainer")"
fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$(host "$src"):/src" -w /src "$img" \
	bash -c 'export PATH=$PATH:$(echo /usr/local/bin/gcc-arm-*/bin); make -f Makefile.mvc2' > "$out/hps.log" 2>&1 || {
	grep -E "error|Error" "$out/hps.log" | head -20
	exit 1
}
cp "$src/bin/MiSTer_MVC2" "$out/MiSTer_MVC2"
ls -l "$out/MiSTer_MVC2"
