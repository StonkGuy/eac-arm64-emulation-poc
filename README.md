# eac-arm64-emulation-poc

**A proof of concept: running VRChat with genuine Easy Anti-Cheat on an arm64 Linux machine through FEX-Emu — and
everything it took to get there.**

This is a standalone repository. It records the investigation and the changes to **FEX-Emu** (2610; developed on 2609.1) that were needed to
bring up one x86-64 game with Epic's Easy Anti-Cheat for Linux/Proton on an arm64 host: the changes are kept as patch
files you can apply yourself, with the tests that show what each one fixes and the diagnostic tools that found the
problems.

![VRChat running on an Apple M2 under Fedora Asahi Remix through the patched FEX-Emu, beside a `fastfetch` of the host](docs/img/vrchat-m2.png)

It is deliberately **one game on one SoC** (VRChat on an Apple M2: developed under Fedora Asahi Remix inside a [muvm](https://github.com/AsahiLinux/muvm)
micro-VM, and also verified on macOS, see below). Treat it as a starting point and take it further — another game, another SoC, another
host — but please report bugs you find here rather than only fixing them in a fork; see
[Bugs, fixes and upstreaming](#bugs-fixes-and-upstreaming).

**macOS.** The same patched FEX also runs VRChat with genuine EAC on an Apple M2 MacBook Air under macOS 27, inside
the [steamac](https://github.com/fxgl/steamac) libkrun guest (Valve's ARM64 SteamOS, Steam's own FEX compatibility tool
swapped for the patched build). Result, timings and limits: [docs/macos-port.md](docs/macos-port.md). Setup and
the downstream fixes live in a separate fork, [StonkGuy/steamac](https://github.com/StonkGuy/steamac) (branch
`fex-eac`).

**Steam Frame.** Got here from a post about the Steam Frame? Start with [docs/steam-frame.md](docs/steam-frame.md): get a
shell, build the patched FEX on the Frame with [`scripts/steamos/`](scripts/steamos), swap it into Steam's FEX tool,
set the launch options. Reported working by another user; not tested here.

What made it hard was not CPU speed but **fidelity**. The anti-cheat launcher injects its client into the game with
`ptrace`, and the emulator has to behave like a real Linux kernel for `ptrace`, signals and `/proc` in ways nothing else
had asked of it; later, VRChat's own network threads exposed two more places where FEX's signal handling differs from
Linux.

## What it takes

| you need | why |
|---|---|
| **FEX-Emu 2610 with the patches: [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) tag `eac-2610.2`** | see [docs/patches.md](docs/patches.md); build with `scripts/build-fex.sh` |
| **an arm64 Linux host** (tested: Apple M2 with Fedora Asahi Remix + muvm, or macOS 27 with a steamac libkrun guest) | a host with 16 KB pages needs a guest with 4 KB pages, hence the muvm VM; on a 4 KB-page host FEX runs directly (untested) |
| **Proton Experimental 11.0** and the *Proton EasyAntiCheat Runtime*, Steam on the same side of the VM boundary as FEX | the versions everything here was measured with |
| host memory tuning (`scripts/host-tune.sh`, Asahi, 16 GB) | with 16 GB the host runs out of memory around an 8 GB VM plus the GPU buffers; zswap/zram/watermark settings keep the OOM killer away |
| the launch options and DXVK caps from `scripts/` | `scripts/set-launch-options.sh`, `scripts/dxvk.conf` |

The series is split into **fixes** and **diagnostics** — patches 0001 (`ptrace`), 0002 (SMC hot pages) and 0007 (signal mask) are
needed to run; 0008 and 0010 are correctness fixes in the same area, the rest is performance, kernel fidelity or tooling. [docs/patches.md](docs/patches.md) explains each patch and its reasoning:

| patch | group | purpose |
|---|---|---|
| 0001 `ptrace` emulation | fix | the EAC launcher injects its client with `ptrace`; FEX emulates the x86-64 view of a tracee. Without it the launcher fails (`Unexpected error. (#1)`) |
| 0007 signal mask | fix | guest handlers run with the Linux signal mask. Without it, Photon time-outs in ~every second session |
| 0008 syscall info | fix | a handler entered from a syscall keeps the registers it sets |
| 0010 seccomp trap | fix | a `SECCOMP_RET_TRAP` `SIGSYS` reports the instruction after the syscall, as Linux does; without it Wine's syscall-trapping filter re-executes the trapped syscall forever |
| 0004 cheap invalidation | performance | `mmap`/`mprotect` on data pages ~22x cheaper with ~200 threads; without it, 60+ s joins |
| 0002 SMC hot pages | fix | pages that keep self-modifying-code faulting stop being write-protected. Without it the anti-cheat client never finishes loading and the game does not start |
| 0003/0005/0006/0009 | **diagnostics** | stats, sampler and thread snapshot (0009 adds the futex word to the snapshot) — tools, not fixes |
| 0011–0020 kernel fidelity | fidelity | the answers a real kernel gives on `arch_prctl` (0011), the debug registers (0012), `/proc/<pid>/status` (0013), unknown regsets (0014), `restart_syscall` (0015) and the signal frame (0016–0020). Not on VRChat's path; a title or Wine observes them |
| 0022–0026 second wave | fidelity | host-stack leak per seccomp trap (0022), guest reads of FEX's call/ret shadow-stack guard pages (0023), TF trap on the IRETQ target (0025), the signal alt stack while a thread exits on it (0026); 0024 is a profiler option. Found on a second title (THE FINALS) whose anti-tamper bootstrapper observes them; trap-, fault- and exit-path only |
| 0033 `/proc/<pid>/maps` | fix | the maps file lists only the emulated process's mappings (below 2^47), not FEX and its AArch64 host libraries. Without it an EAC launcher module that injects into a library it picks from the child's maps chose a host library and failed with `210 Unexpected error. (#1)` (seen on Mini Royale via Proton; VRChat's deployment still serves an older module that is not affected yet) |
| 0035 SMC, remote writes | fix | a code page the guest makes writable (`mprotect` RX to RWX) stays writable for other processes, so a hook written with `NtWriteVirtualMemory` no longer fails with `STATUS_PARTIAL_COPY`. Based on yungtry's patch 0028; consumer The Division 2 (per its author); only with `SMCChecks=mtrack` |

## What is changed where

Three different kinds of change are involved. Only the first is new code; nothing in Wine or Proton is patched.

| layer | what | kind | where |
|---|---|---|---|
| **FEX-Emu (emulator)** | 0001 `ptrace` emulation, 0007/0008/0010 signal and seccomp fidelity, 0004 invalidation, 0002 SMC hot pages, 0011–0020 kernel-fidelity gaps (debug registers, `/proc`, `arch_prctl`, regsets, `restart_syscall`, signal frame), 0022/0023/0025/0026 host-stack, fault and single-step fidelity, 0033 `/proc/<pid>/maps`, 0035 SMC remote writes, 0024 profiler option, 0003/0005/0006/0009 diagnostics | **new source code**, one commit per patch on FEX-2610 | [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) branch `eac-2610` (tests in `eac-tests/`), built by `scripts/build-fex.sh`; `patches/` is a generated snapshot |
| **Proton / Wine** | `WINE_CPU_TOPOLOGY=16:…` (report 16 CPUs; fixes the pre-join stall) | **stock Proton setting**, no patch | launch options, `scripts/set-launch-options.sh` |
| | `EAC_LAUNCHERDIR`, `PROTON_EAC_RUNTIME` (load the EAC runtime), `WINEDEBUG=-all`, `PROTON_USE_XALIA=0` | stock Proton settings | launch options |
| | `DXVK_CONFIG_FILE` → `dxgi.maxDeviceMemory`/`maxSharedMemory` = 3072 | stock DXVK setting | `scripts/dxvk.conf` |
| **VM and host** | guest NIC MTU clamped to 1500; ≥ 8 GB guest RAM; muvm 4 KB-page guest | environment | `scripts/vm/steam-vm.sh`, `scripts/vm/guest-setup.sh` |
| | zswap/zram/watermark/readahead tuning on the 16 GB host | environment | `scripts/host-tune.sh` |
| | optional SMBIOS/DMI/PCI/hostname realism layer | environment, optional | `scripts/vm/realism.sh` |

## What was involved

1. **The EAC launcher** needed an x86-64 `ptrace` view of its tracee, `/proc/<pid>/exe` of the guest binary and
   signal-delivery stops for guest `int3`; the whole conversation is reproduced by a freestanding test
   (`eac-tests/ptrace-inject`, [docs/how-it-works.md](docs/how-it-works.md)).
2. **Joins that took a minute** were code-invalidation cost in a process with ~200 threads (patch 0004, found with
   the stats of patch 0003).
3. **Host out-of-memory kills** of the whole VM: memory tuning and VM sizing ([docs/tuning.md](docs/tuning.md)).
4. **Photon time-outs** (about every second session) were a lost wake-up in Wine's wait protocol: FEX ran guest signal
   handlers with the wrong signal mask (patch 0007). Finding it took a thread-snapshot facility that does not
   `ptrace` the anti-cheat, a stand-alone Wine reproducer, and a look at wineserver's epoll registrations
   ([docs/disconnects.md](docs/disconnects.md)). A second FEX bug of the same family (registers lost inside signal
   handlers, *patch 0008*) turned up while testing 0007.
5. **A ~60 s freeze at start-up** in some launches (the Photon region lookup timing out, then a `Disconnecting`
   rejoin loop) was the game's IL2CPP thread pool starving on a 4-vCPU VM. Not a FEX or Wine bug: reporting 16 CPUs
   with Proton's `WINE_CPU_TOPOLOGY` removes it. Found with in-process thread snapshots and a passive reader of wineserver's
   socket state ([docs/disconnects.md](docs/disconnects.md)).

## Quick start (the tested platform: Fedora Asahi Remix)

On a Steam Frame (or another SteamOS arm64 device), follow [docs/steam-frame.md](docs/steam-frame.md) instead.

```sh
sudo dnf install steam muvm fex-emu && steam          # base install, log in, install VRChat, launch it once
scripts/build-fex.sh                                  # clones and builds StonkGuy/FEX tag eac-2610.2 into work/FEX
scripts/vm/install-overlay.sh
(cd work/FEX/eac-tests/ptrace-inject && ./build.sh clang)
scripts/vm/run-test-in-vm.sh work/FEX/eac-tests/ptrace-inject/ptrace_inject_test   # must print RESULT: PASS
scripts/vm/run-test-in-vm.sh <other eac-tests binary>  # e.g. signal-mask, signal-regs: RESULT: PASS
sudo scripts/host-tune.sh                             # memory tuning (optional, strongly recommended on 16 GB)
scripts/set-launch-options.sh                         # with Steam closed
REALISM=1 VM_MEM_MB=10240 VM_VRAM_MB=3072 scripts/vm/steam-vm.sh   # start Steam in the VM (verified config; REALISM=1 is optional), then launch VRChat
```

Full A-to-Z guide, the exact verified versions and what breaks a working setup: [docs/setup-asahi.md](docs/setup-asahi.md). Tools: [docs/tools.md](docs/tools.md). What works and what is
open: [docs/status.md](docs/status.md).

## Scope and limits

* **Not an anti-cheat bypass.** Nothing fakes, replays or short-circuits EAC's result; no Epic or VRChat binaries,
  keys or traces are included. The patches only make FEX behave like a real Linux kernel for `ptrace`, signals and
  `/proc/<pid>/exe`, and the EAC client that runs is the stock, unmodified one from Valve's Proton EAC Runtime.
* **One caveat, stated plainly.** The optional `scripts/vm/realism.sh` does **not** touch EAC, but it is not pure
  emulation-fidelity either: it presents plausible SMBIOS/DMI data to the guest so it looks less like a microVM (and, a
  step beyond VRChat's guide, a PCI device list and hostname). That is *environment adaptation* following VRChat's own
  published guide, "Using VRChat in a Virtual Machine", which says EAC's VM block is its CPUID hypervisor-vendor check
  and that **"You can get virtualization working alongside EAC in some cases, and we don't mind if you do this."** Read
  that as a policy statement, not a licence: it is tolerated-but-unsupported, and this repository marks the script
  **optional**: VRChat runs without it. It may be useful for other games whose anti-cheat inspects the machine's
  hardware identity. It does not modify the game or the anti-cheat.
* **Not supported by VRChat.** VRChat does not support virtual machines or emulation; Epic/VRChat may change anything
  at any time and enforcement is their call. Use at your own risk, and never run modified clients. Never attach a
  debugger or `perf` to the running game: EAC reports it.

## Bugs, fixes and upstreaming

**Found a bug, or a place where the patched FEX still behaves differently from x86-64 Linux?** Please report it here as
an issue. If you have already fixed it in your own fork, please consider opening a pull request here too (with a test),
so everyone gets the fix and the test suite covers it. FEX code changes go to [StonkGuy/FEX](https://github.com/StonkGuy/FEX) as a pull request against the current `eac-<release>` branch, with a test in its `eac-tests/`.

This repository does not send patches to FEX-Emu or Proton itself and is not affiliated with them (steamac changes are
discussed with its maintainer directly). If you have reviewed a patch and think it is good enough for upstream, **you
are welcome to submit it there yourself**: keep its test with it, follow the upstream project's own process, and
mention where it came from. The same applies to the other patched components (libkrun, Mesa/KosmicKrisp,
virglrenderer, DXMT), unless that project's own rules say otherwise — DXMT, for example, does not accept AI-assisted
contributions.

## Maintenance and support

I will try to keep the patched components up to date with their upstream releases, re-running the regression tests on
every update, and to drop any patch once upstream covers its behaviour. For game-specific problems I will try to help
where I can, but I cannot guarantee that every game, or every fix, will work.

Each FEX release is handled the same way: rebase the patches onto it as branch `eac-<release>` of StonkGuy/FEX, run
`eac-tests` in the guest and natively for the oracle, check a live VRChat session, drop every patch whose behaviour
upstream now provides, and tag the result; this repository and the steamac fork then pin the new tag.

## Other EAC games and FEX hosts

The Proton EAC runtime is the same launcher/client machinery for every game that ships Linux EAC, so the `ptrace` work
is not VRChat-specific; each game still has its own EAC build, integrity checks and server-side policy. VRChat is
verified end to end; Mini Royale's EAC launcher also reaches `301` with 0033 (the game itself was not played past its
account prompt). Results differ from game to game: each title ships its own EAC build and checks, so an untested game
may or may not work. Besides Asahi and the macOS guest above, other FEX hosts are untested, with the exception of the Steam Frame (findings from another user, @VirtuallyNoxide; setup in [docs/steam-frame.md](docs/steam-frame.md)) — see [docs/other-platforms.md](docs/other-platforms.md).

## License

MIT (see [LICENSE](LICENSE)); the patches modify FEX-Emu, also MIT.
