#!/bin/sh
# Sets VRChat's Steam launch options (appid 438100) in every Steam user's localconfig.vdf. Steam must NOT be running
# (it rewrites the file on exit). A backup is kept next to the file.
#   scripts/set-launch-options.sh [extra env assignments, e.g. DXVK_HUD=fps]
set -eu
STEAM=${STEAM_ROOT:-$HOME/.local/share/Steam}
EAC_DIR="$STEAM/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat"
RUNTIME="$STEAM/steamapps/common/Proton EasyAntiCheat Runtime"
HERE=$(cd "$(dirname "$0")" && pwd)
OPTS="env EAC_LAUNCHERDIR=$EAC_DIR PROTON_EAC_RUNTIME='$RUNTIME' WINEDEBUG=-all PROTON_USE_XALIA=0 DXVK_CONFIG_FILE=$HERE/dxvk.conf $* %command%"
pgrep -x steam >/dev/null 2>&1 && { echo "close Steam first"; exit 1; }
for CF in "$STEAM"/userdata/*/config/localconfig.vdf; do
  [ -f "$CF" ] || continue
  grep -q '"438100"' "$CF" || { echo "skip $CF (no VRChat entry yet: launch it once from Steam)"; continue; }
  cp "$CF" "$CF.bak-vrchat-fex-eac"
  sed -i "/\"438100\"/,/\"LaunchOptions\"/s|\"LaunchOptions\"[[:space:]]*\".*\"|\"LaunchOptions\"\t\t\"$OPTS\"|" "$CF"
  echo "updated $CF"
done
