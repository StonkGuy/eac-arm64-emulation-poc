#!/usr/bin/env python3
"""Mimic a stop-the-world garbage collector (Boehm GC on Windows: SuspendThread + GetThreadContext + ResumeThread on every thread)
over threads that are blocked in the kinds of waits a Unity game has, and check that nothing wedges.

  - one thread blocked in a *blocking UDP recvfrom* (the Photon receive thread), fed by a sender thread at --pps packets per second
  - --idle threads blocked in threading.Event.wait() (pool workers waiting for work; a Wine event wait)
  - --sleepers threads that sleep in short intervals (timed waits)
  - one 'collector' thread that every --period seconds stops the world exactly like a GC would

It reports the worst SuspendThread/GetThreadContext/ResumeThread latency, and flags (exit code 3) when
  * the collector cannot finish a world stop within --stuck seconds (it then also dumps all Python stacks), or
  * the receiver stops receiving although the sender keeps sending for more than --recv-stall seconds.

Run it under Wine/Proton with a Windows build of Python (embeddable zip):  python.exe wedge_repro.py --seconds 60
With --dry it only runs the threads/socket part (no Win32 calls), to test the script on Linux.
"""
import argparse, ctypes, faulthandler, os, socket, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument('--seconds', type=float, default=60)
ap.add_argument('--idle', type=int, default=60)
ap.add_argument('--sleepers', type=int, default=10)
ap.add_argument('--io', type=int, default=3, help='pipe readers and tcp readers (each)')
ap.add_argument('--period', type=float, default=0.25, help='seconds between world stops')
ap.add_argument('--pps', type=float, default=60)
ap.add_argument('--stuck', type=float, default=8)
ap.add_argument('--recv-stall', type=float, default=4)
ap.add_argument('--hold', type=float, default=20, help='seconds to keep threads alive after a wedge is detected')
ap.add_argument('--mode', choices=['gc', 'newrecv'], default='gc')
ap.add_argument('--iterations', type=int, default=300)
ap.add_argument('--spin', type=int, default=3, help='busy threads during newrecv (varies timing)')
ap.add_argument('--logfile', default='', help='also write all output (and faulthandler dumps) to this file; proton run does not return stdout')
ap.add_argument('--dry', action='store_true')
ap.add_argument('--port', type=int, default=0)
args = ap.parse_args()

LOGF = None
if args.logfile:
    LOGF = open(args.logfile, 'w', buffering=1)
    sys.stdout = sys.stderr = LOGF
faulthandler.enable(file=LOGF) if LOGF else faulthandler.enable()
IS_WIN = os.name == 'nt'
stop = threading.Event()
targets = {}             # name -> native thread id
tlock = threading.Lock()

def register(name):
    with tlock:
        targets[name] = threading.get_native_id()

# ---------------------------------------------------------------- workload
rx = {'n': 0, 'last': time.time()}
tx = {'n': 0}
rsock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rsock.bind(('127.0.0.1', args.port))
port = rsock.getsockname()[1]
rsock.settimeout(None)    # blocking, like Photon's receive loop

def receiver():
    register('receiver')
    while not stop.is_set():
        try:
            rsock.recvfrom(2048)
        except OSError:
            break
        rx['n'] += 1
        rx['last'] = time.time()

def sender():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    gap = 1.0 / args.pps
    nxt = time.time()
    while not stop.is_set():
        s.sendto(b'x' * 200, ('127.0.0.1', port))
        tx['n'] += 1
        nxt += gap
        d = nxt - time.time()
        if d > 0:
            time.sleep(d)

readers = {}   # name -> {'n': count, 'last': time}
def make_reader(name):
    readers[name] = {'n': 0, 'last': time.time()}
    return readers[name]

def pipe_reader(i):
    name = 'pipe%d' % i; st = make_reader(name); register(name)
    r, w = os.pipe()
    pipes[name] = w
    while not stop.is_set():
        try: os.read(r, 1)
        except OSError: break
        st['n'] += 1; st['last'] = time.time()

def tcp_reader(i):
    name = 'tcp%d' % i; st = make_reader(name)
    ls = socket.socket(); ls.bind(('127.0.0.1', 0)); ls.listen(1)
    c = socket.create_connection(ls.getsockname()); a, _ = ls.accept()
    socks[name] = a
    register(name)
    while not stop.is_set():
        try: c.recv(1)
        except OSError: break
        st['n'] += 1; st['last'] = time.time()

pipes = {}; socks = {}
def poker():
    # wake every pipe/tcp reader a few times per second, like a peer sending data
    while not stop.is_set():
        for w in list(pipes.values()):
            try: os.write(w, b'x')
            except OSError: pass
        for a in list(socks.values()):
            try: a.send(b'x')
            except OSError: pass
        time.sleep(0.3)

def idler(i):
    register('idle%d' % i)
    ev = threading.Event()
    while not stop.is_set():
        ev.wait(3600)

def sleeper(i):
    register('sleep%d' % i)
    while not stop.is_set():
        time.sleep(0.02 + 0.001 * i)

# ---------------------------------------------------------------- mode 'newrecv': a new thread + a connected UDP socket + blocking recvfrom, data already flowing
if args.mode == 'newrecv':
    import random
    try: host_pid = int(open('Z:/proc/self/stat' if IS_WIN else '/proc/self/stat').read().split()[0])
    except OSError: host_pid = os.getpid()
    print('host_pid=%d' % host_pid, flush=True)
    spin_stop = threading.Event()
    def spinner():
        x = 0
        while not spin_stop.is_set():
            for _ in range(20000): x += 1
            time.sleep(0.0005)
    for _ in range(args.spin): threading.Thread(target=spinner, daemon=True).start()
    # a crowd of idle threads like the game has, so the process has >100 threads (FEX's sampler/snapshot needs that)
    for i in range(args.idle): threading.Thread(target=idler, args=(i,), daemon=True).start()
    wedged = 0
    for it in range(args.iterations):
        r = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); r.bind(('127.0.0.1', 0))
        sn = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); sn.bind(('127.0.0.1', 0))
        r.connect(sn.getsockname()); sn.connect(r.getsockname())
        got = {'n': 0}; quit_ = threading.Event()
        def recv_loop():
            while not quit_.is_set():
                try: r.recv(2048)
                except OSError: break
                got['n'] += 1
        def send_loop():
            while not quit_.is_set():
                try: sn.send(b'p' * 150)
                except OSError: break
                time.sleep(0.005)
        pre = random.choice([0, 0, 1, 3])             # packets already queued before the receiver thread starts
        for _ in range(pre): sn.send(b'p' * 150)
        ts = threading.Thread(target=send_loop, daemon=True); ts.start()
        time.sleep(random.random() * 0.01)
        tr = threading.Thread(target=recv_loop, daemon=True); tr.start()
        t0 = time.time()
        while got['n'] == 0 and time.time() - t0 < 2.5: time.sleep(0.01)
        ok = got['n'] > 0
        time.sleep(0.15)
        if not ok:
            wedged += 1
            print('iteration %d: RECEIVER WEDGED (pre-queued %d, sender still sending, queue not drained)' % (it, pre), flush=True)
            time.sleep(args.hold)
            print('RESULT: RECEIVER WEDGED at iteration %d (wedged %d)' % (it, wedged), flush=True)
            os._exit(3)
        quit_.set()
        try: sn.send(b'q')
        except OSError: pass
        try: r.send(b'q')
        except OSError: pass
        tr.join(1); ts.join(1)
        r.close(); sn.close()
        if it % 25 == 0: print('  iteration %d ok (received %d)' % (it, got['n']), flush=True)
    print('RESULT: ok  %d iterations, no wedge' % args.iterations, flush=True)
    os._exit(0)

# ---------------------------------------------------------------- the 'collector'
stats = {'stops': 0, 'max_suspend': 0.0, 'max_ctx': 0.0, 'max_resume': 0.0, 'max_stop_total': 0.0, 'failed': 0}
verdict = {'rc': 0, 'why': ''}

if IS_WIN and not args.dry:
    import ctypes.wintypes as wt
    k = ctypes.PyDLL('kernel32', use_last_error=True)      # PyDLL keeps the GIL: nothing else can be holding it while we stop the world
    k.OpenThread.restype = wt.HANDLE; k.OpenThread.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k.SuspendThread.restype = wt.DWORD; k.SuspendThread.argtypes = [wt.HANDLE]
    k.ResumeThread.restype = wt.DWORD; k.ResumeThread.argtypes = [wt.HANDLE]
    k.GetThreadContext.restype = wt.BOOL; k.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
    k.CloseHandle.argtypes = [wt.HANDLE]
    ACCESS = 0x2 | 0x8 | 0x40    # SUSPEND_RESUME | GET_CONTEXT | QUERY_INFORMATION
    FLAGS = 0x100000 | 0x1 | 0x2  # CONTEXT_AMD64 | CONTROL | INTEGER

    handles = {}
    def get_handle(tid):
        if tid not in handles:
            handles[tid] = k.OpenThread(ACCESS, False, tid)
        return handles[tid]

    ctxbuf = (ctypes.c_ubyte * (1232 + 64))()
    ctxaddr = (ctypes.addressof(ctxbuf) + 15) & ~15

    def world_stop():
        with tlock:
            items = list(targets.items())
        t0 = time.perf_counter(); done = []
        for name, tid in items:
            h = get_handle(tid)
            if not h:
                stats['failed'] += 1; continue
            a = time.perf_counter()
            r = k.SuspendThread(h)
            b = time.perf_counter()
            ctypes.memset(ctxaddr, 0, 1232); ctypes.c_uint32.from_address(ctxaddr + 0x30).value = FLAGS
            ok = k.GetThreadContext(h, ctypes.c_void_p(ctxaddr))
            c = time.perf_counter()
            done.append((name, h))
            stats['max_suspend'] = max(stats['max_suspend'], b - a)
            stats['max_ctx'] = max(stats['max_ctx'], c - b)
            if r == 0xFFFFFFFF or not ok: stats['failed'] += 1
        for name, h in done:
            a = time.perf_counter()
            k.ResumeThread(h)
            stats['max_resume'] = max(stats['max_resume'], time.perf_counter() - a)
        stats['max_stop_total'] = max(stats['max_stop_total'], time.perf_counter() - t0)
        stats['stops'] += 1
else:
    def world_stop():
        time.sleep(0.001); stats['stops'] += 1

def collector():
    while not stop.is_set():
        # if a world stop does not complete, faulthandler (a C thread, no GIL needed) dumps every Python stack (a wrapper outside then takes a snapshot)
        faulthandler.dump_traceback_later(args.stuck, exit=False, file=LOGF) if LOGF else faulthandler.dump_traceback_later(args.stuck, exit=False)
        world_stop()
        faulthandler.cancel_dump_traceback_later()
        time.sleep(args.period)

# ---------------------------------------------------------------- run
threads = [threading.Thread(target=receiver, daemon=True), threading.Thread(target=sender, daemon=True)]
threads += [threading.Thread(target=idler, args=(i,), daemon=True) for i in range(args.idle)]
threads += [threading.Thread(target=sleeper, args=(i,), daemon=True) for i in range(args.sleepers)]
threads += [threading.Thread(target=pipe_reader, args=(i,), daemon=True) for i in range(args.io)]
threads += [threading.Thread(target=tcp_reader, args=(i,), daemon=True) for i in range(args.io)]
threads += [threading.Thread(target=poker, daemon=True)]

def gc_spinner():
    x = 0
    while not stop.is_set():
        for _ in range(20000): x += 1
        time.sleep(0.0005)
threads += [threading.Thread(target=gc_spinner, daemon=True) for _ in range(args.spin)]
for t in threads: t.start()
time.sleep(0.5)
col = threading.Thread(target=collector, daemon=True); col.start()
try: host_pid = int(open('Z:/proc/self/stat' if IS_WIN else '/proc/self/stat').read().split()[0])
except OSError: host_pid = os.getpid()
print('host_pid=%d' % host_pid, flush=True)
print('repro: port %d, %d targets, world stop every %.2fs for %.0fs%s' % (port, len(targets), args.period, args.seconds, ' (dry)' if args.dry or not IS_WIN else ''), flush=True)
t_start = time.time(); t_end = time.time() + args.seconds
last_print = 0
while time.time() < t_end:
    time.sleep(0.2)
    now = time.time()
    for nm, st in list(readers.items()):
        if now - st['last'] > args.recv_stall and now - t_start > 3:
            verdict = {'rc': 3, 'why': 'READER WEDGED: %s got nothing for %.1fs (n=%d) although poked every 0.3s' % (nm, now - st['last'], st['n'])}
            print(verdict['why'], flush=True); time.sleep(args.hold); break
    if verdict['rc']: break
    if now - rx['last'] > args.recv_stall and tx['n'] > rx['n'] + 10:
        verdict = {'rc': 3, 'why': 'RECEIVER WEDGED: no packet for %.1fs (sent %d, received %d)' % (now - rx['last'], tx['n'], rx['n'])}
        print(verdict['why'], flush=True)
        time.sleep(args.hold)      # keep every thread alive so a wrapper can take a thread snapshot
        break
    if now - last_print > 5:
        last_print = now
        print('  t=%4.0fs sent %6d recv %6d  world stops %5d  max suspend %.0f ms ctx %.0f ms resume %.0f ms  stop-total %.0f ms' % (
            args.seconds - (t_end - now), tx['n'], rx['n'], stats['stops'], stats['max_suspend'] * 1e3, stats['max_ctx'] * 1e3, stats['max_resume'] * 1e3, stats['max_stop_total'] * 1e3), flush=True)
stop.set()
print('RESULT: %s  sent=%d recv=%d stops=%d failed=%d max_suspend=%.0fms max_ctx=%.0fms max_stop_total=%.0fms' % (
    verdict['why'] or 'ok', tx['n'], rx['n'], stats['stops'], stats['failed'], stats['max_suspend'] * 1e3, stats['max_ctx'] * 1e3, stats['max_stop_total'] * 1e3), flush=True)
os._exit(verdict['rc'])
