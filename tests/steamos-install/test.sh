#!/bin/sh
# scripts/steamos/install-fex-tool.sh on a fake Steam FEX tool in a throw-away HOME: install backs up Valve's build once
# and swaps the stage in, including wrapper/config-only updates. Incomplete stages and a running FEXServer are refused.
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../scripts/steamos/install-fex-tool.sh"
d=$(mktemp -d); fail=0
trap 'rm -rf "$d"' EXIT HUP INT TERM
mkdir -p "$d/bin"
printf '#!/bin/sh\n[ "${MOCK_FEXSERVER:-0}" = 1 ]\n' > "$d/bin/pgrep"; chmod +x "$d/bin/pgrep"
PATH=$d/bin:$PATH; export PATH
BINS="FEX FEXBash FEXGetConfig FEXOfflineCompiler FEXServer FEXServerManager FEXpidof"
mk() { mkdir -p "$1/usr/bin"; for b in $BINS; do echo "$2 $b" > "$1/usr/bin/$b"; done; echo "$2" > "$1/FEXCompatTool"
       printf 'FEX describe\t%s\n' "$2" > "$1/VERSIONS.txt"; echo "{\"$2\":1}" > "$1/ConfigTemplate.json"; }
V=$d/home/.local/share/Steam/steamapps/common/FEX-Emu; N=$d/home/fex-eac-work/stage
mk "$V" FEX-valve; mk "$N" FEX-patched
run() { HOME=$d/home sh "$S" "$@" > "$d/out" 2>&1; }
ok() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; sed 's/^/     /' "$d/out"; fail=1; fi; }
is() { grep -q "^$2 FEX$" "$V/usr/bin/FEX" && grep -q "$2" "$V/VERSIONS.txt"; }
run restore; r=$?; [ $r != 0 ] && is x FEX-valve; ok $? "restore without a backup fails and leaves the tool alone"
run; [ $? = 0 ] && is x FEX-patched; ok $? "install swaps the stage in"
[ -f "$d/home/fex-eac-work/valve-backup/FEX-valve/usr/bin/FEX" ]; ok $? "install backs up Valve's build"
run; [ $? = 0 ] && grep -q "already installed" "$d/out"; ok $? "second install is a no-op"
for f in FEXCompatTool VERSIONS.txt ConfigTemplate.json; do
  printf '\nupdated\n' >> "$N/$f"; run; r=$?
  [ $r = 0 ] && cmp -s "$N/$f" "$V/$f" && ! grep -q "already installed" "$d/out"; ok $? "$f-only update is installed"
done
cp "$N/ConfigTemplate.json" "$d/config"; : > "$N/ConfigTemplate.json"; run; r=$?
[ $r != 0 ] && cmp -s "$d/config" "$V/ConfigTemplate.json"; ok $? "empty config is refused even when every binary matches"
cp "$d/config" "$N/ConfigTemplate.json"
MOCK_FEXSERVER=1 run restore; r=$?
[ $r != 0 ] && is x FEX-patched && grep -q "FEXServer running" "$d/out"; ok $? "restore refuses a running FEXServer, tool untouched"
run restore; [ $? = 0 ] && is x FEX-valve; ok $? "restore puts Valve's build back"
MOCK_FEXSERVER=1 run; r=$?
[ $r != 0 ] && is x FEX-valve && grep -q "FEXServer running" "$d/out"; ok $? "install refuses a running FEXServer, tool untouched"
rm "$N/usr/bin/FEXpidof"; run; r=$?; [ $r != 0 ] && is x FEX-valve; ok $? "incomplete stage is refused, tool untouched"
[ $fail = 0 ] && echo PASS || { echo FAILED; exit 1; }
