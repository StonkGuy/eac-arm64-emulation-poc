#!/usr/bin/env python3
"""Tally outcomes of instrumented sessions. usage: classify_runs.py PREFIX [PREFIX...]  (reads <PREFIX><n>.log)"""
import sys, re, glob, datetime
STEPS = [('Stomp', 'Successfully connected to Stomp'), ('Region', 'OnRegionListReceived'), ('DestSet', 'Destination set'), ('EAC', 'AntiCheat Session Begin: Success'),
         ('Instant', 'Room instantiate took'), ('InWorld', 'Finished entering world'), ('Timeout', 'ClientTimeout')]
def ts(l):
    m = re.match(r'(\d{4})\.(\d\d)\.(\d\d) (\d\d):(\d\d):(\d\d) ', l)
    return datetime.datetime(*map(int, m.groups())).timestamp() if m else None
tot = {}
by_arm = {}
print('%-6s %-9s %-6s %-6s %-7s %-5s %-7s %-7s %s' % ('run', 'outcome', 'Stomp', 'Region', 'DestSet', 'EAC', 'InWorld', 'timeouts', 'notes'))
for pre in sys.argv[1:]:
    for f in sorted(glob.glob(pre + '[0-9]*.log'), key=lambda x: int(re.search(r'\d+', x[len(pre):]).group())):
        L = f[:-4]; lines = open(f, errors='replace').read().splitlines()
        first = ts(lines[0]) if lines else None; t = {}
        for l in lines:
            for k, pat in STEPS:
                if pat in l and k not in t: t[k] = (ts(l) or 0) - (first or 0)
        n_to = sum('ClientTimeout' in l for l in lines)
        last = ts(lines[-1]) - first if lines and first else 0
        if n_to: out = 'TIMEOUT'
        elif 'InWorld' not in t: out = 'HANG' if 'Stomp' in t else 'EARLYFAIL'
        elif last < 75: out = 'short'
        else: out = 'ok'
        tot[out] = tot.get(out, 0) + 1
        arm = L.split('_', 1)[1] if '_' in L else '-'
        by_arm.setdefault(arm, {}); by_arm[arm][out] = by_arm[arm].get(out, 0) + 1
        g = lambda k: ('%.0fs' % t[k]) if k in t else '-'
        print('%-6s %-9s %-6s %-6s %-7s %-5s %-7s %-7d log spans %.0fs' % (L, out, g('Stomp'), g('Region'), g('DestSet'), g('EAC'), g('InWorld'), n_to, last))
print('summary:', tot)
if len(by_arm) > 1:
    for a, d in sorted(by_arm.items()): print('  arm %-6s %s' % (a, d))
    from math import comb
    def fisher(a, b, c, d):          # two-sided exact test on [[a,b],[c,d]]
        n = a + b + c + d; r1 = a + b; c1 = a + c
        p0 = comb(r1, a) * comb(n - r1, c) / comb(n, c1); tot = 0
        for x in range(max(0, c1 - (n - r1)), min(r1, c1) + 1):
            px = comb(r1, x) * comb(n - r1, c1 - x) / comb(n, c1)
            if px <= p0 + 1e-12: tot += px
        return min(1.0, tot)
    def bad(d): return d.get('TIMEOUT', 0) + d.get('HANG', 0) + d.get('EARLYFAIL', 0)
    def good(d): return d.get('ok', 0)
    if 'ctl' in by_arm:
        for a, d in sorted(by_arm.items()):
            if a == 'ctl': continue
            c = by_arm['ctl']
            if bad(c) + good(c) and bad(d) + good(d):
                print('  %s vs ctl: failures %d/%d vs %d/%d, Fisher p=%.3f (sessions marked short are ignored)' % (a, bad(d), bad(d) + good(d), bad(c), bad(c) + good(c), fisher(bad(d), good(d), bad(c), good(c))))
