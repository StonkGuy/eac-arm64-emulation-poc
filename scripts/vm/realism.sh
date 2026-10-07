#!/bin/sh
# OPTIONAL, runs as root inside the VM. Makes the guest look less like a microVM to software that checks, following the
# hardware-realism advice in VRChat's "Using VRChat in a Virtual Machine" guide: plausible SMBIOS/DMI strings, a PCI device
# list, a systemd-looking PID 1 and a desktop-style hostname. It does not touch the anti-cheat or its results.
# Override values through environment variables (REALISM_*). Unverified whether it is still necessary; see docs/status.md.
set -u
R=${REALISM_DIR:-/tmp/vrchat-fex-eac-realism}
rm -rf "$R"; mkdir -p "$R/virt/dmi/id" "$R/class/dmi/id" "$R/pci"

put() { for d in "$R/virt/dmi/id" "$R/class/dmi/id"; do printf '%s\n' "$2" > "$d/$1"; done; }
put sys_vendor      "${REALISM_VENDOR:-Gigabyte Technology Co., Ltd.}"
put product_name    "${REALISM_PRODUCT:-Z490 AORUS ELITE}"
put product_version "${REALISM_PRODUCT_VERSION:--CF}"
put board_vendor    "${REALISM_VENDOR:-Gigabyte Technology Co., Ltd.}"
put board_name      "${REALISM_PRODUCT:-Z490 AORUS ELITE}"
put board_version   "${REALISM_PRODUCT_VERSION:--CF}"
put bios_vendor     "${REALISM_BIOS_VENDOR:-American Megatrends International, LLC.}"
put bios_version    "${REALISM_BIOS_VERSION:-F10}"
put bios_date       "${REALISM_BIOS_DATE:-07/01/2021}"
put bios_release    "5.17"
put chassis_type    "3"
for f in board_serial board_asset_tag product_serial product_family product_sku chassis_vendor chassis_version chassis_serial chassis_asset_tag; do put $f "Default string"; done
mount -t overlay overlay -o "lowerdir=$R/virt:/sys/devices/virtual" /sys/devices/virtual 2>/dev/null
mount -t overlay overlay -o "lowerdir=$R/class:/sys/class" /sys/class 2>/dev/null

# PCI device list (host bridge, ISA bridge, a GPU-looking function)
if [ -d /proc/bus ]; then
  mount -t tmpfs tmpfs /proc/bus && mkdir -p /proc/bus/pci
  printf '0000\t80869b33\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t\n0008\t80869b44\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t\n0010\t10de1e87\t10\td0000000\tc0000000\t0\t0\t0\t0\t0\t1000000\t10000000\t0\t0\t0\t0\t0\t\n' > /proc/bus/pci/devices
fi

# PID 1 looks like systemd
printf 'systemd\n' > "$R/comm"; printf '/usr/lib/systemd/systemd\0' > "$R/cmdline"
mount --bind "$R/comm" /proc/1/comm 2>/dev/null
mount --bind "$R/cmdline" /proc/1/cmdline 2>/dev/null

HN=${REALISM_HOSTNAME:-GAMING-PC}
hostname "$HN" 2>/dev/null; echo "$HN" > /etc/hostname 2>/dev/null
[ -e /etc/mtab ] || ln -s /proc/self/mounts /etc/mtab 2>/dev/null
exit 0
