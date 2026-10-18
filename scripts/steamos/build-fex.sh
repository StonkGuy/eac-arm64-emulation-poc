#!/bin/sh
# SteamOS (Steam Frame, or the steamac Mac guest): build the patched FEX (StonkGuy/FEX, pinned tag) on the device in
# Steam mode and stage it for install-fex-tool.sh. POSIX sh; needs clang, cmake, ninja, lld, git, python3. Re-run safe.
set -eu

WORK=${WORK:-$HOME/fex-eac-work}
SRC=$WORK/FEX
STAGE=$WORK/stage
REPO=https://github.com/StonkGuy/FEX.git
FEX_EAC_TAG=eac-2610.2
FEX_EAC_SHA=cc1cc2a14da9f2e844f5b2b2b5bec3db237bb836

# Clone only the exact tag unless the tree is already there.
if [ ! -d "$SRC/.git" ]; then
  mkdir -p "$WORK"
  git clone --depth 1 --branch "$FEX_EAC_TAG" "$REPO" "$SRC"
fi
cd "$SRC"

# The tree must be the pinned commit. A tree from an older build-fex.sh (upstream FEX-2610 with the patch files
# applied by git am) or from another tag is not updated in place.
HEADSHA=$(git rev-parse HEAD)
if [ "$HEADSHA" != "$FEX_EAC_SHA" ]; then
  if git cat-file -e "$FEX_EAC_TAG^{commit}" 2>/dev/null &&
     [ "$(git rev-parse "$FEX_EAC_TAG^{commit}")" = "$FEX_EAC_SHA" ] && git diff --quiet HEAD; then
    git checkout -q "$FEX_EAC_TAG"
  else
    echo "$SRC is at $(git rev-parse --short HEAD), not $FEX_EAC_TAG ($FEX_EAC_SHA)." >&2
    echo "It is probably a tree from an older build-fex.sh. Remove it and run this script again:" >&2
    echo "  rm -rf '$SRC'" >&2
    exit 1
  fi
fi
[ "$(git rev-parse "$FEX_EAC_TAG^{commit}")" = "$FEX_EAC_SHA" ] ||
  { echo "$FEX_EAC_TAG is $(git rev-parse "$FEX_EAC_TAG^{commit}"), expected $FEX_EAC_SHA" >&2; exit 1; }

# This pinned Steam Runtime configuration needs no test binaries, thunks, profiler or disassembler dependencies.
git submodule update --init --recursive --depth 1 -- \
  Source/Common/cpp-optparse External/fmt External/drm-headers External/xxhash \
  External/jemalloc_glibc External/range-v3 External/unordered_dense External/rpmalloc

# Release Steam-mode build: no tests, thunks or FEXConfig, native CPU tuning, module scanning off for clang 19.
cmake -S . -B build-steam -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_STEAM_SUPPORT=ON \
  -DBUILD_TESTING=OFF \
  -DBUILD_THUNKS=OFF \
  -DBUILD_FEXCONFIG=OFF \
  -DENABLE_LTO=OFF \
  -DENABLE_ASSERTIONS=OFF \
  -DTUNE_CPU=native \
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
  -DCMAKE_INSTALL_PREFIX=/usr

# Build only the tools Valve ships, skipping development helpers. Strip staged copies, leaving the build intact.
cmake --build build-steam --target FEX FEXBash FEXGetConfig FEXOfflineCompiler FEXServer FEXServerManager FEXpidof FEXCompatTool \
  -j"${JOBS:-$(nproc)}"
DESTDIR=$STAGE cmake --install build-steam --component Runtime --strip

echo "staged: $STAGE ($FEX_EAC_TAG)"
