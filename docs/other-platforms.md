# Other platforms

The emulator patches only need **FEX-Emu 2610 on aarch64 Linux** with a kernel that allows the usual `ptrace`
between a process and its child (`kernel.yama.ptrace_scope` ≤ 1 is fine for a parent tracing its own child). They are
not Asahi-specific. Everything else in this repository (muvm scripts, host tuning) is.

**Only one configuration has actually been run here: VRChat on an Apple M2 under Fedora Asahi Remix 44 (muvm, FEX-2610).** Every other host below is untried — the rows say what is *expected*, not what was measured.

| Platform | Status here | Notes |
|---|---|---|
| Apple M2 / Fedora Asahi Remix 44 (muvm, FEX-2610) | **the only tested setup** | 16K-page host: needs the muvm 4K guest. This is the machine everything in this repository was measured on. |
| Asahi Linux on other M-series hosts (M1, M3/M4) | untested | Expected to work the same way as the M2 setup (16K-page host, same muvm path), but it has not been tried. |
| Valve Steam Frame (arm64 SteamOS, FEX + Proton) | **untested** | Same FEX/Proton stack in principle, 4K pages, no VM layer; rebuild the patched FEX and use it as the x86 interpreter. EAC behaviour on SteamOS/arm64 is unknown. |
| Any arm64 Linux with 4K pages + FEX | **untested** | `scripts/build-fex.sh`, then register the patched binary with binfmt_misc. |
| macOS on Apple silicon | **plan, untested** | A Linux arm64 guest under **libkrun** (not UTM/Parallels) with FEX + Venus, like the `steamac` project; the one open question is whether EAC accepts the guest. See [macos-port.md](macos-port.md) and [platform-viability.md](platform-viability.md). |

If you try one of these, `tests/ptrace-inject` is the quickest check that the emulation behaves; please report results.
