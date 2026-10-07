#!/usr/bin/env python3
"""Log the Apple SMC's power readings twice a second (Asahi Linux, macsmc hwmon + battery).

usage: powertl.py SECONDS OUTFILE      line format: P <epoch> sys=<uW> ac=<uW> heat=<uW> acv=<mV> aci=<mA> batv=<mV> batw=<uW> ...

Why: a MacBook that is "plugged in" to a weak source (a 5 V USB port, a hub, a 15 W phone charger) keeps running at full
speed on the battery for ~15 s and is then throttled to the adapter's power for ~12 s, over and over. The game's frame
rate swings between ~45 and ~20 FPS with the same period. `ac` (AC input power) pinned at a low constant while `sys`
alternates between two levels is the signature. Compare with tools/canary.c, which measures the CPU speed directly.
"""
import glob, sys, time

def find_hwmon(name):
    for d in glob.glob('/sys/class/hwmon/hwmon*'):
        try:
            if open(d + '/name').read().strip() == name: return d + '/'
        except OSError: pass
    return None

HW = find_hwmon('macsmc_hwmon'); BAT = '/sys/class/power_supply/macsmc-battery/'
if not HW: sys.exit('no macsmc_hwmon found: this tool is for Apple-silicon machines running Asahi Linux')

def rd(p):
    try:
        with open(p) as f: return f.read().strip()
    except OSError: return '0'

secs = float(sys.argv[1]); out = open(sys.argv[2], 'w', buffering=1)
end = time.time() + secs
while time.time() < end:
    now = time.time()
    out.write('P %.2f sys=%s ac=%s heat=%s acv=%s aci=%s batv=%s batw=%s batI=%s nand=%s chg=%s wifi=%s soc=%s\n' % (
        now, rd(HW + 'power1_input'), rd(HW + 'power2_input'), rd(HW + 'power3_input'), rd(HW + 'in0_input'), rd(HW + 'curr1_input'),
        rd(HW + 'in1_input'), rd(BAT + 'power_now'), rd(BAT + 'current_now'), rd(HW + 'temp1_input'), rd(HW + 'temp3_input'), rd(HW + 'temp4_input'), rd(BAT + 'capacity')))
    time.sleep(max(0, 0.5 - (time.time() - now)))
