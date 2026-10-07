#!/bin/sh
# Builds FEX-Emu 2609.1 with the patches from patches/ applied.
#
#   scripts/build-fex.sh            clone (if needed) + patch + configure + build into ./work/FEX/build
#   JOBS=6 scripts/build-fex.sh     limit parallel jobs (default: nproc)
#   CC=clang CXX=clang++            override the compilers (FEX needs a recent clang)
#
# Build dependencies (Fedora): sudo dnf builddep fex-emu   (or: clang cmake ninja-build git python3 nasm libepoxy-devel
# SDL2-devel openssl-devel squashfuse-devel qt6-qtbase-devel ...). This script builds only the emulator itself; the
# thunks/RootFS of your distro's fex-emu package keep being used.
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$REPO/work}
TAG=FEX-2609.1
BASE=9fbdc00bd6401aff3b32d79e78ff98b8a13e4dcf
JOBS=${JOBS:-$(nproc)}
CC=${CC:-clang}; CXX=${CXX:-clang++}

mkdir -p "$WORK"
if [ ! -d "$WORK/FEX/.git" ]; then
  git clone --branch "$TAG" --recurse-submodules --shallow-submodules https://github.com/FEX-Emu/FEX.git "$WORK/FEX"
fi
cd "$WORK/FEX"
git cat-file -e "$BASE^{commit}" 2>/dev/null || { echo "expected FEX at $TAG ($BASE)"; exit 1; }
git merge-base --is-ancestor "$BASE" HEAD || { echo "$WORK/FEX is not based on $TAG ($BASE): remove it and run again"; exit 1; }
HAVE=$(git rev-list --count "$BASE"..HEAD); WANT=$(ls "$REPO"/patches/*.patch | wc -l)
if [ "$HAVE" -eq 0 ]; then
  git -c user.name=build -c user.email=build@invalid am "$REPO"/patches/*.patch
elif [ "$HAVE" -ne "$WANT" ]; then
  echo "$WORK/FEX has $HAVE commits on top of $TAG but patches/ has $WANT: remove $WORK/FEX and run again"; exit 1
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
  -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF -DENABLE_LTO=OFF -DENABLE_ASSERTIONS=OFF -DTUNE_CPU=native
cmake --build build -j "$JOBS"
echo
echo "built: $WORK/FEX/build/Bin/FEX"
echo "next:  scripts/vm/install-overlay.sh   (installs it as an overlay and registers it with binfmt inside the VM)"
