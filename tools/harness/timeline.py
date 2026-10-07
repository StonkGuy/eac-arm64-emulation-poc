#!/usr/bin/env python3
# Runs in the VM. Every 0.5 s: per-thread cumulative CPU ticks of the game + system counters. usage: timeline.py SECONDS OUTFILE
import os, sys, time, subprocess
secs = float(sys.argv[1]); out = open(sys.argv[2], 'w', buffering=1)
def pid(name):
    r = subprocess.run(['pgrep', '-x', name], capture_output=True, text=True).stdout.split()
    return int(r[0]) if r else 0
P = pid('VRChat.exe')
while not P: time.sleep(0.5); P = pid('VRChat.exe')
WS = pid('wineserver')
def rd(path):
    try:
        with open(path) as f: return f.read()
    except Exception: return ''
def vm():
    d = {}
    for l in rd('/proc/vmstat').splitlines():
        k, v = l.split()
        if k in ('pgfault', 'pgmajfault', 'pswpin', 'pswpout', 'allocstall_normal', 'pgscan_direct', 'pgscan_kswapd', 'compact_stall', 'thp_fault_alloc', 'nr_dirty', 'nr_writeback'): d[k] = v
    return d
def pres(n):
    return rd('/proc/pressure/' + n).splitlines()[0].split('total=')[1] if rd('/proc/pressure/' + n) else '0'
def udpq():
    # connected UDP sockets (the Photon game server): rx_queue bytes and drops, from /proc/net/udp
    r = []
    for l in rd('/proc/net/udp').splitlines()[1:]:
        f = l.split()
        if len(f) >= 13 and not f[2].startswith('00000000:0000'):
            rx = int(f[4].split(':')[1], 16)
            r.append('%s>%d/d%s' % (f[2].split(':')[1], rx, f[12]))
    return ','.join(r) if r else '-'
last_dump = 0
last_total = -1; last_change = time.time(); diags = 0; last_diag = 0
def sh(cmd, to=10):
    try: return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=to).stdout
    except Exception as e: return 'ERR %s' % e
start_t = time.time()
end = time.time() + secs
while time.time() < end:
    now = time.time()
    rows = []
    try: tids = os.listdir('/proc/%d/task' % P)
    except Exception: break
    for t in tids:
        s = rd('/proc/%d/task/%s/stat' % (P, t))
        if not s: continue
        c = s.split(')')[0].split('(', 1)[1].replace(' ', '_'); f = s.rsplit(')', 1)[1].split()
        rows.append('%s:%s:%d:%s' % (t, c, int(f[11]) + int(f[12]), f[0]))
    ws = rd('/proc/%d/stat' % WS).rsplit(')', 1)[1].split() if WS else None
    v = vm(); mi = rd('/proc/meminfo')
    avail = [l.split()[1] for l in mi.splitlines() if l.startswith('MemAvailable')][0]
    out.write('T %.2f ws=%s cpu_psi=%s mem_psi=%s io_psi=%s avail=%s %s\n' % (now, (int(ws[11]) + int(ws[12])) if ws else 0, pres('cpu'), pres('memory'), pres('io'), avail, ' '.join('%s=%s' % kv for kv in v.items())))
    out.write('R %.2f %s\n' % (now, ' '.join(rows)))
    sl = []
    for t in tids:
        ss = rd('/proc/%d/task/%s/schedstat' % (P, t)).split()
        if len(ss) == 3: sl.append('%s:%s' % (t, ss[2]))
    out.write('S %.2f %s\n' % (now, ' '.join(sl)))
    total = sum(int(r.rsplit(':', 2)[1]) for r in rows if r.rsplit(':', 2)[1].isdigit())
    if total != last_total: last_total = total; last_change = now
    ms = [r for r in rows if r.split(':', 1)[0] == str(P)]
    if ms and ms[0].rsplit(':', 1)[1].strip() == 'S':
        if globals().get('main_s_since') is None: globals()['main_s_since'] = now
    else: globals()['main_s_since'] = None
    # hang diagnostics: the game is alive but no thread has made CPU progress for 20 s
    if ((now - last_change > 20) or (globals().get('main_s_since') is not None and now - globals()['main_s_since'] > 20)) and now - start_t > 25 and diags < 3 and now - last_diag > 40:
        diags += 1; last_diag = now
        out.write('X %.2f BEGIN hang diag %d (no CPU progress for %.0f s)\n' % (now, diags, now - last_change))
        blk = []
        blk.append('curl api: ' + sh("curl -sS -m 8 -o /dev/null -w '%{http_code} dns=%{time_namelookup} connect=%{time_connect} tls=%{time_appconnect} total=%{time_total}' https://api.vrchat.cloud/api/1/config 2>&1; echo", 12).strip())
        blk.append('getent: ' + sh('getent hosts api.vrchat.cloud | head -1').strip())
        blk.append('ss -tanp (game):\n' + sh("ss -tanpi 2>/dev/null | grep -A1 -E 'pid=%d,|pid=%d\\)' | head -60" % (P, P)))
        blk.append('ss -uanp (game):\n' + sh("ss -uanp 2>/dev/null | grep 'pid=%d' | head" % P))
        th = []
        for t in tids:
            sc = rd('/proc/%d/task/%s/syscall' % (P, t)).strip(); w = rd('/proc/%d/task/%s/wchan' % (P, t)).strip(); c = rd('/proc/%d/task/%s/comm' % (P, t)).strip()
            th.append('%s %s %s %s' % (t, c, w, sc[:70]))
        blk.append('threads (tid comm wchan syscall+args):\n' + '\n'.join(th))
        blk.append('wineserver wchan: ' + rd('/proc/%d/wchan' % WS) if WS else '')
        for b in blk:
            for ln in b.split('\n'): out.write('x ' + ln + '\n')
        out.write('X %.2f END\n' % time.time())
    q = udpq(); out.write('U %.2f %s\n' % (now, q))
    # when a UDP socket has a big unread backlog, record what every thread is doing (syscall + kernel wait channel)
    big = any(int(x.split('>')[1].split('/')[0]) > 16384 for x in q.split(',') if '>' in x)
    if big and now - last_dump > 1.0:
        last_dump = now
        d = []
        for t in tids:
            sc = rd('/proc/%d/task/%s/syscall' % (P, t)).split()
            w = rd('/proc/%d/task/%s/wchan' % (P, t)).strip()
            c = rd('/proc/%d/task/%s/comm' % (P, t)).strip().replace(' ', '_')
            if sc and sc[0] != 'running': d.append('%s:%s:%s:%s' % (t, c, sc[0], w))
        out.write('D %.2f %s\n' % (now, ' '.join(d)))
        big_n = globals().get('big_n', 0) + 1; globals()['big_n'] = big_n
        if big_n in (1, 10, 40):
            # the stuck socket: kernel memory/drops, which processes hold it, and whether wineserver still polls it
            ex = sh("ss -uanpie 2>/dev/null | grep -A1 -E ':2700[0-9]' | head -12")
            out.write('X %.2f BEGIN socket diag %d\n' % (now, big_n))
            for ln in ex.split('\n'): out.write('x ' + ln + '\n')
            for ws in ([WS] if WS else []):
                for m in __import__('re').findall(r'fd=(\d+)\)', ex):
                    pass
            fds = set(__import__('re').findall(r'pid=%d,fd=(\d+)' % P, ex)) | set(__import__('re').findall(r'wineserver",pid=\d+,fd=(\d+)', ex))
            wsfd = set(__import__('re').findall(r'wineserver",pid=\d+,fd=(\d+)', ex))
            for fd in sorted(fds):
                out.write('x game fdinfo %s: %s\n' % (fd, rd('/proc/%d/fdinfo/%s' % (P, fd)).replace('\n', ' | ')))
            if WS:
                eps = sh('ls -l /proc/%d/fd 2>/dev/null | grep eventpoll' % WS)
                out.write('x wineserver eventpoll fds: %s\n' % eps.replace('\n', ' ; '))
                for e in __import__('re').findall(r' (\d+) -> anon_inode:\[eventpoll\]', eps):
                    fi = rd('/proc/%d/fdinfo/%s' % (WS, e))
                    for fd in wsfd:
                        for ln in fi.split('\n'):
                            if ln.startswith('tfd:') and ln.split()[1] == fd: out.write('x wineserver epoll %s has: %s\n' % (e, ln))
                    out.write('x wineserver epoll %s: %d registered fds\n' % (e, fi.count('tfd:')))
            out.write('X %.2f END\n' % time.time())
    time.sleep(max(0, 0.5 - (time.time() - now)))
