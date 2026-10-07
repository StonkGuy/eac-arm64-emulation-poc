#!/bin/sh
# Host-side memory watchdog for the muvm guest. The guest has no swap (stripped muvm kernel: no zram, no
# virtio_balloon) and its `/` is virtiofs onto the host disk, so once VRChat's working set (~5 GB anon) plus
# Steam/Wine fills the guest, the *guest kernel's own OOM killer* kills VRChat.exe -- the game "joins then dies".
# This watches the guest's available memory and posts a desktop notification before that happens, and reports an
# actual OOM kill (and host memory pressure) if it does. Read-only: it never changes VM state.
#
#   scripts/vm/oom-watch.sh [POLL_SECONDS]        (default 15)
# Environment: OOM_WATCH_LOW_MB (500) guest-available warning, OOM_WATCH_CRIT_MB (200) critical,
#              OOM_WATCH_HOST_MB (900) host MemAvailable warning, OOM_WATCH_NO_DESKTOP=1 to only print.
set -u
POLL=${1:-15}
LOW=${OOM_WATCH_LOW_MB:-500}
CRIT=${OOM_WATCH_CRIT_MB:-200}
HOST_LOW=${OOM_WATCH_HOST_MB:-900}

notify() { # $1=urgency $2=title $3=body
  printf '%s [%s] %s: %s\n' "$(date +%T)" "$1" "$2" "$3"
  [ "${OOM_WATCH_NO_DESKTOP:-0}" = 1 ] && return 0
  command -v notify-send >/dev/null 2>&1 && notify-send -u "$1" -a VRChat-Asahi "$2" "$3" 2>/dev/null
}

# muvm's stdout capture is unreliable, but the guest's /home is the same virtiofs the host mounts, so have the guest
# write the numbers to a file and read it back here.
QFILE=${XDG_CACHE_HOME:-$HOME/.cache}/vrchat-fex-eac/.oomwatch
mkdir -p "$(dirname "$QFILE")" 2>/dev/null
guest_query() { # prints "<MemAvailable_MB> <oom_vrchat_count>"
  rm -f "$QFILE" 2>/dev/null
  timeout 20 muvm --privileged -- /bin/sh -c \
    "a=\$(free -m | awk '/Mem:/{print \$7}'); n=\$(dmesg 2>/dev/null | grep -c 'Out of memory: Killed process.*VRChat'); echo \"\$a \$n\" > '$QFILE'" >/dev/null 2>&1
  tr -d '\r' < "$QFILE" 2>/dev/null
}

state=ok; last_oom=0; first=1
printf 'oom-watch: polling %ss (low<%sMB crit<%sMB host<%sMB)\n' "$POLL" "$LOW" "$CRIT" "$HOST_LOW"
while :; do
  if ! pgrep -f '[m]uvm -x' >/dev/null 2>&1; then
    [ "$state" != novm ] && notify low "VRChat memory watch" "VM is not running; watchdog stopped."
    exit 0
  fi
  q=$(guest_query); avail=${q%% *}; oom=${q##* }
  hostavail=$(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo)

  if [ "$first" = 1 ] && [ -n "$oom" ]; then last_oom=$oom; first=0; fi

  if [ -n "$oom" ] && [ "$oom" -gt "$last_oom" ]; then
    last_oom=$oom
    notify critical "VRChat was killed by the guest OOM killer" \
      "The guest ran out of memory (no swap). Relaunch with a larger guest: --mem 8192 or more, and quit between instances."
    state=oom; sleep "$POLL"; continue
  fi

  case "$avail" in ''|*[!0-9]*) : ;;  # transient decode failure: skip this tick
  *)
    if [ "$avail" -lt "$CRIT" ] && [ "$state" != crit ]; then
      notify critical "VRChat guest memory critical (${avail} MB free)" "One world transition away from an OOM kill. Quit to desktop soon."
      state=crit
    elif [ "$avail" -lt "$LOW" ] && [ "$state" != low ] && [ "$state" != crit ]; then
      notify normal "VRChat guest memory low (${avail} MB free)" "Consider quitting unused worlds/apps."
      state=low
    elif [ "$avail" -ge "$LOW" ] && [ "$state" != ok ]; then
      state=ok
    fi
  ;; esac

  if [ "$hostavail" -lt "$HOST_LOW" ] && [ "$state" != hostlow ]; then
    notify critical "Host memory low (${hostavail} MB available)" "The VM and the desktop are competing; free RAM or shut the VM down."
    state=hostlow
  fi
  sleep "$POLL"
done
