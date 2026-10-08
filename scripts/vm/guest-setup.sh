#!/bin/sh
# Runs as root INSIDE the muvm VM before the guest server starts (muvm -x). Generated wrapper exports:
#   FEX_OVERLAY_BIN  path of the patched FEX binary (host path, visible in the VM through the shared home)
#   REALISM          1 to also apply the optional environment-realism module
# 1. Wine/Proton need a large vm.max_map_count; 2. newly exec'd x86 processes must use the patched FEX.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)

/usr/sbin/sysctl -w vm.max_map_count=1048576 >/dev/null 2>&1

# The virtio-NIC comes up with a 64 KB MTU (guest-to-host loopback can take it, the real path to the
# internet cannot). Left alone, TCP emits ~43 KB super-segments that arrive as fragments and get
# retransmitted, and large UDP datagrams are dropped - seen as stutter and failed Photon region connects.
# Set the guest MTU to the real path MTU.
for IF in $(ls /sys/class/net 2>/dev/null | grep -v '^lo$'); do
  [ "$(cat /sys/class/net/$IF/mtu 2>/dev/null)" -gt 1500 ] 2>/dev/null && \
    ip link set dev "$IF" mtu 1500 >/dev/null 2>&1
done

# binfmt_misc handlers are registered with flag F (the interpreter inode is pinned), so swap them for the patched binary
if [ -n "${FEX_OVERLAY_BIN:-}" ] && [ -x "$FEX_OVERLAY_BIN" ]; then
  for n in x86 x86_64; do
    CONF=/usr/lib/binfmt.d/FEX-$n.conf
    [ -e /proc/sys/fs/binfmt_misc/FEX-$n ] && echo -1 > /proc/sys/fs/binfmt_misc/FEX-$n
    sed "s|/usr/bin/FEX|$FEX_OVERLAY_BIN|" "$CONF" > /proc/sys/fs/binfmt_misc/register
  done
fi

[ "${REALISM:-0}" = 1 ] && "$HERE/realism.sh"
exit 0
