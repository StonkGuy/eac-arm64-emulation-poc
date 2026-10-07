#!/usr/bin/env python3
# Runs in the VM as root. Light-weight watcher for the two failure modes: (1) the Photon UDP socket's receive queue stops draining,
# (2) the main thread sleeps for 25 s during start-up. On either it takes an in-FEX thread snapshot (needs FEX_PROFILESAMPLEHZ>0)
# and copies snapshot, maps, thread names, socket state. usage: wedgewatch.py LABEL SECONDS
import os, sys, time, subprocess, shutil
label = sys.argv[1]; secs = float(sys.argv[2]); OUT = os.environ.get('VRC_WORK', os.getcwd()).rstrip('/') + '/'
def rd(p):
    try:
        with open(p) as f: return f.read()
    except Exception: return ''
def pid():
    r = subprocess.run(['pgrep', '-x', 'VRChat.exe'], capture_output=True, text=True).stdout.split()
    return int(r[0]) if r else 0
end = time.time() + secs
P = 0
while not P and time.time() < end: P = pid(); time.sleep(0.5)
if not P: sys.exit(0)
start = time.time()
trig = '/dev/shm/fex-%d-snapshot' % P; snapf = '/dev/shm/fex-%d-snap' % P
log = open(OUT + 'wedge_%s.log' % label, 'w', buffering=1)
def big_udp():
    m = 0
    for l in rd('/proc/net/udp').splitlines()[1:]:
        f = l.split()
        if len(f) >= 5 and not f[2].startswith('00000000:0000'):
            m = max(m, int(f[4].split(':')[1], 16))
    return m
def ws_epoll_info(tag):
    """What does wineserver's epoll set say about the sockets that have data waiting (any TCP/UDP socket of the game with a non-empty
    receive queue, and the game-server UDP socket)? Reads wineserver's /proc only."""
    import re
    try:
        ss = subprocess.run("ss -tuanpie 2>/dev/null | grep -A1 'VRChat.exe'", shell=True, capture_output=True, text=True, timeout=10).stdout
        ws = subprocess.run(['pgrep', '-x', 'wineserver'], capture_output=True, text=True).stdout.split()
        out = ['ss (sockets of VRChat.exe):\n' + ss]
        if ws:
            w = ws[0]
            wsfds = []
            for ln in ss.splitlines():
                f = ln.split()
                if len(f) < 6 or 'VRChat.exe' not in ln: continue
                rx = f[2] if f[0] in ('tcp', 'udp') else f[1]   # `ss -tu` prints a protocol column first
                m = re.search(r'wineserver",pid=%s,fd=(\d+)' % w, ln)
                if m and ((rx.isdigit() and int(rx) > 0) or re.search(r':2700\d\s', ln)): wsfds.append(m.group(1))
            wsfds = sorted(set(wsfds), key=int)
            out.append('wineserver pid %s holds socket fds with data waiting / the game-server socket: %s' % (w, wsfds))
            for fd in wsfds:
                out.append('fdinfo %s: %s' % (fd, rd('/proc/%s/fdinfo/%s' % (w, fd)).replace('\n', ' | ')))
            ls = subprocess.run('ls -l /proc/%s/fd' % w, shell=True, capture_output=True, text=True).stdout
            for m in re.finditer(r' (\d+) -> anon_inode:\[eventpoll\]', ls):
                ep = m.group(1); fi = rd('/proc/%s/fdinfo/%s' % (w, ep)); n = fi.count('tfd:')
                out.append('eventpoll fd %s: %d registered' % (ep, n))
                for ln in fi.split('\n'):
                    if ln.startswith('tfd:') and ln.split()[1] in wsfds: out.append('   ' + ln)
        open(OUT + 'snapepoll_%s_%s.txt' % (label, tag), 'w').write('\n'.join(out) + '\n')
    except Exception as e: log.write('epoll info failed: %s\n' % e)

def snapshot(reason, n):
    log.write('%.1f SNAPSHOT %d: %s\n' % (time.time() - start, n, reason))
    ws_epoll_info('wedge%d' % n)
    open(trig, 'w').close()
    for _ in range(40):
        time.sleep(0.25)
        if not os.path.exists(trig): break
    time.sleep(1.5)
    for src, dst in ((snapf, 'snap_%s_%d.bin' % (label, n)), ('/proc/%d/maps' % P, 'snapmaps_%s_%d.txt' % (label, n))):
        try: shutil.copy(src, OUT + dst)
        except Exception as e: log.write('copy %s failed: %s\n' % (src, e))
    names = []
    for t in os.listdir('/proc/%d/task' % P):
        names.append('%s %s' % (t, rd('/proc/%d/task/%s/comm' % (P, t)).strip()))
    open(OUT + 'snapnames_%s_%d.txt' % (label, n), 'w').write('\n'.join(names) + '\n')
    sc = []
    for t in os.listdir('/proc/%d/task' % P):
        sc.append('%s %s | %s' % (t, rd('/proc/%d/task/%s/comm' % (P, t)).strip(), rd('/proc/%d/task/%s/syscall' % (P, t)).strip()[:90]))
    open(OUT + 'snapsys_%s_%d.txt' % (label, n), 'w').write('\n'.join(sc) + '\n')
    # file descriptor tables (pipe and socket inodes) and, when FEX_SIGNALTRACE is on, the trace rings of the game and of wineserver
    try:
        import gzip
        wsp = subprocess.run(['pgrep', '-x', 'wineserver'], capture_output=True, text=True).stdout.split()
        for who, q in (('game', str(P)), ('ws', wsp[0] if wsp else '')):
            if not q: continue
            open(OUT + 'snapfds_%s_%d_%s.txt' % (label, n, who), 'w').write(subprocess.run('ls -l /proc/%s/fd' % q, shell=True, capture_output=True, text=True).stdout)
            ring = '/dev/shm/fex-%s-sigtrace' % q
            if os.path.exists(ring):
                with open(ring, 'rb') as fi, gzip.open(OUT + 'sigtrace_%s_%d_%s.gz' % (label, n, who), 'wb', 3) as fo: shutil.copyfileobj(fi, fo)
    except Exception as e: log.write('fd/ring capture failed: %s\n' % e)
    try:
        ws = subprocess.run(['pgrep', '-x', 'wineserver'], capture_output=True, text=True).stdout.split()
        if ws:
            shutil.copy('/proc/%s/maps' % ws[0], OUT + 'snapws_maps_%s_%d.txt' % (label, n))
            open(OUT + 'snapws_fd_%s_%d.txt' % (label, n), 'w').write(subprocess.run('ls -l /proc/%s/fd | grep -i "shm\\|fsync\\|socket\\|epoll" | head -60; mount | grep -i "shm\\|virtiofs\\|fuse"' % ws[0], shell=True, capture_output=True, text=True).stdout)
    except Exception as e: log.write('wineserver capture failed: %s\n' % e)
    out = subprocess.run("ss -uanpie 2>/dev/null | grep -A1 -E ':2700[0-9]'; echo; echo TCP; ss -tanpie 2>/dev/null | head -120; echo; curl -sS -m 8 -o /dev/null -w 'api %{http_code} total=%{time_total}\\n' https://api.vrchat.cloud/api/1/config 2>&1", shell=True, capture_output=True, text=True, timeout=20).stdout
    open(OUT + 'snapnet_%s_%d.txt' % (label, n), 'w').write(out)
big_since = None; s_since = None; n = 0; last_q = -1; sock_seen = None; baseline_done = False
while time.time() < end:
    now = time.time()
    try: st = rd('/proc/%d/stat' % P).rsplit(')', 1)[1].split()[0]
    except Exception: break
    q = big_udp()
    if sock_seen is None and any(not l.split()[2].startswith('00000000:0000') for l in rd('/proc/net/udp').splitlines()[1:] if len(l.split()) > 2 and l.split()[2].upper().endswith(':697A')): sock_seen = now
    if sock_seen and not baseline_done and now - sock_seen > 14:
        baseline_done = True; ws_epoll_info('baseline')
    if q != last_q and (q > 0 or last_q > 0): log.write('%.1f udp_rx=%d\n' % (now - start, q)); last_q = q
    big_since = (big_since or now) if q > 65536 else None
    s_since = (s_since or now) if st == 'S' else None
    if n < 2 and ((big_since and now - big_since > 6) or (s_since and now - s_since > 25 and now - start > 40 and rd('/proc/%d/task/%d/schedstat' % (P, P)).split()[0:1] != [])):
        # main thread sleeping 25 s straight during normal play would be unusual; confirm the UDP queue is not draining either
        if big_since or (s_since and now - start < 150):
            n += 1; snapshot('udp backlog' if big_since else 'main thread asleep', n)
            big_since = None; s_since = None; time.sleep(15)
    time.sleep(1)
log.write('end\n')
