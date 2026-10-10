#!/bin/sh
# scripts/set-launch-options.sh must change only VRChat's (appid 438100) LaunchOptions, never another app's, and must add
# the line when the VRChat block has none. Runs on a sample localconfig.vdf in a throw-away Steam root.
HERE=$(cd "$(dirname "$0")" && pwd); S="$HERE/../../scripts/set-launch-options.sh"
d=$(mktemp -d); mkdir -p "$d/userdata/1/config" "$d/userdata/2/config"; fail=0
app() { printf '\t\t\t\t"%s"\n\t\t\t\t{\n\t\t\t\t\t"LastPlayed"\t\t"1"\n%s\t\t\t\t}\n' "$1" "$2"; }
lo() { printf '\t\t\t\t\t"LaunchOptions"\t\t"%s"\n' "$1"; }
# user 1: a value mentioning 438100 before VRChat, VRChat with options, another app after it
{ printf '"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n'
  app 10 "$(lo 'keep-first')"; app 20 '\t\t\t\t\t"Note"\t\t"see 438100"\n'
  app 438100 "$(lo 'OLD')"; app 30 "$(lo 'keep-last')"
  printf '\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n'; } > "$d/userdata/1/config/localconfig.vdf"
# user 2: VRChat has no LaunchOptions; the next app does
{ printf '"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n\t\t\t\t"apps"\n\t\t\t\t{\n'
  app 438100 ''; app 30 "$(lo 'keep-last')"
  printf '\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n}\n'; } > "$d/userdata/2/config/localconfig.vdf"
STEAM_ROOT=$d VM_CPUS=4 VRC_REPORT_CPUS=0 sh "$S" DXVK_HUD=fps > /dev/null || { echo "FAIL script exited non-zero"; fail=1; }
chk() { if grep -q "$2" "$1"; then echo "ok   $3"; else echo "FAIL $3"; fail=1; fi; }
nochk() { if grep -q "$2" "$1"; then echo "FAIL $3"; fail=1; else echo "ok   $3"; fi; }
U1=$d/userdata/1/config/localconfig.vdf; U2=$d/userdata/2/config/localconfig.vdf
chk $U1 'keep-first' "user1: app before VRChat keeps its options"
chk $U1 'keep-last' "user1: app after VRChat keeps its options"
nochk $U1 '"OLD"' "user1: VRChat's old options replaced"
chk $U1 'DXVK_HUD=fps' "user1: VRChat has the new options"
chk $U2 'keep-last' "user2: the app after a VRChat block without options is untouched"
chk $U2 'DXVK_HUD=fps' "user2: VRChat gets a LaunchOptions line"
[ "$(grep -c LaunchOptions $U2)" = 2 ] && echo "ok   user2: exactly two LaunchOptions lines" || { echo "FAIL user2: LaunchOptions count"; fail=1; }
rm -rf "$d"; [ $fail = 0 ] && echo PASS || { echo FAILED; exit 1; }
