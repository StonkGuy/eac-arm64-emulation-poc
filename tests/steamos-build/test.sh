#!/bin/sh
# Check SteamOS build orchestration without cloning or compiling FEX.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$HERE/../../scripts/steamos/build-fex.sh
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT HUP INT TERM
mkdir -p "$d/bin"
cat > "$d/bin/git" <<'EOF'
#!/bin/sh
{ printf 'git'; printf ' <%s>' "$@"; printf '\n'; } >> "$MOCK_LOG"
case "$1" in
clone) for target do :; done; mkdir -p "$target/.git";;
rev-parse) if [ "$2" = HEAD ]; then echo "${MOCK_HEAD:-39107ddbb1f1f9ad3778a5aba65f9947cd7e84ce}"; else echo 39107ddbb1f1f9ad3778a5aba65f9947cd7e84ce; fi;;
esac
EOF
cat > "$d/bin/cmake" <<'EOF'
#!/bin/sh
{ printf 'cmake'; printf ' <%s>' "$@"; printf '\n'; } >> "$MOCK_LOG"
if [ "$1" = --install ]; then
  mkdir -p "$DESTDIR"
  echo staged > "$DESTDIR/marker"
fi
EOF
printf '#!/bin/sh\necho 3\n' > "$d/bin/nproc"
chmod +x "$d/bin/"*
export PATH="$d/bin:$PATH" WORK="$d/work with spaces" MOCK_LOG="$d/commands"
sh "$SCRIPT" > "$d/out" 2>&1 || { cat "$d/out"; exit 1; }
grep -q '^git <clone> <--depth> <1> <--branch> <eac-2610.3>' "$MOCK_LOG"
! grep -q '<--recurse-submodules>' "$MOCK_LOG"
echo 'ok   clone fetches only the pinned tag without unused submodules'
grep -q '^git <submodule> <update> <--init> <--recursive> <--depth> <1> <--> <Source/Common/cpp-optparse> <External/fmt> <External/drm-headers> <External/xxhash> <External/jemalloc_glibc> <External/range-v3> <External/unordered_dense> <External/rpmalloc>$' "$MOCK_LOG"
echo 'ok   shallow-fetches only the eight Runtime dependencies'
grep -q '^cmake <--build> <build-steam> <--target> <FEX> <FEXBash> <FEXGetConfig> <FEXOfflineCompiler> <FEXServer> <FEXServerManager> <FEXpidof> <FEXCompatTool> <-j3>$' "$MOCK_LOG"
echo 'ok   builds only the eight runtime tools with the detected job count'
grep -q '^cmake <--install> <build-steam> <--component> <Runtime> <--strip>$' "$MOCK_LOG"
[ -s "$WORK/stage/marker" ]
echo 'ok   strips only the staged Runtime component'
: > "$MOCK_LOG"
JOBS=2 sh "$SCRIPT" > "$d/out" 2>&1 || { cat "$d/out"; exit 1; }
! grep -q '^git <clone>' "$MOCK_LOG"
grep -q '^git <submodule> <update>' "$MOCK_LOG"
grep -q '<-j2>$' "$MOCK_LOG"
echo 'ok   reuses an existing checkout and respects JOBS'
: > "$MOCK_LOG"
MOCK_HEAD=old-clean-checkout sh "$SCRIPT" > "$d/out" 2>&1 || { cat "$d/out"; exit 1; }
grep -q '^git <checkout> <-q> <eac-2610.3>$' "$MOCK_LOG"
grep -q '^git <submodule> <update>.*<Source/Common/cpp-optparse>.*<External/rpmalloc>$' "$MOCK_LOG"
echo 'ok   clean checkout migration uses the same selected dependencies'
echo PASS
