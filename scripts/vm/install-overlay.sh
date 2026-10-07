#!/bin/sh
# Installs the patched FEX binary where the VM launcher expects it (default ~/.local/share/vrchat-fex-eac/fex/FEX).
#   scripts/vm/install-overlay.sh [path/to/FEX]
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${1:-$REPO/work/FEX/build/Bin/FEX}
DST=${FEX_OVERLAY_DIR:-$HOME/.local/share/vrchat-fex-eac/fex}
[ -x "$SRC" ] || { echo "no FEX binary at $SRC (run scripts/build-fex.sh)"; exit 1; }
mkdir -p "$DST"
cp "$SRC" "$DST/FEX.new" && chmod +x "$DST/FEX.new" && mv -f "$DST/FEX.new" "$DST/FEX"   # atomic: running VMs keep the old inode
echo "installed $DST/FEX"
echo "note: a running VM keeps using the old binary until its binfmt handlers are re-registered (restart the VM)."
