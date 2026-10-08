#!/usr/bin/env python3
# Runs in the VM as root. Stall-specific trigger for the pre-join stall: the Unity log printed "Locating best region" but not
# "Got best network region" for TRIG seconds (healthy sessions: 2-10 s; stalled ones: 62-113 s). At +TRIG and +TRIG2 it takes
# the same passive capture as wedgewatch.py (FEX all-thread snapshot via patch 0006, maps, thread names, per-thread syscalls,
# fd tables, wineserver epoll + ss). No ptrace. Also writes one 'healthy' baseline capture 20 s after world entry if asked.
#   stallwatch.py LABEL SECONDS LOGFILE [TRIG=12] [TRIG2=40] [BASELINE=0]
import os, sys, time, importlib.util
label, secs, logf = sys.argv[1], float(sys.argv[2]), sys.argv[3]
TRIG = float(sys.argv[4]) if len(sys.argv) > 4 else 12; TRIG2 = float(sys.argv[5]) if len(sys.argv) > 5 else 40
BASE = len(sys.argv) > 6 and sys.argv[6] == '1'
sys.argv = ['wedgewatch.py', label, '0']                      # import wedgewatch's helpers without running its loop for long
# output directory: $VRC_WORK or the current directory (as for wedgewatch.py)
spec = importlib.util.spec_from_file_location('ww', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wedgewatch.py'))
src = open(spec.origin).read().split('big_since = None; s_since = None')[0]   # definitions only, not the watch loop
end = time.time() + secs
ns = {'__name__': 'ww'}
# wedgewatch waits for the VRChat.exe pid at import time; give it the full window
src = src.replace("secs = float(sys.argv[2])", "secs = %r" % secs)
exec(compile(src, spec.origin, 'exec'), ns)
log = ns['log']; snapshot = ns['snapshot']
def rd(p):
    try: return open(p, errors='replace').read()
    except Exception: return ''
t_loc = None; n = 0; done_world = False
while time.time() < end:
    t = rd(logf)
    if t_loc is None and 'Locating best region' in t: t_loc = time.time(); log.write('%.1f saw Locating best region\n' % (time.time() - ns['start']))
    if 'Got best network region' in t and t_loc and n == 0:
        log.write('%.1f healthy: region found after %.1f s\n' % (time.time() - ns['start'], time.time() - t_loc)); t_loc = -1
    if t_loc and t_loc > 0:
        el = time.time() - t_loc
        if n == 0 and el >= TRIG: n = 1; snapshot('region lookup stalled %.0f s' % el, 1)
        if n == 1 and el >= TRIG2 and 'Got best network region' not in rd(logf): n = 2; snapshot('region lookup stalled %.0f s' % el, 2); break
    if BASE and not done_world and 'Finished entering world' in t:
        time.sleep(20); snapshot('healthy baseline', 9); done_world = True; break
    if t_loc == -1 and not BASE: break
    time.sleep(0.5)
log.write('end\n')
