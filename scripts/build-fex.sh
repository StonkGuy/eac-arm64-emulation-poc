#!/bin/sh
# Builds the patched FEX-Emu: StonkGuy/FEX at the pinned tag (branch eac-2610 = FEX-2610 + the patches described in
# docs/patches.md; patches/ is a generated snapshot of the same commits).
#
#   scripts/build-fex.sh            clone (if needed) + configure + build into ./work/FEX/build
#   JOBS=6 scripts/build-fex.sh     limit parallel jobs (default: nproc)
#   CC=clang CXX=clang++            override the compilers (FEX needs a recent clang)
#
# Build dependencies (Fedora): sudo dnf builddep fex-emu   (or: clang cmake ninja-build git python3 nasm libepoxy-devel
# SDL2-devel openssl-devel squashfuse-devel qt6-qtbase-devel ...). This script builds only the emulator itself; the
# thunks/RootFS of your distro's fex-emu package keep being used.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$REPO/work}
FEX_REPO=https://github.com/StonkGuy/FEX.git
FEX_EAC_TAG=eac-2610.3
FEX_EAC_SHA=938eedfcd79eb00e86561fbdf4933770766f0ba9
JOBS=${JOBS:-$(nproc)}
CC=${CC:-clang}; CXX=${CXX:-clang++}

mkdir -p "$WORK"
if [ ! -d "$WORK/FEX/.git" ]; then
  git clone --branch "$FEX_EAC_TAG" --recurse-submodules --shallow-submodules "$FEX_REPO" "$WORK/FEX"
fi
cd "$WORK/FEX"
# A tree from an older version of this script (upstream FEX-2610 with patches/ applied) or another tag is not updated
# in place.
if [ "$(git rev-parse HEAD)" != "$FEX_EAC_SHA" ]; then
  if [ "$(git rev-parse -q --verify "$FEX_EAC_TAG^{commit}" || true)" = "$FEX_EAC_SHA" ] && git diff --quiet HEAD; then
    git checkout -q "$FEX_EAC_TAG" && git submodule update --init --recursive --depth 1
  else
    echo "$WORK/FEX is not $FEX_EAC_TAG ($FEX_EAC_SHA); probably a tree from an older build-fex.sh." >&2
    echo "Remove it and run again:  rm -rf '$WORK/FEX'" >&2; exit 1
  fi
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF -DTUNE_CPU=native \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF   # 2610's fmt is C++20; without this CMake needs clang-scan-deps
cmake --build build -j "$JOBS"
echo
echo "built: $WORK/FEX/build/Bin/FEX"
echo "next:  scripts/vm/install-overlay.sh   (installs it as an overlay; the VM registers it with binfmt when it starts)"
