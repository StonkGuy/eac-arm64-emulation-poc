#!/usr/bin/env python3
"""Resolve a FEX thread snapshot (`touch /dev/shm/fex-<pid>-snapshot` in the VM -> /dev/shm/fex-<pid>-snap).

usage: resolve_snap.py SNAP MAPS [NAMES]
  SNAP   copy of /dev/shm/fex-<pid>-snap
  MAPS   copy of /proc/<pid>/maps taken at the same time
  NAMES  optional text file with lines "<tid> <thread name>"
For every thread: the x86 syscall it is blocked in (number from rax, arguments), the return addresses found on its stack
(any stack word that points into an executable file mapping), shown as module+offset or, for ELF libraries and Wine PE
DLLs whose files are readable here, as the nearest exported symbol.
"""
import bisect, os, re, struct, subprocess, sys

SYS = {0: 'read', 1: 'write', 7: 'poll', 17: 'pread64', 23: 'select', 35: 'nanosleep', 44: 'sendto', 45: 'recvfrom', 46: 'sendmsg', 47: 'recvmsg', 202: 'futex',
       230: 'clock_nanosleep', 232: 'epoll_wait', 233: 'epoll_ctl', 270: 'pselect6', 271: 'ppoll', 281: 'epoll_pwait', 449: 'futex_waitv', 61: 'wait4', 131: 'sigaltstack',
       14: 'rt_sigprocmask', 13: 'rt_sigaction', 34: 'pause', 130: 'rt_sigsuspend', 128: 'rt_sigtimedwait', 247: 'waitid', 307: 'sendmmsg', 299: 'recvmmsg', 16: 'ioctl'}
FUTEX_OPS = {0: 'WAIT', 1: 'WAKE', 3: 'REQUEUE', 4: 'CMP_REQUEUE', 5: 'WAKE_OP', 9: 'WAIT_BITSET', 10: 'WAKE_BITSET', 128: 'WAIT|PRIV', 129: 'WAKE|PRIV', 137: 'WAIT_BITSET|PRIV'}

snap = open(sys.argv[1], 'rb').read(); REC = 80 * 8
names = {}
if len(sys.argv) > 3:
    for l in open(sys.argv[3]):
        p = l.split(None, 1)
        if len(p) == 2 and p[0].isdigit(): names[int(p[0])] = p[1].strip()

maps = []   # (start, end, perms, offset, path)
for l in open(sys.argv[2]):
    p = l.split(None, 5)
    if len(p) < 5: continue
    a, b = [int(x, 16) for x in p[0].split('-')]
    maps.append((a, b, p[1], int(p[2], 16), p[5].strip() if len(p) > 5 else ''))
maps.sort(); starts = [m[0] for m in maps]
base = {}   # path -> image base (lowest start - offset of any mapping, 0-offset mapping preferred)
for a, b, perm, off, path in maps:
    if path and not path.startswith('['):
        base[path] = min(base.get(path, 1 << 64), a - off)

def region(addr):
    i = bisect.bisect_right(starts, addr) - 1
    return maps[i] if i >= 0 and maps[i][0] <= addr < maps[i][1] else None

_sym = {}
def pe_exports(path):
    try: d = open(path, 'rb').read()
    except OSError: return None
    if d[:2] != b'MZ': return None
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    if d[pe:pe + 4] != b'PE\0\0': return None
    nsec = struct.unpack_from('<H', d, pe + 6)[0]; osz = struct.unpack_from('<H', d, pe + 20)[0]; opt = pe + 24
    magic = struct.unpack_from('<H', d, opt)[0]; dd = opt + (112 if magic == 0x20b else 96)
    exp_rva, exp_sz = struct.unpack_from('<II', d, dd)
    secs = []
    so = opt + osz
    for i in range(nsec):
        vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, so + 40 * i + 8)
        secs.append((va, max(vsz, rsz), rptr))
    def off(rva):
        for va, sz, rp in secs:
            if va <= rva < va + sz: return rva - va + rp
    if not exp_rva: return []
    e = off(exp_rva)
    if e is None: return []
    nfunc, nnames, afunc, anames, aords = struct.unpack_from('<IIIII', d, e + 20)
    fo, no, oo = off(afunc), off(anames), off(aords)
    out = []
    for i in range(nnames):
        nm = struct.unpack_from('<I', d, no + 4 * i)[0]; o = struct.unpack_from('<H', d, oo + 2 * i)[0]
        rva = struct.unpack_from('<I', d, fo + 4 * o)[0]
        s = off(nm)
        out.append((rva, d[s:d.index(b'\0', s)].decode('latin1')))
    return sorted(out)

def elf_syms(path):
    for tool in ('llvm-nm', 'nm'):
        try:
            r = subprocess.run([tool, '-D', '--defined-only', path], capture_output=True, text=True, timeout=20)
            if r.returncode == 0:
                out = []
                for l in r.stdout.splitlines():
                    p = l.split()
                    if len(p) == 3 and p[1] in 'TtWwiI':
                        out.append((int(p[0], 16), p[2]))
                return sorted(out)
        except (OSError, subprocess.TimeoutExpired): pass
    return None

def symbolize(addr):
    r = region(addr)
    if not r: return None
    path = r[4]
    if not path or path.startswith('['): return '%s+0x%x' % (path or 'anon', addr - r[0])
    rel = addr - base[path]
    short = os.path.basename(path.replace(' (deleted)', ''))
    if path not in _sym:
        real = path.replace(' (deleted)', '')
        _sym[path] = (pe_exports(real) if real.lower().endswith(('.dll', '.exe', '.drv', '.sys')) else elf_syms(real)) if os.path.exists(real) else None
    tab = _sym[path]
    if tab:
        i = bisect.bisect_right([t[0] for t in tab], rel) - 1
        if i >= 0 and rel - tab[i][0] < 0x4000: return '%s!%s+0x%x' % (short, tab[i][1], rel - tab[i][0])
    return '%s+0x%x' % (short, rel)

# The record grew over the series: patches 0005/0006 write 80 words (640 B); later builds that add the futex-word
# diagnostics write 160 words (1280 B). The file only holds the array (no header), so a wrong choice silently misaligns
# and invents threads. Detect it: a real record's first word is a small Linux tid; the misaligned decoys start mid-record
# and their "tid" is a random register value, usually far above pid_max. Pick the size whose nonzero tids are plausible.
def _plausible(rec):
    if len(snap) % rec: return (-1.0, 0)
    tot = plaus = 0
    for i in range(len(snap) // rec):
        t = struct.unpack_from('<Q', snap, i * rec)[0]
        if t:
            tot += 1
            if t < (1 << 24): plaus += 1
    return (plaus / tot if tot else -1.0, tot)
f1280, f640 = _plausible(1280), _plausible(640)
if f1280[0] < 0 and f640[0] < 0:
    sys.exit('snapshot size %d is not a whole number of 640- or 1280-byte records' % len(snap))
# The profiler's MaxSnaps is 1024, so a full snapshot is exactly 1024 records: file size / 1024 is the record size.
# Prefer that (plausibility alone cannot separate the two when the misaligned stride also looks plausible).
if len(snap) % 1280 == 0 and len(snap) // 1280 == 1024:
    REC = 1280
elif len(snap) % 640 == 0 and len(snap) // 640 == 1024:
    REC = 640
else:
    REC = 1280 if (f1280[0] >= f640[0]) else 640
WORDS = REC // 8
n = len(snap) // REC
print('%d thread records (record %d B)' % (sum(1 for i in range(n) if struct.unpack_from('<Q', snap, i * REC)[0]), REC))
# Layouts (from patches 0006 and 0009):
#   640 B  (=80 words):  Tid HostPC Rip Spare Gregs[16] Stack[60]
#   1280 B (=160 words): Tid HostPC Rip FutexWord FutexExpected FutexAddr Gregs[16] Stack[138]
# Gregs order: rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15
if REC == 1280:
    GOFF, SOFF, FW = 6, 22, (3, 4)
else:
    GOFF, SOFF, FW = 4, 20, None
for i in range(n):
    f = struct.unpack_from('<%dQ' % WORDS, snap, i * REC)
    tid = f[0]
    if not tid: continue
    rip, g, stack = f[2], f[GOFF:GOFF + 16], f[SOFF:SOFF + (WORDS - SOFF)]
    rax = g[0]; args = (g[7], g[6], g[2], g[10], g[8], g[9])
    print('\ntid %d  %s' % (tid, names.get(tid, '')))
    nm = SYS.get(rax)
    if nm:
        extra = ''
        if nm == 'futex': extra = ' op=%s' % FUTEX_OPS.get(args[1] & 0xff | (args[1] & 0x80), args[1])
        if FW is not None:
            fw = f[FW[0]]
            if fw >> 63:
                w, e = fw & 0xffffffff, f[FW[1]] & 0xffffffff
                extra += ' word=%d expected=%d %s' % (w, e, 'EQUAL (nobody woke it)' if w == e else 'DIFF (lost wake-up?)')
        print('  syscall %s(%s)%s' % (nm, ', '.join('0x%x' % a for a in args[:4]), extra))
    else:
        print('  rax=0x%x (not a syscall: thread was running or in the middle of a block)' % rax)
    print('  rip %s   rsp 0x%x' % (symbolize(rip) or hex(rip), g[4]))
    seen = 0
    for w in stack:
        r = region(w)
        if r and 'x' in r[2] and r[4] and not r[4].startswith('['):
            print('    stack> %s' % symbolize(w)); seen += 1
            if seen >= 14: break
