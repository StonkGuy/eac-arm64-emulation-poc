#!/usr/bin/env python3
"""Reads the DXVK HUD 'FPS: nn.n' from the VRChat window with XGetImage. usage: hudfps.py [seconds] [interval] -> prints 'epoch fps' lines."""
import sys, time, json, os
sys.argv_backup = sys.argv; sys.argv = ['x', 'list']
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'xin.py')).read().split("a = sys.argv[1:]")[0])
TPL = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hud_templates.json')
templates = json.load(open(TPL)) if os.path.exists(TPL) else {}
def cells(rows):
    # find the text row band (y 14..24 in the client) and digit cells starting after 'FPS:' (x0=69, advance 12)
    out = []
    for k in range(4):
        x0 = 69 + 12 * k
        out.append(''.join('#' if (rows[y][x][0] > 200 and rows[y][x][1] > 200 and rows[y][x][2] > 200) else '.' for y in range(14, 24) for x in range(x0, x0 + 10)))
    return out
def read(win):
    rows = grab(win, 0, 0, 200, 40)
    if rows is None: return None
    cs = cells(rows); s = ''
    for k, c in enumerate(cs):
        if c.count('#') == 0: s += '?' if k != 2 else ''; continue
        s += templates.get(c, '?')
    return s, cs
if __name__ == '__main__':
    dur = float(sys.argv_backup[1]) if len(sys.argv_backup) > 1 else 10; iv = float(sys.argv_backup[2]) if len(sys.argv_backup) > 2 else 0.5
    win = find_window('VRChat'); end = time.time() + dur
    while time.time() < end:
        r = read(win)
        if r: print("%.1f %s" % (time.time(), r[0]), flush=True)
        time.sleep(iv)
