#!/usr/bin/env python3
"""Read wineserver's own per-socket state from its memory, passively (no ptrace attach, no signal; wineserver is a separate
process from the game, so nothing in the game can observe this). Runs in the VM as root.

For every socket wineserver has registered in its epoll set it prints the AFD event-select state that decides whether the
application gets woken: mask, pending_events, reported_events, the event object (and whether it is signalled), the window
for WSAAsyncSelect mode, and whether any receive async or poll request is queued — joined with the kernel receive queue.

Layouts are those of ValveSoftware/wine experimental-wine-11.0-20261001 (server/object.h, fd.c, sock.c, event.c) on LP64.
Proton's struct object header is 72 bytes (upstream: 56; found empirically: fd->user at +120, fd->unix_fd at +196), so
every field after the header sits 16 bytes later than in upstream sources. Every row is self-checked (sock->obj.ops == &sock_ops, sock->fd == poll_users[index], fd->unix_fd == the epoll fd), so a
layout mismatch prints BAD instead of wrong numbers.

  wsmem.py [OUTFILE]          (default: stdout)   symbols are read from the wineserver binary with nm
"""
import os, re, struct, subprocess, sys

def sh(c): return subprocess.run(c, shell=True, capture_output=True, text=True).stdout
ws = int(sh('pgrep -x wineserver').split()[0])
maps = open('/proc/%d/maps' % ws).read().splitlines()
binpath = base = None
for l in maps:
    p = l.split()
    if len(p) >= 6 and ' '.join(p[5:]).endswith('/wineserver') and int(p[2], 16) == 0:
        binpath = ' '.join(p[5:]); base = int(p[0].split('-')[0], 16); break
assert base, 'wineserver mapping not found'
sym = {}; rsym = []
for l in sh('nm "%s"' % binpath).splitlines():
    p = l.split()
    if len(p) == 3: sym[p[2]] = int(p[0], 16) + base; rsym.append((int(p[0], 16) + base, p[2]))
rsym.sort()
def symname(a):
    import bisect
    i = bisect.bisect_right([x for x, _ in rsym], a) - 1
    return rsym[i][1] if i >= 0 and a - rsym[i][0] < 0x400 else hex(a)
mem = open('/proc/%d/mem' % ws, 'rb', 0)
def rd(a, n):
    mem.seek(a); return mem.read(n)
def u64(a): return struct.unpack('<Q', rd(a, 8))[0]
def u32(a): return struct.unpack('<I', rd(a, 4))[0]
def lst_empty(a): return u64(a) == a

# epoll registrations: tfd -> (events, data = poll index)
reg = {}
for f in os.listdir('/proc/%d/fd' % ws):
    try:
        if os.readlink('/proc/%d/fd/%s' % (ws, f)) != 'anon_inode:[eventpoll]': continue
    except OSError: continue
    for m in re.finditer(r'tfd:\s+(\d+)\s+events:\s+([0-9a-f]+)\s+data:\s+([0-9a-f]+)', open('/proc/%d/fdinfo/%s' % (ws, f)).read()):
        reg[int(m[1])] = (int(m[2], 16), int(m[3], 16))
# socket inode -> (recvq, local, peer) from ss, and wineserver fd -> inode
ssq = {}
for l in sh('ss -tuanie').splitlines():
    p = l.split(); mi = re.search(r'ino:(\d+)', l)
    if len(p) >= 6 and p[0] in ('tcp', 'udp') and mi: ssq[mi[1]] = (int(p[2]), p[4], p[5], p[1])
fdino = {}
for f in os.listdir('/proc/%d/fd' % ws):
    try: l = os.readlink('/proc/%d/fd/%s' % (ws, f))
    except OSError: continue
    if l.startswith('socket:['): fdino[int(f)] = l[8:-1]

poll_users = u64(sym['poll_users']); SOCK_OPS = sym['sock_ops']
OBJ = 72                                   # sizeof(struct object) in this Proton build
out = open(sys.argv[1], 'w') if len(sys.argv) > 1 else sys.stdout
STATES = {0: 'UNCONN', 1: 'LISTEN', 2: 'CONNECTING', 3: 'CONNECTED', 4: 'CONNLESS'}
print('wineserver pid %d base %#x  sockets in epoll: %d' % (ws, base, sum(1 for t in reg if t in fdino)), file=out)
print('%-5s %-6s %-5s %-24s %-10s %-6s %-5s %-5s %-5s %-14s %-12s %-6s %-5s %-5s %s' % (
    'wsfd', 'recvq', 'epoll', 'peer', 'state', 'mask', 'pend', 'rept', 'nblk?', 'event', 'evsync', 'win', 'rdq', 'pollq', 'check'), file=out)
for tfd in sorted(reg):
    if tfd not in fdino: continue
    events, idx = reg[tfd]
    q = ssq.get(fdino[tfd], (-1, '?', '?', '?'))
    try:
        fdp = u64(poll_users + 8 * idx); sk = u64(fdp + OBJ + 48)
        ok = (u64(sk + 8) == SOCK_OPS and u64(sk + OBJ) == fdp and struct.unpack('<i', rd(fdp + OBJ + 124, 4))[0] == tfd)
        state, mask, pend, rept = struct.unpack('<IIII', rd(sk + OBJ + 8, 16))
        ev = u64(sk + OBJ + 32); win, msg = u32(sk + OBJ + 40), u32(sk + OBJ + 44)
        evs = '-'
        if ev:
            sync = u64(ev + OBJ); so = symname(u64(sync + 8))
            evs = '%s:%#x' % (so.replace('_sync_ops', ''), u32(sync + OBJ)) if so == 'event_sync_ops' else so.replace('_sync_ops', '')
        rdq = 'empty' if lst_empty(sk + OBJ + 120) else 'QUEUED'; pq = 'empty' if lst_empty(sk + OBJ + 200) else 'QUEUED'
        print('%-5d %-6d %-5x %-24s %-10s %-6x %-5x %-5x %-5s %-14x %-12s %-6x %-5s %-5s %s' % (
            tfd, q[0], events, q[2][-24:], STATES.get(state, state), mask, pend, rept, '', ev, evs, win, rdq, pq,
            'ok' if ok else 'BAD'), file=out)
    except Exception as e:
        print('%-5d error %s' % (tfd, e), file=out)
print('legend: AFD bits READ=1 OOB=2 WRITE=4 HUP=8 RESET=10 CLOSE=20 CONNECT=40 ACCEPT=80 CONNECT_ERR=100; '
      'evsync event:<bits> bit0=manual bit1=signaled; win!=0 => WSAAsyncSelect (window-message) mode', file=out)
