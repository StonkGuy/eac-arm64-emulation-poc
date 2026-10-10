#!/bin/sh
# Sets VRChat's Steam launch options (appid 438100) in every Steam user's localconfig.vdf. Steam must NOT be running
# (it rewrites the file on exit). A backup is kept next to the file.
#   scripts/set-launch-options.sh [extra env assignments, e.g. DXVK_HUD=fps]
set -eu
STEAM=${STEAM_ROOT:-$HOME/.local/share/Steam}
EAC_DIR="$STEAM/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat"
RUNTIME="$STEAM/steamapps/common/Proton EasyAntiCheat Runtime"
HERE=$(cd "$(dirname "$0")" && pwd)
# Report more CPUs to Windows code than the VM has. The game's IL2CPP thread pool starts with one worker per reported CPU,
# and muvm gives the guest only the host's performance cores (4 on an M2): with 4 workers the start-up burst starves the
# pool and the Photon region lookup times out (~60 s freeze, then a "Disconnecting" rejoin loop; docs/disconnects.md).
# WINE_CPU_TOPOLOGY is Proton's own setting: N logical CPUs mapped onto guest CPU ids (repeats allowed).
#   VRC_REPORT_CPUS (16; 0 = do not set)   VM_CPUS (guest CPU count; default = host performance cores, as muvm picks them)
REPORT=${VRC_REPORT_CPUS:-16}
GUEST=${VM_CPUS:-$(n=0; for f in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/cpuinfo_max_freq; do [ "$(cat "$f")" -gt 2500000 ] && n=$((n + 1)); done; echo $n)}
[ "$GUEST" -ge 1 ] 2>/dev/null || GUEST=$(nproc)
TOPO=""
if [ "$REPORT" -gt "$GUEST" ]; then
  ids=""; i=0; while [ $i -lt "$REPORT" ]; do ids="$ids${ids:+,}$((i % GUEST))"; i=$((i + 1)); done
  TOPO="WINE_CPU_TOPOLOGY=$REPORT:$ids "
fi
OPTS="env EAC_LAUNCHERDIR=$EAC_DIR PROTON_EAC_RUNTIME='$RUNTIME' WINEDEBUG=-all PROTON_USE_XALIA=0 DXVK_CONFIG_FILE=$HERE/dxvk.conf $TOPO$* %command%"
pgrep -x steam >/dev/null 2>&1 && { echo "close Steam first"; exit 1; }
# Only the app block of 438100 is edited: the block is found by its key line and followed by brace depth, so another
# app's LaunchOptions is never touched, and a VRChat block without one gets a LaunchOptions line added.
set_options() {
  OPTS="$OPTS" awk '
    function emit() { printf "\t\t\t\t\t\"LaunchOptions\"\t\t\"%s\"\n", ENVIRON["OPTS"]; done = 1 }
    inapp && $0 ~ /^[ \t]*\{[ \t]*$/ { d++; print; next }
    inapp && $0 ~ /^[ \t]*\}[ \t]*$/ { d--; if (d == 0) { if (!done) emit(); inapp = 0 } print; next }
    inapp && d == 1 && $0 ~ /^[ \t]*"LaunchOptions"[ \t]/ { emit(); next }
    pending { pending = 0; if ($0 ~ /^[ \t]*\{[ \t]*$/) { inapp = 1; d = 1; done = 0; print; next } }
    $0 ~ /^[ \t]*"438100"[ \t]*$/ { pending = 1 }
    { print }
  ' "$1"
}
for CF in "$STEAM"/userdata/*/config/localconfig.vdf; do
  [ -f "$CF" ] || continue
  grep -Eq '^[[:space:]]*"438100"[[:space:]]*$' "$CF" || { echo "skip $CF (no VRChat entry yet: launch it once from Steam)"; continue; }
  cp "$CF" "$CF.bak-vrchat-fex-eac"
  set_options "$CF.bak-vrchat-fex-eac" > "$CF.new" && mv "$CF.new" "$CF"
  echo "updated $CF"
done
