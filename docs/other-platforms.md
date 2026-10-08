# Other platforms

The emulator patches only need **FEX-Emu 2610 on aarch64 Linux** with a kernel that allows the usual `ptrace`
between a process and its child (`kernel.yama.ptrace_scope` ≤ 1 is fine for a parent tracing its own child). They are
not Asahi-specific. Everything else in this repository (muvm scripts, host tuning) is.

**Two configurations have actually been run: VRChat on an Apple M2 under Fedora Asahi Remix 44 (muvm, FEX-2610), and on an Apple M2 MacBook Air under macOS 27 (steamac libkrun guest, FEX-2610).** Every other host below is untried — the rows say what is *expected*, not what was measured.

| Platform | Status here | Notes |
|---|---|---|
| Apple M2 / Fedora Asahi Remix 44 (muvm, FEX-2610) | **tested** | 16K-page host: needs the muvm 4K guest. The machine most of this repository was measured on. |
| Asahi Linux on other M-series hosts (M1, M3/M4) | untested | Expected to work the same way as the M2 setup (16K-page host, same muvm path), but it has not been tried. |
| Valve Steam Frame (arm64 SteamOS, FEX + Proton) | **untested** | The macOS guest is this same SteamOS image and compatibility-tool mechanism, so the same FEX swap is expected to apply; unverified. 4K pages, no VM layer; rebuild the patched FEX and use it as the x86 interpreter. EAC behaviour on SteamOS/arm64 is unknown. |
| Any arm64 Linux with 4K pages + FEX | **untested** | `scripts/build-fex.sh`, then register the patched binary with binfmt_misc. |
| macOS on Apple silicon | **tested** (M2 MacBook Air, macOS 27.0) | The steamac libkrun guest (Valve's ARM64 SteamOS) with the patched FEX swapped into Steam's FEX compatibility tool; EAC `301`, Session Begin and world entry in three sessions. See [macos-port.md](macos-port.md). Other Macs untested. |

If you try one of these, `tests/ptrace-inject` is the quickest check that the emulation behaves; please report results.
