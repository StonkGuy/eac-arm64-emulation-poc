#!/usr/bin/env python3
"""Parse FEX live-stats shm files (FEX_PROFILESTATS=1). usage: fexstats.py FILE [FILE...]  (times are CNTVCT ticks @24MHz)"""
import struct, sys
HDR = 64; FREQ = 24e6
# per-thread slot: u32 next, u32 tid, then u64 fields (older FEX: 13 fields incl. padding; newer: 15 with invalidation stats)
F = ["JIT", "Signal", "SIGBUS", "SMC", "FloatFB", "CacheMiss", "CacheRdLock", "CacheWrLock", "JITCount", "DiskHit", "DiskMiss", "DiskLookup", "InvCount", "InvTime", "CallRetFlush"]
def parse(path):
    d = open(path, "rb").read()
    ver, app, tsz = struct.unpack_from("<BBH", d, 0)
    ver_s = d[4:52].split(b"\0")[0].decode(errors="replace")
    out = []
    nf = (tsz - 8) // 8
    for off in range(HDR, len(d) - tsz + 1, tsz):
        nxt, tid = struct.unpack_from("<II", d, off)
        vals = list(struct.unpack_from("<%dQ" % nf, d, off + 8)) + [0] * 15
        vals = vals[:15]
        if tid == 0 and not any(vals): continue
        out.append((tid, vals))
    return ver, app, tsz, ver_s, out
if __name__ == "__main__":
    for p in sys.argv[1:]:
        ver, app, tsz, vs, rows = parse(p)
        print(f"== {p}  statsver={ver} app={app} slot={tsz} fex={vs}")
        print(f"{'tid':>8} {'jit_s':>8} {'sig_s':>8} {'smc':>8} {'sigbus':>7} {'fpfb':>8} {'cmiss':>9} {'jitcnt':>8} {'inval':>8} {'inv_s':>8} {'crflush':>8}")
        tot = [0]*15
        for tid, v in rows:
            print(f"{tid:>8} {v[0]/FREQ:8.2f} {v[1]/FREQ:8.2f} {v[3]:8d} {v[2]:7d} {v[4]:8d} {v[5]:9d} {v[8]:8d} {v[12]:8d} {v[13]/FREQ:8.2f} {v[14]:8d}")
            tot = [a+b for a, b in zip(tot, v)]
        print(f"{'TOTAL':>8} {tot[0]/FREQ:8.2f} {tot[1]/FREQ:8.2f} {tot[3]:8d} {tot[2]:7d} {tot[4]:8d} {tot[5]:9d} {tot[8]:8d} {tot[12]:8d} {tot[13]/FREQ:8.2f} {tot[14]:8d}")
