# Other platforms

The emulator patches only need **FEX-Emu 2609.1 on aarch64 Linux** with a kernel that allows the usual `ptrace`
between a process and its child (`kernel.yama.ptrace_scope` ≤ 1 is fine for a parent tracing its own child). They are
not Asahi-specific. Everything else in this repository (muvm scripts, host tuning) is.

| Platform | Expectation | Notes |
|---|---|---|
| Asahi Linux (M1/M2) | works (tested on M2) | 16K-page host: needs the muvm VM. |
| Valve Steam Frame (arm64 SteamOS, FEX + Proton) | *untested* | Same FEX/Proton stack, 4K pages, no VM layer; rebuild the patched FEX and use it as the x86 interpreter. EAC behaviour on SteamOS/arm64 is unknown. |
| Any arm64 Linux with 4K pages + FEX | *untested* | `scripts/build-fex.sh`, then register the patched binary with binfmt_misc. |
| macOS on Apple silicon | *untested* | Run a Linux arm64 guest (UTM/Parallels/…), install FEX and Steam there. GPU virtualisation quality decides playability. |

If you try one of these, `tests/ptrace-inject` is the quickest check that the emulation behaves; please report results.
