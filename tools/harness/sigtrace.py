#!/usr/bin/env python3
"""Decode FEX_SIGNALTRACE rings (/dev/shm/fex-<pid>-sigtrace) and follow one signal through them.

usage: sigtrace.py GAME_RING [SERVER_RING] [--sig 10] [--tid TID] [--last SECONDS] [--events]
  GAME_RING    ring of the process whose threads receive the signal (the Wine process)
  SERVER_RING  optional ring of the sender (wineserver): its kill/tgkill calls are matched with the arrivals in GAME_RING
  --sig        signal to follow (default 10 = SIGUSR1, Wine's suspend/APC signal)
  --tid        print every event of that thread (default: threads with anomalies)
  --last       only look at the last SECONDS of the ring (default 60)
Files may be gzip-compressed. The summary lists, per thread, how every arrival of the signal ended
(delivered to the guest / recorded as pending because FEX considered it masked / deferred) and flags
arrivals that were consumed without a guest handler run, kills that never arrived, and handler runs that never returned.
"""
import collections, gzip, struct, sys

NAMES = {1: 'HostArrive', 2: 'Masked', 3: 'Deferred', 4: 'Delivered', 5: 'SigReturn', 6: 'ProcMask', 7: 'Reraise', 8: 'Kill',
         9: 'Read16', 10: 'Write16', 11: 'EpollCtl'}
REC = struct.Struct('<QIHHQQ')
HZ = 24_000_000.0

def load(path):
    d = (gzip.open(path) if path.endswith('.gz') else open(path, 'rb')).read()
    out = []
    for i in range(0, len(d) - REC.size + 1, REC.size):
        tsc, tid, ev, sig, a, b = REC.unpack_from(d, i)
        if tsc: out.append((tsc, tid, ev, sig, a, b))
    out.sort()
    return out

args = [a for a in sys.argv[1:] if not a.startswith('--')]
opt = {}
it = iter(sys.argv[1:])
for a in it:
    if a.startswith('--'):
        opt[a] = next(it, '') if a in ('--sig', '--tid', '--last') else True
        if a in ('--sig', '--tid', '--last'):
            args = [x for x in args if x != opt[a]]
SIG = int(opt.get('--sig', 10)); LAST = float(opt.get('--last', 60)); TID = int(opt['--tid']) if '--tid' in opt else None
game = load(args[0]); server = load(args[1]) if len(args) > 1 else []
if not game: sys.exit('empty game ring')
t_end = game[-1][0]
game = [r for r in game if (t_end - r[0]) / HZ <= LAST]
t0 = game[0][0]
rel = lambda t: (t - t_end) / HZ

per = collections.defaultdict(list)
for r in game: per[r[1]].append(r)

def fmt(r):
    tsc, tid, ev, sig, a, b = r
    nm = NAMES.get(ev, str(ev))
    extra = {'HostArrive': 'mask=%016x defer=%d si_code=%d' % (a, b >> 32, b & 0xffffffff), 'Masked': 'mask=%016x pending=%016x' % (a, b),
             'Delivered': 'handler=0x%x hostmask=%016x' % (a, b), 'SigReturn': 'rsp=0x%x savedmask=%016x' % (a, b),
             'ProcMask': 'how=%d set=%016x mask-after=%016x' % (sig, a, b), 'Reraise': 'mask=%016x pending=%016x' % (a, b),
             'Kill': 'target=%d tgid=%d' % (a, b)}.get(nm, '')
    return '%+9.4fs tid %-6d %-10s sig=%-2d %s' % (rel(tsc), tid, nm, sig, extra)

summary = []
for tid, recs in per.items():
    arrivals = [r for r in recs if r[2] == 1 and r[3] == SIG]
    if not arrivals: continue
    seq = [(r[2], r[3]) for r in recs]
    delivered = sum(1 for r in recs if r[2] == 4 and r[3] == SIG)
    masked = sum(1 for r in recs if r[2] == 2 and r[3] == SIG)
    deferred = sum(1 for r in recs if r[2] == 3 and r[3] == SIG)
    rets = sum(1 for r in recs if r[2] == 5)
    reraise = sum(1 for r in recs if r[2] == 7 and r[3] == SIG)
    # a Deferred frame is delivered later through the fault-page path and shows up as another HostArrive + Delivered
    summary.append((tid, len(arrivals), delivered, masked, deferred, reraise, rets))

print('signal %d in the last %.0fs of the ring (%d events, %d threads saw it)' % (SIG, LAST, len(game), len(summary)))
tot = [sum(x[i] for x in summary) for i in range(1, 7)]
print('totals: arrivals %d  delivered %d  masked %d  deferred %d  re-raised %d  rt_sigreturn %d' % tuple(tot))
bad = [x for x in summary if x[1] > x[2] + x[3] + x[4] or x[2] > x[6]]
print('threads where an arrival did not end in a guest handler (arrivals > delivered+masked+deferred) or a handler has not returned (delivered > sigreturns):')
for tid, ar, de, ma, df, rr, rt in sorted(bad, key=lambda x: -x[1])[:25]:
    print('  tid %-6d arrivals %-4d delivered %-4d masked %-4d deferred %-4d reraised %-4d sigreturns %-4d' % (tid, ar, de, ma, df, rr, rt))
if server:
    kills = [r for r in server if r[2] == 8 and r[3] == SIG]
    print('\nsender side: %d kill/tgkill(sig %d) calls' % (len(kills), SIG))
    # match each kill with the next arrival on the target tid in the game ring
    arr = collections.defaultdict(list)
    for r in game:
        if r[2] == 1 and r[3] == SIG: arr[r[1]].append(r[0])
    unmatched = []
    for k in kills:
        tgt = k[4]; later = [t for t in arr.get(tgt, []) if t >= k[0] - 240000]
        if later: arr[tgt].remove(later[0])
        else: unmatched.append(k)
    print('kills with no arrival at the target afterwards (lost or coalesced by the kernel): %d' % len(unmatched))
    for k in unmatched[-20:]:
        print('  at %+9.4fs sender tid %d -> target tid %d' % ((k[0] - t_end) / HZ, k[1], k[4]))
focus = [TID] if TID else [x[0] for x in sorted(bad, key=lambda x: -x[1])[:3]]
if '--events' in opt or TID:
    for tid in focus:
        print('\n=== events of tid %d (last 60 of %d)' % (tid, len(per[tid])))
        for r in per[tid][-60:]: print('  ' + fmt(r))
