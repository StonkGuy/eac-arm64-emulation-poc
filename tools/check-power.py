#!/usr/bin/env python3
"""Is the power source good enough for gaming? (Apple silicon, Asahi Linux)

Loads all performance cores for ~40 s and watches the SMC's power readings. Prints a verdict:
  * AC input power while loaded (a MacBook Air M2 under a game wants ~25-30 W; the 30 W charger is the minimum),
  * how much the battery had to make up,
  * whether the system had to throttle to the adapter's limit (system power collapsing while the load is unchanged).
usage: check-power.py [seconds]      (stop other heavy programs first; takes the machine to full CPU load)
"""
import glob, os, statistics, subprocess, sys, time

def hw(name):
    for d in glob.glob('/sys/class/hwmon/hwmon*'):
        try:
            if open(d + '/name').read().strip() == name: return d + '/'
        except OSError: pass

HW = hw('macsmc_hwmon'); BAT = '/sys/class/power_supply/macsmc-battery/'
if not HW: sys.exit('macsmc_hwmon not found: needs an Apple-silicon machine running Asahi Linux')
rd = lambda p: int(open(p).read().strip() or 0)
caps = {}
for c in glob.glob('/sys/devices/system/cpu/cpu[0-9]*/cpu_capacity'):
    caps[int(c.split('/')[-2][3:])] = rd(c)
top = max(caps.values()) if caps else 0
cores = sorted(c for c, v in caps.items() if v == top) or list(range(os.cpu_count()))
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 40

def power(): return rd(HW + 'power1_input') / 1e6, rd(HW + 'power2_input') / 1e6, rd(BAT + 'power_now') / 1e6
idle = power()
print('idle: system %.1f W, AC input %.1f W, battery %+.1f W   (AC online: %s)' % (idle + (open('/sys/class/power_supply/macsmc-ac/online').read().strip(),)))
print('loading cores %s for %.0f s ...' % (cores, secs))
procs = [subprocess.Popen([sys.executable, '-c', 'import os\nos.sched_setaffinity(0,{%d})\nwhile True: pass' % c]) for c in cores]
S = []
try:
    t0 = time.time()
    while time.time() - t0 < secs:
        S.append((time.time() - t0,) + power()); time.sleep(0.5)
finally:
    for p in procs: p.kill()
sysw = [s[1] for s in S]; ac = [s[2] for s in S]; bat = [s[3] for s in S]
hi, lo = max(sysw), min(sysw[len(sysw) // 4:] or sysw)
print('under load: system %.1f..%.1f W (median %.1f), AC input median %.1f W (max %.1f), battery median %+.1f W' % (min(sysw), hi, statistics.median(sysw), statistics.median(ac), max(ac), statistics.median(bat)))
print('timeline (system W every 2 s): ' + ' '.join('%.0f' % s[1] for s in S[::4]))
weak = statistics.median(ac) < 25 and statistics.median(bat) < -2
throttled = hi > 1.5 * lo and hi - lo > 6
if weak or throttled:
    print('\nVERDICT: the power source is too weak. AC input tops out at %.1f W while the machine wants %.0f W; the difference comes from the battery' % (max(ac), hi))
    print('and, when that runs out of its short allowance, the SoC is throttled hard (CPU ~5x slower for ~10-15 s at a time). That shows up as a frame rate')
    print('that swings between ~45 and ~20 FPS and as network time-outs. Use the 30 W+ USB-C PD charger (or MagSafe) in the Mac\'s own port, no hub or PC port.')
else:
    print('\nVERDICT: no sign of power limiting under full CPU load.')
