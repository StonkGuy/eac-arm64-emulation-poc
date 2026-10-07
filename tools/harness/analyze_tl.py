#!/usr/bin/env python3
"""Correlate HUD FPS with guest per-thread CPU, guest/host pressure counters and UDP backlog. usage: analyze_tl.py LABEL"""
import sys, collections, statistics, bisect
L = sys.argv[1]
def num(x):
    try: return float(x)
    except: return None
# ---- FPS
fps = []
for l in open('fps_tl_%s.txt' % L):
    p = l.split()
    if len(p) == 2 and num(p[0]) and num(p[1]) is not None and 0 < float(p[1]) < 100: fps.append((float(p[0]), float(p[1])))
fts = [t for t, _ in fps]
# ---- guest timeline
G_T = []; G_R = []; G_U = []; G_D = []
for l in open('timeline_%s.txt' % L):
    k = l[0]; p = l.rstrip('\n').split(' ')
    if k == 'T':
        d = {'t': float(p[1])}
        for kv in p[2:]:
            if '=' in kv: a, b = kv.split('=', 1); d[a] = num(b) if num(b) is not None else b
        G_T.append(d)
    elif k == 'R':
        thr = {}
        for item in p[2:]:
            tid, _, rest = item.partition(':')
            f = rest.rsplit(':', 2)
            if len(f) == 3 and f[1].isdigit(): thr[tid] = (f[0], int(f[1]), f[2])
        G_R.append((float(p[1]), thr))
    elif k == 'U': G_U.append((float(p[1]), p[2] if len(p) > 2 else '-'))
    elif k == 'D': G_D.append((float(p[1]), p[2:]))
H_T = []; H_V = []
try:
    for l in open('hosttl_%s.txt' % L):
        k = l[0]; p = l.rstrip('\n').split(' ')
        if k == 'H':
            d = {'t': float(p[1])}
            for kv in p[2:]:
                if '=' in kv: a, b = kv.split('=', 1); d[a] = b
            H_T.append(d)
        elif k == 'V':
            thr = {}
            for item in p[2:]:
                f = item.split(':')
                if len(f) == 2: thr[f[0]] = int(f[1])
            H_V.append((float(p[1]), thr))
except FileNotFoundError: pass
print('fps pts %d  guest pts %d  host pts %d  udp-dumps %d' % (len(fps), len(G_R), len(H_T), len(G_D)))
if not fps or len(G_R) < 4: sys.exit('not enough data')
def fps_at(t):
    i = bisect.bisect_left(fts, t)
    cands = [j for j in (i - 1, i) if 0 <= j < len(fps) and abs(fps[j][0] - t) < 0.6]
    return fps[min(cands, key=lambda j: abs(fps[j][0] - t))][1] if cands else None
def cls(f): return None if f is None else ('LOW' if f < 30 else ('HIGH' if f >= 40 else 'MID'))
# intervals between consecutive guest samples
grp = collections.defaultdict(lambda: collections.defaultdict(list))   # class -> group -> [cpu%]
tot_by = collections.defaultdict(list)
for (t0, a), (t1, b) in zip(G_R, G_R[1:]):
    dt = t1 - t0
    if dt <= 0: continue
    c = cls(fps_at((t0 + t1) / 2))
    if not c: continue
    per = collections.defaultdict(float)
    for tid, (comm, ticks, st) in b.items():
        if tid in a: per[comm] += (ticks - a[tid][1]) / 100.0 / dt * 100.0
    for k, v in per.items(): grp[c][k].append(v)
    tot_by[c].append(sum(per.values()))
cnt = {c: len(tot_by[c]) for c in ('LOW', 'MID', 'HIGH')}
print('intervals LOW/MID/HIGH:', cnt['LOW'], cnt['MID'], cnt['HIGH'], ' fps mean %.1f p10 %.1f' % (statistics.mean(f for _, f in fps), sorted(f for _, f in fps)[len(fps) // 10]))
names = set(grp['LOW']) | set(grp['HIGH'])
rows = []
for n in names:
    lo = grp['LOW'].get(n, []); hi = grp['HIGH'].get(n, [])
    if not lo or not hi: continue
    ml, mh = statistics.mean(lo), statistics.mean(hi)
    rows.append((ml - mh, n, ml, mh))
print('\nthread-group CPU%  (sum over threads with that name; 100 = one core)    LOW    HIGH   LOW-HIGH')
for d, n, ml, mh in sorted(rows, key=lambda r: -abs(r[0]))[:12]: print('  %-18s %7.1f %7.1f %+7.1f' % (n[:18], ml, mh, d))
print('  %-18s %7.1f %7.1f' % ('(all game threads)', statistics.mean(tot_by['LOW']) if tot_by['LOW'] else 0, statistics.mean(tot_by['HIGH']) if tot_by['HIGH'] else 0))
# wineserver + guest counters
def delta_series(seq, key):
    out = []
    for a, b in zip(seq, seq[1:]):
        if key in a and key in b and isinstance(a[key], float) and isinstance(b[key], float):
            dt = b['t'] - a['t']
            if dt > 0: out.append(((a['t'] + b['t']) / 2, (b[key] - a[key]) / dt))
    return out
def by_class(series, scale=1.0):
    r = collections.defaultdict(list)
    for t, v in series:
        c = cls(fps_at(t))
        if c: r[c].append(v * scale)
    return r
print('\nguest counters per second            LOW        HIGH')
for key, scale, label in (('ws', 1.0, 'wineserver CPU% (ticks/s)'), ('cpu_psi', 1e-6 * 100, 'PSI cpu some %'), ('mem_psi', 1e-6 * 100, 'PSI mem some %'), ('io_psi', 1e-6 * 100, 'PSI io some %'),
                          ('pgfault', 1.0, 'page faults/s'), ('pgmajfault', 1.0, 'MAJOR faults/s'), ('pswpin', 1.0, 'swap-in pages/s'), ('pswpout', 1.0, 'swap-out pages/s'), ('allocstall_normal', 1.0, 'direct-reclaim stalls/s'), ('pgscan_direct', 1.0, 'direct scan pages/s'), ('pgscan_kswapd', 1.0, 'kswapd scan pages/s'), ('compact_stall', 1.0, 'compaction stalls/s')):
    r = by_class(delta_series(G_T, key), scale)
    if r['LOW'] and r['HIGH']: print('  %-26s %9.1f %9.1f' % (label, statistics.mean(r['LOW']), statistics.mean(r['HIGH'])))
if H_T:
    print('\nhost counters per second             LOW        HIGH')
    for h in H_T:
        for k in list(h):
            if k not in ('t', 'avail', 'swapfree') and not isinstance(h[k], float):
                try: h[k] = float(h[k].split()[0]) if k.endswith('psi') else float(h[k])
                except: pass
    for h in H_T:
        for k in ('avail', 'swapfree'): h[k] = num(h.get(k)) if num(h.get(k)) is not None else h.get(k)
    for key, scale, label in (('cpu_psi', 1e-6 * 100, 'host PSI cpu some %'), ('mem_psi', 1e-6 * 100, 'host PSI mem some %'), ('io_psi', 1e-6 * 100, 'host PSI io some %'), ('pswpin', 1.0, 'host swap-in pages/s'), ('pswpout', 1.0, 'host swap-out pages/s'), ('zswpin', 1.0, 'host zswap-in/s'), ('zswpout', 1.0, 'host zswap-out/s'), ('pgmajfault', 1.0, 'host major faults/s'), ('allocstall_normal', 1.0, 'host direct reclaim/s'), ('pgscan_direct', 1.0, 'host direct scan/s'), ('workingset_refault_anon', 1.0, 'host anon refaults/s'), ('workingset_refault_file', 1.0, 'host file refaults/s')):
        r = by_class(delta_series(H_T, key), scale)
        if r['LOW'] and r['HIGH']: print('  %-26s %9.1f %9.1f' % (label, statistics.mean(r['LOW']), statistics.mean(r['HIGH'])))
    a = [h['avail'] for h in H_T if isinstance(h.get('avail'), float)]
    if a: print('  host MemAvailable MB: min %.0f  mean %.0f  max %.0f' % (min(a) / 1024, statistics.mean(a) / 1024, max(a) / 1024))
    # VM process threads: which host threads (vCPUs) are busy
    dg = collections.defaultdict(lambda: collections.defaultdict(list))
    for (t0, a), (t1, b) in zip(H_V, H_V[1:]):
        dt = t1 - t0; c = cls(fps_at((t0 + t1) / 2))
        if dt <= 0 or not c: continue
        tot = sum(max(0, b.get(k, 0) - a.get(k, 0)) for k in b) / 100.0 / dt * 100.0
        dg[c]['VM process total CPU%'].append(tot)
    for c in ('LOW', 'HIGH'):
        if dg[c]['VM process total CPU%']: print('  VM process CPU%% (%s): %.0f' % (c, statistics.mean(dg[c]['VM process total CPU%'])))
# UDP backlog
print('\nUDP sockets with unread data (guest), non-empty samples:')
ev = [(t, q) for t, q in G_U if any(int(x.split('>')[1].split('/')[0]) > 0 for x in q.split(',') if '>' in x)]
print('  %d of %d samples; max backlog %s' % (len(ev), len(G_U), max((max(int(x.split('>')[1].split('/')[0]) for x in q.split(',') if '>' in x) for _, q in ev), default=0)))
for t, q in ev[:6]: print('   %.1f %s fps=%s' % (t, q, fps_at(t)))
for t, d in G_D[:2]:
    print('  stall dump at %.1f (fps %s): %d threads blocked' % (t, fps_at(t), len(d)))
    for x in d[:60]:
        f = x.split(':')
        if len(f) >= 4 and f[2] not in ('202', '449', '230', '35', '7', '232', '281'): print('     ', x)
# worst FPS windows
low_runs = []; cur = None
for t, f in fps:
    if f < 30:
        if cur is None: cur = [t, t]
        else: cur[1] = t
    elif cur is not None: low_runs.append(cur); cur = None
print('\nlow-FPS (<30) stretches: %s' % ', '.join('%.0fs@+%.0f' % (b - a + 0.5, a - fps[0][0]) for a, b in low_runs[:30]))
