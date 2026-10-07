#!/usr/bin/env python3
"""Decode a FEX_SIGNALTRACE ring and follow Wine's thread wake-up traffic (the 16-byte reply read/write on a pipe).

usage: wakeflow.py RING [--tid TID] [--fd FD] [--last SECONDS] [--events]

FEX_SIGNALTRACE=1 (patch 0007) makes every FEX process write a ring at /dev/shm/fex-<pid>-sigtrace with the signal
events and, since the same patch, the 16-byte reads/writes (Read16/Write16, the first 8 bytes are Wine's wait cookie)
and epoll_ctl (EpollCtl). `wedgewatch.py` copies a running game's ring (and wineserver's, if it has one) to
sigtrace_<label>_<n>_game.gz / _ws.gz at a wedge. This tool reads one ring and prints

  * a per-thread, per-event count, and
  * every Read16/Write16 with its fd, result and cookie, plus every EpollCtl (fd, events), so a reply that the server
    wrote but the waiter never read (a lost wake-up) shows up as a Write16 with no matching Read16 of the same cookie.

A ring is a lock-free array of fixed 32-byte records: uint64 Tsc (CNTVCT_EL0, 24 MHz on Apple silicon), uint32 Tid,
uint16 Ev, uint16 Sig, uint64 A, uint64 B. The tool auto-detects the record size, since a truncated/odd file must not
be read misaligned.
"""
import collections, gzip, struct, sys

HZ = 24_000_000.0
NAMES = {1: 'HostArrive', 2: 'Masked', 3: 'Deferred', 4: 'Delivered', 5: 'SigReturn', 6: 'ProcMask', 7: 'Reraise', 8: 'Kill',
         9: 'Read16', 10: 'Write16', 11: 'EpollCtl'}
REC = struct.Struct('<QIHHQQ')


def load(path):
    d = (gzip.open(path) if path.endswith('.gz') else open(path, 'rb')).read()
    n = len(d) // REC.size
    if n == 0:
        return []
    out = []
    for i in range(n):
        tsc, tid, ev, sig, a, b = REC.unpack_from(d, i * REC.size)
        if tsc and ev in NAMES:  # a zero tsc is an unwritten slot; an unknown event means a misread
            out.append((tsc, tid, ev, sig, a, b))
    out.sort()
    return out


def cookie(b):
    return 'ERR(%d)' % (b & ~(1 << 63)) if b & (1 << 63) else '0x%016x' % b


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    opt = {}
    for i, a in enumerate(argv):
        if a in ('--tid', '--fd', '--last'):
            opt[a] = argv[i + 1] if i + 1 < len(argv) else ''
    path = args[0]
    tid = int(opt['--tid']) if '--tid' in opt else None
    fd = int(opt['--fd']) if '--fd' in opt else None
    last = float(opt.get('--last', 120))
    all_events = '--events' in argv

    ev = load(path)
    if not ev:
        sys.exit('empty or unreadable ring: %s' % path)
    t_end = ev[-1][0]
    ev = [r for r in ev if (t_end - r[0]) / HZ <= last]
    t0 = ev[0][0]
    rel = lambda t: (t - t0) / HZ

    print('%s: %d events, %d threads, window %.0fs' % (path, len(ev), len(set(r[1] for r in ev)), last))

    counts = collections.Counter((r[1], NAMES[r[2]]) for r in ev)
    print('\nper (tid, event):')
    for (t, en), c in sorted(counts.items(), key=lambda kv: -kv[1])[:25]:
        print('  tid %-7d %-10s %d' % (t, en, c))

    sel = [r for r in ev if (tid is None or r[1] == tid) and
           (fd is None or r[2] not in (9, 10, 11) or r[3] == fd)]
    show = [r for r in sel if r[2] in (9, 10, 11)] if not all_events else sel
    print('\nread/write/epoll traffic%s:' % ('' if tid is None else ' for tid %d' % tid))
    for tsc, t, e, sig, a, b in show:
        if e == 11:
            print('  %+9.4fs tid %-7d EpollCtl fd=%-5d op=%d events=0x%x result=%d' % (rel(tsc), t, a, sig, b & 0xffffffff, b >> 32))
        else:
            print('  %+9.4fs tid %-7d %-8s fd=%-5d result=%d cookie=%s' % (rel(tsc), t, NAMES[e], sig, a, cookie(b)))

    # Lost-wake check across the whole ring: a 16-byte write whose cookie is never read back.
    writes = [r for r in ev if r[2] == 10 and r[4] == 16]
    reads = collections.Counter(r[5] for r in ev if r[2] == 9 and r[4] == 16)
    unread = [r for r in writes if not reads.get(r[5])]
    print('\n16-byte writes whose cookie was never read back on the same ring: %d of %d' % (len(unread), len(writes)))
    for tsc, t, e, sig, a, b in unread[-15:]:
        print('  %+9.4fs tid %-7d fd=%-5d cookie=%s (server wrote a reply the waiter did not consume)' % (rel(tsc), t, sig, cookie(b)))


if __name__ == '__main__':
    main(sys.argv[1:])
