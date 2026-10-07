#!/usr/bin/env python3
# Runs on the host: every 0.5 s, memory/reclaim/PSI counters and CPU time of the VM process threads. usage: hosttl.py SECONDS OUTFILE
import os, sys, time, subprocess
secs = float(sys.argv[1]); out = open(sys.argv[2], 'w', buffering=1)
def rd(p):
    try:
        with open(p) as f: return f.read()
    except Exception: return ''
def vmpid():
    r2 = subprocess.run(['bash', '-c', "ps -eo pid,comm | awk '$2 ~ /^VM:/ {print $1}' | head -1"], capture_output=True, text=True).stdout.split()
    return int(r2[0]) if r2 else 0
VP = vmpid()
KEYS = ('pswpin', 'pswpout', 'zswpin', 'zswpout', 'pgmajfault', 'allocstall_normal', 'allocstall_movable', 'pgscan_direct', 'pgscan_kswapd', 'pgsteal_direct', 'pgsteal_kswapd', 'compact_stall', 'workingset_refault_anon', 'workingset_refault_file')
end = time.time() + secs
while time.time() < end:
    now = time.time(); v = {}
    for l in rd('/proc/vmstat').splitlines():
        k, x = l.split()
        if k in KEYS: v[k] = x
    def psi(n):
        r = rd('/proc/pressure/' + n).splitlines()
        return ' '.join(x.split('total=')[1] for x in r if 'total=' in x) if r else '0 0'
    mi = rd('/proc/meminfo'); a = {l.split(':')[0]: l.split()[1] for l in mi.splitlines()}
    thr = []
    if VP:
        for t in os.listdir('/proc/%d/task' % VP):
            s = rd('/proc/%d/task/%s/stat' % (VP, t))
            if not s: continue
            f = s.rsplit(')', 1)[1].split()
            tk = int(f[11]) + int(f[12])
            if tk > 0: thr.append('%s:%d' % (t, tk))
    out.write('H %.2f avail=%s swapfree=%s cpu_psi=%s mem_psi=%s io_psi=%s %s\n' % (now, a.get('MemAvailable'), a.get('SwapFree'), psi('cpu'), psi('memory'), psi('io'), ' '.join('%s=%s' % kv for kv in v.items())))
    out.write('V %.2f %s\n' % (now, ' '.join(thr)))
    time.sleep(max(0, 0.5 - (time.time() - now)))
