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
for CF in "$STEAM"/userdata/*/config/localconfig.vdf; do
  [ -f "$CF" ] || continue
  grep -q '"438100"' "$CF" || { echo "skip $CF (no VRChat entry yet: launch it once from Steam)"; continue; }
  cp "$CF" "$CF.bak-vrchat-fex-eac"
  sed -i "/\"438100\"/,/\"LaunchOptions\"/s|\"LaunchOptions\"[[:space:]]*\".*\"|\"LaunchOptions\"\t\t\"$OPTS\"|" "$CF"
  echo "updated $CF"
done
