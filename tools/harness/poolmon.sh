#!/bin/sh
# In the VM (as root): once per second, count the game's IL2CPP thread-pool workers and total threads (thread names only,
# from /proc; passive). usage: poolmon.sh OUTFILE [SECONDS=150]
O=$1; S=${2:-150}; : > $O.tmp
for i in $(seq 1 $S); do
  P=$(pgrep -x VRChat.exe | head -1)
  if [ -n "$P" ]; then
    n=0; t=0; for c in /proc/$P/task/*/comm; do t=$((t+1)); case "$(cat $c 2>/dev/null)" in "IL2CPP Threadpo"*) n=$((n+1));; esac; done
    echo "$i $n $t" >> $O.tmp
  fi
  sleep 1
done; mv $O.tmp $O
