# eac-arm64-emulation-poc

**A proof of concept: running VRChat with genuine Easy Anti-Cheat on an arm64 Linux machine through FEX-Emu — and
everything it took to get there.**

This is a standalone repository. It records the investigation and the changes to **FEX-Emu** (2610; developed on 2609.1) that were needed to
bring up one x86-64 game with Epic's Easy Anti-Cheat for Linux/Proton on an arm64 host: the changes are kept as patch
files you can apply yourself, with the tests that show what each one fixes and the diagnostic tools that found the
problems.

![VRChat running on an Apple M2 under Fedora Asahi Remix through the patched FEX-Emu, beside a `fastfetch` of the host](docs/img/vrchat-m2.png)

It is deliberately **one game on one machine** (VRChat on an Apple M2 under Fedora Asahi Remix, inside a [muvm](https://github.com/AsahiLinux/muvm)
micro-VM). Treat it as a starting point. **Fork it and take it further** — another game, another SoC, or turning the
signal fixes into something upstreamable. Nothing here is submitted to, or a proposal for, the FEX or Proton projects;
see [Not a contribution](#not-a-contribution).

What made it hard was not CPU speed but **fidelity**. The anti-cheat launcher injects its client into the game with
`ptrace`, and the emulator has to behave like a real Linux kernel for `ptrace`, signals and `/proc` in ways nothing else
had asked of it; later, VRChat's own network threads exposed two more places where FEX's signal handling differs from
Linux.

## What it takes

| you need | why |
|---|---|
| **FEX-Emu 2610 with the patches in `patches/`** | see [docs/patches.md](docs/patches.md); build with `scripts/build-fex.sh` |
| **an arm64 Linux host** (tested: Apple M2, Fedora Asahi Remix, muvm) | a host with 16 KB pages needs a guest with 4 KB pages, hence the muvm VM; on a 4 KB-page host FEX runs directly (untested) |
| **Proton Experimental 11.0** and the *Proton EasyAntiCheat Runtime*, Steam on the same side of the VM boundary as FEX | the versions everything here was measured with |
| host memory tuning (`scripts/host-tune.sh`, Asahi, 16 GB) | with 16 GB the host runs out of memory around an 8 GB VM plus the GPU buffers; zswap/zram/watermark settings keep the OOM killer away |
| the launch options and DXVK caps from `scripts/` | `scripts/set-launch-options.sh`, `scripts/dxvk.conf` |

The series is split into **fixes** and **diagnostics** — only patch 0001 (`ptrace`) and patch 0007 (signal mask) are needed to
run; 0008/0009 are correctness fixes in the same area, the rest is performance or tooling. [docs/patches.md](docs/patches.md) explains each patch and its reasoning:

| patch | group | purpose |
|---|---|---|
| 0001 `ptrace` emulation | fix | the EAC launcher injects its client with `ptrace`; FEX emulates the x86-64 view of a tracee. Without it the launcher fails (`Unexpected error (#1)`) |
| 0007 signal mask | fix | guest handlers run with the Linux signal mask. Without it, Photon time-outs in ~every second session |
| 0008 syscall info | fix | a handler entered from a syscall keeps the registers it sets |
| 0009 sigsuspend mask | correctness fix | `rt_sigsuspend` no longer host-blocks FEX's own signals; regression-free, no observed effect on the game |
| 0004 cheap invalidation | performance | `mmap`/`mprotect` on data pages ~22x cheaper with ~200 threads; without it, 60+ s joins |
| 0002 SMC hot pages | performance (unproven) | pages that keep self-modifying-code faulting stop being write-protected |
| 0003/0005/0006/0010 | **diagnostics** | stats, sampler and thread snapshot (the last also records the futex word) — tools, not fixes |

## What is changed where

Three different kinds of change are involved. Only the first is new code; nothing in Wine or Proton is patched.

| layer | what | kind | where |
|---|---|---|---|
| **FEX-Emu (emulator)** | 0001 `ptrace` emulation, 0007 signal mask, 0008 syscall info, 0009 sigsuspend mask, 0004 invalidation, 0002 SMC hot pages, 0003/0005/0006/0010 diagnostics | **new source code**, patches against FEX-2610 | `patches/`, built by `scripts/build-fex.sh` |
| **Proton / Wine** | `WINE_CPU_TOPOLOGY=16:…` (report 16 CPUs; fixes the pre-join stall) | **stock Proton setting**, no patch | launch options, `scripts/set-launch-options.sh` |
| | `EAC_LAUNCHERDIR`, `PROTON_EAC_RUNTIME` (load the EAC runtime), `WINEDEBUG=-all`, `PROTON_USE_XALIA=0` | stock Proton settings | launch options |
| | `DXVK_CONFIG_FILE` → `dxgi.maxDeviceMemory`/`maxSharedMemory` = 3072 | stock DXVK setting | `scripts/dxvk.conf` |
| **VM and host** | guest NIC MTU clamped to 1500; ≥ 8 GB guest RAM; muvm 4 KB-page guest | environment | `scripts/vm/steam-vm.sh`, `scripts/vm/guest-setup.sh` |
| | zswap/zram/watermark/readahead tuning on the 16 GB host | environment | `scripts/host-tune.sh` |
| | optional SMBIOS/DMI/PCI/hostname realism layer | environment, optional | `scripts/vm/realism.sh` |

## What was involved

1. **The EAC launcher** needed an x86-64 `ptrace` view of its tracee, `/proc/<pid>/exe` of the guest binary and
   signal-delivery stops for guest `int3`; the whole conversation is reproduced by a freestanding test
   (`tests/ptrace-inject`, [docs/how-it-works.md](docs/how-it-works.md)).
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

```sh
sudo dnf install steam muvm fex-emu && steam          # base install, log in, install VRChat, launch it once
scripts/build-fex.sh                                  # FEX 2610 with patches/*.patch applied
scripts/vm/install-overlay.sh
tests/ptrace-inject/build.sh clang && tests/ptrace-inject/run-in-vm.sh   # must print RESULT: PASS
tests/signal-mask/build.sh clang                      # run the binary in the VM too; passes (fails 1, 3, 4b with FEX_SIGNALMASKFIX=0)
tests/signal-regs/build.sh clang                      # run the binary in the VM too; passes with patch 0008
sudo scripts/host-tune.sh                             # memory tuning (optional, strongly recommended on 16 GB)
scripts/set-launch-options.sh                         # with Steam closed
REALISM=1 VM_MEM_MB=10240 VM_VRAM_MB=3072 scripts/vm/steam-vm.sh   # start Steam in the VM (verified config), then launch VRChat
```

Full A-to-Z guide, the exact verified versions and what breaks a working setup: [docs/setup-asahi.md](docs/setup-asahi.md). Tools: [docs/tools.md](docs/tools.md). What works and what is
open: [docs/status.md](docs/status.md).

## What this is not

* **Not an anti-cheat bypass.** Nothing fakes, replays or short-circuits EAC's result; no Epic or VRChat binaries,
  keys or traces are included. The patches only make FEX behave like a real Linux kernel for `ptrace`, signals and
  `/proc/<pid>/exe`, and the EAC client that runs is the stock, unmodified one from Valve's Proton EAC Runtime.
* **One caveat, stated plainly.** The optional `scripts/vm/realism.sh` does **not** touch EAC, but it is not pure
  emulation-fidelity either: it presents plausible SMBIOS/DMI data to the guest so it looks less like a microVM (and, a
  step beyond VRChat's guide, a PCI device list and hostname). That is *environment adaptation* following VRChat's own
  published guide, "Using VRChat in a Virtual Machine", which says EAC's VM block is its CPUID hypervisor-vendor check
  and that **"You can get virtualization working alongside EAC in some cases, and we don't mind if you do this."** Read
  that as a policy statement, not a licence: it is tolerated-but-unsupported, and this repository marks the script
  **optional and unverified as necessary**. It is not required to reproduce the emulation work, and it does not modify
  the game or the anti-cheat.
* **Not supported by VRChat.** VRChat does not support virtual machines or emulation; Epic/VRChat may change anything
  at any time and enforcement is their call. Use at your own risk, and never run modified clients. Never attach a
  debugger or `perf` to the running game: EAC reports it.

## Not a contribution

The changes are kept here as patch files so the setup can be reproduced on one machine. They are **not offered to the
FEX project or to Proton**, and this repository is not affiliated with either. If any part is worth upstreaming, that
is for someone to do from a fork, with their own testing and reasoning.

## Other EAC games and FEX hosts

The Proton EAC runtime is the same launcher/client machinery for every game that ships Linux EAC, so the `ptrace` work
is not VRChat-specific; each game still has its own EAC build, integrity checks and server-side policy. Only VRChat has
been tried. Other FEX hosts (for instance Valve's Steam Frame) are untested — see [docs/other-platforms.md](docs/other-platforms.md).

## License

MIT (see [LICENSE](LICENSE)); the patches modify FEX-Emu, also MIT.
