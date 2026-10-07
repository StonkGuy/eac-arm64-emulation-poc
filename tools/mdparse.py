#!/usr/bin/env python3
"""Minimal Windows minidump summary: exception record, faulting module+offset, registers, stack module hits."""
import struct, sys
d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, dir_rva = struct.unpack_from("<4sIII", d, 0)
assert sig == b"MDMP", "not a minidump"
streams = {}
for i in range(nstreams):
    t, sz, rva = struct.unpack_from("<III", d, dir_rva + 12 * i); streams.setdefault(t, []).append((sz, rva))
print("streams:", {k: len(v) for k, v in streams.items()})
def ustr(rva):
    n, = struct.unpack_from("<I", d, rva); return d[rva + 4: rva + 4 + n].decode("utf-16le", "replace")
mods = []
if 4 in streams:
    sz, rva = streams[4][0]; n, = struct.unpack_from("<I", d, rva)
    for i in range(n):
        base, size, csum, ts, name_rva = struct.unpack_from("<QIIII", d, rva + 4 + 108 * i)
        mods.append((base, size, ustr(name_rva)))
def modof(a):
    for b, s, n in mods:
        if b <= a < b + s: return "%s+0x%x" % (n.split("\\")[-1], a - b)
    return "?"
EXC = {0xc0000005: "ACCESS_VIOLATION", 0xc000001d: "ILLEGAL_INSTRUCTION", 0xc00000fd: "STACK_OVERFLOW", 0xc0000094: "INT_DIVIDE_BY_ZERO", 0x80000003: "BREAKPOINT", 0xc0000096: "PRIV_INSTRUCTION", 0xc000001e: "INVALID_LOCK_SEQ", 0xe06d7363: "C++ EXCEPTION", 0x80000004: "SINGLE_STEP", 0xc0000409: "STACK_BUFFER_OVERRUN", 0xc0000374: "HEAP_CORRUPTION"}
if 6 in streams:
    sz, rva = streams[6][0]
    tid, _ = struct.unpack_from("<II", d, rva)
    code, flags, rec, addr, nparam = struct.unpack_from("<IIQQI", d, rva + 8)
    info = struct.unpack_from("<15Q", d, rva + 8 + 32)
    ctx_sz, ctx_rva = struct.unpack_from("<II", d, rva + 8 + 32 + 120)
    print("exception thread=%d code=0x%08x (%s) addr=0x%x [%s] params=%s" % (tid, code, EXC.get(code, "?"), addr, modof(addr), [hex(x) for x in info[:nparam]]))
    # x64 CONTEXT: P1Home..P6Home (48) ContextFlags(4) MxCsr(4) SegCs.. EFlags(4)... DR0-7 (48) Rax.. at 0x78
    names = ["rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "rip"]
    vals = struct.unpack_from("<17Q", d, ctx_rva + 0x78)
    for n, v in zip(names, vals): print("  %-4s %016x %s" % (n, v, modof(v) if v > 0x10000 else ""))
    rsp = vals[4]
    # find stack memory for rsp
    if 9 in streams:
        sz9, rva9 = streams[9][0]; n, base_rva = struct.unpack_from("<QQ", d, rva9)[0], struct.unpack_from("<Q", d, rva9 + 8)[0]
        off = base_rva
        for i in range(n):
            start, size = struct.unpack_from("<QQ", d, rva9 + 16 + 16 * i)
            if start <= rsp < start + size:
                words = struct.unpack_from("<%dQ" % min(64, (start + size - rsp) // 8), d, off + (rsp - start))
                print("stack words from rsp with module hits:")
                for j, w in enumerate(words):
                    m = modof(w)
                    if m != "?": print("   rsp+0x%x: %016x %s" % (j * 8, w, m))
                break
            off += size
print("modules:", len(mods)); 
for b, s, n in mods[:6]: print("  %016x %8x %s" % (b, s, n.split("\\")[-1]))
