#!/bin/sh
# Regenerates patches/ from the patched FEX: one file per commit of StonkGuy/FEX between FEX-2610 and the pinned tag,
# leaving out merge commits and the commits that only add eac-tests/, EAC-README.md or CI (.github/), and the eac-tests/ part of a commit that also
# changes source (each patch holds only the FEX source change; the tests live in the tag's eac-tests/). patches/withdrawn/ and patches/README.md stay.
#   scripts/export-patches.sh
# The tag and commit are the ones scripts/build-fex.sh pins; NUMBERS are the patch numbers docs/patches.md uses, in commit order (since eac-2610.2 patches arrive through merged
# pull requests, so commit order is not number order).
set -eu
REPO=$(cd "$(dirname "$0")/.." && pwd)
eval "$(grep -E '^(FEX_REPO|FEX_EAC_TAG|FEX_EAC_SHA)=' "$REPO/scripts/build-fex.sh")"
BASE=14c92681f4d62cf84d901460e0358de09c8847a7
NUMBERS="0001 0002 0003 0004 0005 0006 0007 0008 0009 0010 0011 0012 0013 0014 0015 0016 0017 0018 0019 0020
0022 0023 0024 0025 0026 0033 0035 0036 0037 0041 0042 0043 0044 0045 0046 0047 0038 0039 0048 0049 0050 0051 0052 0053"

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
git init -q "$T"
git -C "$T" fetch -q --depth 200 "$FEX_REPO" "refs/tags/$FEX_EAC_TAG:refs/tags/$FEX_EAC_TAG"
[ "$(git -C "$T" rev-parse "$FEX_EAC_TAG^{commit}")" = "$FEX_EAC_SHA" ] || { echo "$FEX_EAC_TAG is not $FEX_EAC_SHA" >&2; exit 1; }
git -C "$T" cat-file -e "$BASE^{commit}" || { echo "$BASE not within 200 commits of $FEX_EAC_TAG" >&2; exit 1; }

COMMITS=$(git -C "$T" rev-list --reverse --no-merges "$BASE..$FEX_EAC_SHA" -- . ':!eac-tests' ':!EAC-README.md' ':!.github')
set -- $NUMBERS
[ "$(echo "$COMMITS" | wc -l)" -eq $# ] || { echo "$(echo "$COMMITS" | wc -l) commits, $# numbers" >&2; exit 1; }
rm -f "$REPO"/patches/[0-9]*.patch
for c in $COMMITS; do
  f=$(git -C "$T" format-patch -q -1 "$c" --start-number "$(echo "$1" | sed 's/^0*//')" -o "$REPO/patches" -- . ':!eac-tests' ':!EAC-README.md' ':!.github')
  echo "$(basename "$f")"; shift
done
