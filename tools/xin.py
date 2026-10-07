#!/usr/bin/env python3
"""Tiny XTEST input injector (python ctypes; no xdotool on this box).
usage: xin.py pos | list | focus | activate SUBSTR | move DX DY | warp X Y | click [1|2|3] | down N | up N | key KEYNAME [KEYNAME...] | hold KEYNAME SECONDS | drag DX DY SECONDS"""
import ctypes, sys, time
X = ctypes.CDLL("libX11.so.6"); T = ctypes.CDLL("libXtst.so.6")
X.XOpenDisplay.restype = ctypes.c_void_p; X.XOpenDisplay.argtypes = [ctypes.c_char_p]
X.XStringToKeysym.restype = ctypes.c_ulong; X.XStringToKeysym.argtypes = [ctypes.c_char_p]
X.XKeysymToKeycode.restype = ctypes.c_ubyte; X.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
X.XFlush.argtypes = [ctypes.c_void_p]; X.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
X.XDefaultRootWindow.restype = ctypes.c_ulong; X.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
X.XQueryPointer.argtypes = [ctypes.c_void_p, ctypes.c_ulong] + [ctypes.c_void_p]*7
T.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeRelativeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
T.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]
d = X.XOpenDisplay(None)
if not d: sys.exit("cannot open DISPLAY")
def pos():
    rw, cw = ctypes.c_ulong(), ctypes.c_ulong(); rx, ry, wx, wy, m = (ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_uint())
    X.XQueryPointer(d, X.XDefaultRootWindow(d), ctypes.byref(rw), ctypes.byref(cw), ctypes.byref(rx), ctypes.byref(ry), ctypes.byref(wx), ctypes.byref(wy), ctypes.byref(m))
    return rx.value, ry.value
def kc(name):
    k = X.XKeysymToKeycode(d, X.XStringToKeysym(name.encode()))
    if not k: sys.exit("unknown key " + name)
    return k

class XClientMessage(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong), ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong), ("format", ctypes.c_int), ("data", ctypes.c_long * 5)]
class XEvent(ctypes.Union):
    _fields_ = [("xclient", XClientMessage), ("pad", ctypes.c_long * 24)]
X.XInternAtom.restype = ctypes.c_ulong; X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
X.XGetWindowProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_long, ctypes.c_long, ctypes.c_int, ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_void_p)]
X.XGetInputFocus.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong), ctypes.POINTER(ctypes.c_int)]
X.XSendEvent.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_long, ctypes.c_void_p]
X.XFree.argtypes = [ctypes.c_void_p]
def prop(win, name, fmt_atom=0, maxlen=4096):
    a = X.XInternAtom(d, name.encode(), 0); t = ctypes.c_ulong(); f = ctypes.c_int(); n = ctypes.c_ulong(); r = ctypes.c_ulong(); data = ctypes.c_void_p()
    if X.XGetWindowProperty(d, win, a, 0, maxlen, 0, fmt_atom, ctypes.byref(t), ctypes.byref(f), ctypes.byref(n), ctypes.byref(r), ctypes.byref(data)) != 0 or not data.value or n.value == 0:
        return None, 0, 0
    size = {8: 1, 16: 2, 32: 8}[f.value]
    raw = ctypes.string_at(data.value, n.value * size); X.XFree(data.value); return raw, f.value, n.value
def wname(win):
    raw, f, n = prop(win, "_NET_WM_NAME")
    if raw is None: raw, f, n = prop(win, "WM_NAME")
    return raw.decode(errors="replace") if raw else ""
def root(): return X.XDefaultRootWindow(d)
def clients():
    raw, f, n = prop(root(), "_NET_CLIENT_LIST")
    import struct
    return list(struct.unpack("<%dQ" % n, raw)) if raw else []
def active():
    raw, f, n = prop(root(), "_NET_ACTIVE_WINDOW")
    import struct
    return struct.unpack("<Q", raw[:8])[0] if raw else 0
def activate(win):
    ev = XEvent(); ev.xclient.type = 33; ev.xclient.window = win; ev.xclient.message_type = X.XInternAtom(d, b"_NET_ACTIVE_WINDOW", 0)
    ev.xclient.format = 32; ev.xclient.data[0] = 2; ev.xclient.data[1] = 0; ev.xclient.data[2] = 0
    X.XSendEvent(d, root(), 0, (1 << 20) | (1 << 19), ctypes.byref(ev)); X.XFlush(d)

class XImage(ctypes.Structure):
    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int), ("xoffset", ctypes.c_int), ("format", ctypes.c_int), ("data", ctypes.c_void_p),
                ("byte_order", ctypes.c_int), ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int), ("bitmap_pad", ctypes.c_int),
                ("depth", ctypes.c_int), ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]
X.XGetImage.restype = ctypes.POINTER(XImage)
X.XGetImage.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong, ctypes.c_int]
X.XDestroyImage.argtypes = [ctypes.POINTER(XImage)]
def find_window(substr):
    for w in clients():
        if substr.lower() in wname(w).lower(): return w
    return 0
def grab(win, x, y, w, h):
    """returns list of rows of (r,g,b) or None"""
    im = X.XGetImage(d, win, x, y, w, h, 0xffffffff, 2)
    if not im: return None
    bpl = im.contents.bytes_per_line; bpp = im.contents.bits_per_pixel // 8
    raw = ctypes.string_at(im.contents.data, bpl * h)
    rows = []
    for j in range(h):
        row = []
        for i in range(w):
            o = j * bpl + i * bpp; row.append((raw[o + 2], raw[o + 1], raw[o]))
        rows.append(row)
    X.XDestroyImage(im); return rows
a = sys.argv[1:]
if not a: sys.exit(__doc__)
c = a[0]
if c == "pos": print(*pos())
elif c == "list":
    act = active()
    for w in clients(): print(("* " if w == act else "  ") + hex(w), wname(w))
elif c == "grab":
    win = find_window(a[1]); rows = grab(win, int(a[2]), int(a[3]), int(a[4]), int(a[5]))
    if rows is None: sys.exit("XGetImage failed")
    from collections import Counter
    print("grabbed", len(rows[0]), "x", len(rows), "colors:", Counter(p for r in rows for p in r).most_common(5))
    open(a[6], "w").write("P3\n%d %d\n255\n" % (len(rows[0]), len(rows)) + "\n".join(" ".join("%d %d %d" % p for p in r) for r in rows))
elif c == "focus":
    f = ctypes.c_ulong(); rv = ctypes.c_int(); X.XGetInputFocus(d, ctypes.byref(f), ctypes.byref(rv)); act = active()
    print("input_focus=%s active=%s %s" % (hex(f.value), hex(act), wname(act)))
elif c == "activate":
    for w in clients():
        if a[1].lower() in wname(w).lower(): activate(w); print("activated", hex(w), wname(w)); break
    else: sys.exit("no window matching " + a[1])
elif c == "move": T.XTestFakeRelativeMotionEvent(d, int(a[1]), int(a[2]), 0); X.XFlush(d)
elif c == "warp": T.XTestFakeMotionEvent(d, -1, int(a[1]), int(a[2]), 0); X.XFlush(d)
elif c == "click":
    b = int(a[1]) if len(a) > 1 else 1; T.XTestFakeButtonEvent(d, b, 1, 0); X.XFlush(d); time.sleep(0.08); T.XTestFakeButtonEvent(d, b, 0, 0); X.XFlush(d)
elif c == "down": T.XTestFakeButtonEvent(d, int(a[1]), 1, 0); X.XFlush(d)
elif c == "up": T.XTestFakeButtonEvent(d, int(a[1]), 0, 0); X.XFlush(d)
elif c == "key":
    for n in a[1:]:
        k = kc(n); T.XTestFakeKeyEvent(d, k, 1, 0); X.XFlush(d); time.sleep(0.08); T.XTestFakeKeyEvent(d, k, 0, 0); X.XFlush(d); time.sleep(0.15)
elif c == "hold":
    k = kc(a[1]); T.XTestFakeKeyEvent(d, k, 1, 0); X.XFlush(d); time.sleep(float(a[2])); T.XTestFakeKeyEvent(d, k, 0, 0); X.XFlush(d)
elif c == "drag":   # hold right button and move the mouse in small steps (VRChat desktop look)
    dx, dy, secs = int(a[1]), int(a[2]), float(a[3]); steps = max(1, int(secs * 60))
    T.XTestFakeButtonEvent(d, 3, 1, 0); X.XFlush(d)
    for i in range(steps):
        T.XTestFakeRelativeMotionEvent(d, dx // steps, dy // steps, 0); X.XFlush(d); time.sleep(secs / steps)
    T.XTestFakeButtonEvent(d, 3, 0, 0); X.XFlush(d)
X.XSync(d, 0)
