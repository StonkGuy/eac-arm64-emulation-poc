#!/bin/bash
# Host-side memory tuning for running VRChat in a muvm VM on a 16 GB Apple-silicon Asahi machine.
#
#   sudo scripts/host-tune.sh            apply (runtime only, undone by reboot or `revert`)
#   sudo scripts/host-tune.sh revert     restore the values saved at apply time
#   sudo scripts/host-tune.sh status     show current values
#
# What it changes and why (all runtime sysctls, nothing is written to disk except the saved originals):
#
#  vm.watermark_boost_factor=0   An OOM report showed the min watermark inflated from 352 MB to 1.1 GB by
#                                "watermark boosting" (it triggers reclaim long before memory is really short and wastes
#                                ~800 MB of a 16 GB machine). 0 disables boosting.
#  vm.page-cluster=0             Swap in one page at a time. With zswap/zram in front of swap, readahead only wastes memory.
#  vm.swappiness=60              The distro default for Asahi is 10, which makes the kernel throw away page cache (game files)
#                                before compressing cold anonymous memory (Steam web helpers, idle browser tabs) into zswap.
#  nvme read_ahead_kb=256        The distro value is 4096 (4 MB per read-ahead!). For mmap'ed game assets and swap-in under memory
#                                pressure that evicts far more useful page cache than it prefetches.
#  /home mounted noatime         relatime still dirties btrfs metadata on reads of files not read recently.
#  zram swap (8 GB, zstd)        The 8 GB swapfile was completely full when the kernel OOM-killed the VM. zswap keeps swapped
#                                pages compressed in RAM but every page still needs a swap *slot*; zram adds 8 GB of slots at
#                                no disk cost (it only uses RAM for pages that actually get stored in it).
set -u
SAVE=/var/lib/vrc-host-tune.saved
KEYS="vm.watermark_boost_factor vm.page-cluster vm.swappiness"
NVME=/sys/block/nvme0n1/queue/read_ahead_kb

[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

status() {
  for k in $KEYS; do printf '%-28s %s\n' "$k" "$(sysctl -n $k)"; done
  echo "nvme read_ahead_kb=$(cat $NVME 2>/dev/null)  /home options: $(grep ' /home ' /proc/mounts | awk '{print $4}' | tr ',' '\n' | grep -E '^(no)?atime$' | tr '\n' ' ')"
  echo "zswap: enabled=$(cat /sys/module/zswap/parameters/enabled) compressor=$(cat /sys/module/zswap/parameters/compressor) max_pool=$(cat /sys/module/zswap/parameters/max_pool_percent)%"
  swapon --show
}

case "${1:-apply}" in
  status) status ;;
  apply)
    if [ ! -e "$SAVE" ]; then { for k in $KEYS; do echo "$k=$(sysctl -n $k)"; done; echo "read_ahead_kb=$(cat $NVME)"; echo "home_atime=$(grep ' /home ' /proc/mounts | awk '{print $4}' | tr ',' '\n' | grep -E '^(no)?atime$' | head -1)"; } > "$SAVE"; fi
    sysctl -w vm.watermark_boost_factor=0 vm.page-cluster=0 vm.swappiness=60 >/dev/null
    echo 256 > $NVME
    mount -o remount,noatime /home
    if ! swapon --show=NAME --noheadings | grep -q '^/dev/zram'; then
      modprobe zram num_devices=1 2>/dev/null || true
      if [ -e /sys/block/zram0 ]; then
        echo 1 > /sys/block/zram0/reset 2>/dev/null || true
        echo zstd > /sys/block/zram0/comp_algorithm 2>/dev/null || true
        echo 8G > /sys/block/zram0/disksize
        mkswap -L vrc-zram /dev/zram0 >/dev/null && swapon -p 100 /dev/zram0
      else
        echo "zram not available; skipped"
      fi
    fi
    status ;;
  revert)
    if [ -e "$SAVE" ]; then
      while IFS== read -r k v; do
        case "$k" in
          read_ahead_kb) echo "$v" > $NVME ;;
          home_atime) [ -n "$v" ] && mount -o remount,"$v" /home ;;
          *) sysctl -w "$k=$v" >/dev/null ;;
        esac
      done < "$SAVE"
    fi
    if swapon --show=NAME --noheadings | grep -q '^/dev/zram0'; then swapoff /dev/zram0 && echo 1 > /sys/block/zram0/reset; fi
    status ;;
  *) echo "usage: $0 [apply|revert|status]"; exit 2 ;;
esac
