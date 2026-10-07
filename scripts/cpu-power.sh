#!/bin/bash
# CPU frequency policy for a fanless Apple-silicon laptop (Asahi Linux), runtime only.
#
#   scripts/cpu-power.sh status                 show governors and clocks (no root needed)
#   sudo scripts/cpu-power.sh cap MHZ           limit the performance cluster's maximum clock (for example 2400)
#   sudo scripts/cpu-power.sh governor NAME     set the cpufreq governor of every CPU (performance, schedutil, ...)
#   sudo scripts/cpu-power.sh revert            restore the values saved by the first change
#
# Why: a MacBook Air has no fan. Linux (unlike macOS) has no closed-loop thermal control for the SoC, so a game that keeps
# the performance cores at their top clock heats the chip until the firmware clamps every core to its lowest clock for a few
# seconds, over and over (see docs/troubleshooting.md). A lower fixed maximum clock keeps the chip below that limit.
set -u
SAVE=/var/lib/vrc-cpu-power.saved
CPUS=$(ls -d /sys/devices/system/cpu/cpu[0-9]* | sed 's#.*/cpu##' | sort -n)

status() {
  for c in $CPUS; do
    d=/sys/devices/system/cpu/cpu$c/cpufreq
    [ -d $d ] && printf 'cpu%-2s cur %7s kHz  max %7s kHz  governor %-12s cluster %s\n' $c "$(cat $d/scaling_cur_freq)" "$(cat $d/scaling_max_freq)" "$(cat $d/scaling_governor)" "$(cat $d/related_cpus)"
  done
}
save() {
  [ -e "$SAVE" ] && return
  for c in $CPUS; do d=/sys/devices/system/cpu/cpu$c/cpufreq; echo "$c $(cat $d/scaling_governor) $(cat $d/scaling_max_freq)"; done > "$SAVE"
}
need_root() { [ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }; }

case "${1:-status}" in
  status) status ;;
  cap)
    need_root; [ -n "${2:-}" ] || { echo "usage: cap MHZ"; exit 1; }; save
    khz=$(( $2 * 1000 ))
    for c in $CPUS; do
      d=/sys/devices/system/cpu/cpu$c/cpufreq
      # the performance cluster is the one whose maximum is above the efficiency cluster's
      [ "$(cat $d/cpuinfo_max_freq)" -gt 2500000 ] && echo "$khz" > $d/scaling_max_freq
    done
    status ;;
  governor)
    need_root; [ -n "${2:-}" ] || { echo "usage: governor NAME"; exit 1; }; save
    for c in $CPUS; do echo "$2" > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor; done
    status ;;
  revert)
    need_root; [ -e "$SAVE" ] || { echo "nothing saved"; exit 0; }
    while read -r c gov max; do
      d=/sys/devices/system/cpu/cpu$c/cpufreq; echo "$gov" > $d/scaling_governor; echo "$max" > $d/scaling_max_freq
    done < "$SAVE"
    rm -f "$SAVE"; status ;;
  *) sed -n 2,9p "$0"; exit 1 ;;
esac
