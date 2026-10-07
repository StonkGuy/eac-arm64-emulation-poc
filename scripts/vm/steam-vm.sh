#!/bin/sh
# Starts the muvm VM that runs Steam (x86-64 Steam through FEX, the way the distro's `steam` wrapper does) with
#   * the patched FEX registered as the x86 interpreter inside the VM,
#   * a leaner memory profile (guest RAM and the "VRAM" the GPU driver reports are capped),
#   * the VM in its own systemd scope with high CPU/IO weight and memory protection.
#
#   scripts/vm/steam-vm.sh [steam args...]
# Environment: VM_MEM_MB (8192) VM_VRAM_MB (4096) REALISM (0|1) FEX_OVERLAY_DIR VM_MEMLOW (8G)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
STATE=${XDG_STATE_HOME:-$HOME/.local/state}/vrchat-fex-eac; mkdir -p "$STATE"
OVERLAY=${FEX_OVERLAY_DIR:-$HOME/.local/share/vrchat-fex-eac/fex}/FEX
[ -x "$OVERLAY" ] || { echo "patched FEX missing; run scripts/build-fex.sh and scripts/vm/install-overlay.sh"; exit 1; }
STEAM_SH=$HOME/.local/share/fex-steam/steam-launcher/bin_steam.sh
[ -x "$STEAM_SH" ] || { echo "run the distro's 'steam' once first so it installs $STEAM_SH"; exit 1; }

cat > "$STATE/guest-setup.sh" <<WRAP
#!/bin/sh
export FEX_OVERLAY_BIN='$OVERLAY' REALISM='${REALISM:-0}'
exec '$HERE/guest-setup.sh'
WRAP
chmod +x "$STATE/guest-setup.sh"

rm -rf "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/krun"   # a stale muvm server socket makes `muvm` talk to a dead VM
exec systemd-run --user --scope --quiet -p CPUWeight=1000 -p IOWeight=1000 -p "MemoryLow=${VM_MEMLOW:-8G}" \
  muvm -x "$STATE/guest-setup.sh" --mem "${VM_MEM_MB:-8192}" --vram "${VM_VRAM_MB:-4096}" -- \
  FEXBash -c "$STEAM_SH -cef-force-occlusion $*"
