# Tools

Everything in `tools/` is plain Python 3 with no dependencies beyond the standard library, except `canary.c` (C) and the
X11 helpers (`xin.py`, and `hudfps.py` which drives it), which need libX11/libXtst. They were written for the
measurements in this repository; most carry a usage line in the header comment or docstring.

## Game measurements (run where the game's X display is, i.e. inside the VM)

| tool | what it does |
|---|---|
| `hudfps.py [seconds] [interval]` | Reads the DXVK HUD's `FPS: nn.n` from the game window with XGetImage and prints `epoch fps`; uses `xin.py` and the digit templates in `hud_templates.json` (set `DXVK_HUD=fps` in the launch options). |
| `xin.py` | Tiny XTEST input injector and screen grabber (ctypes over libX11/libXtst): `list`, `activate SUBSTR`, `key`, `click`, `drag`, `grab`, ... Used to drive the game menu unattended. |
| `fexstats.py FILE…` | Parses the live statistics FEX publishes with `FEX_PROFILESTATS=1` (per-thread JIT/invalidation/SMC counters). |
| `resolve_samples.py TAG FEX_UNSTRIPPED [NM]` | Resolves the output of FEX's in-process sampling profiler (patch 0005) to host symbols and guest modules; see [how-it-works.md](how-it-works.md). |
| `mdparse.py` | Summarises a Windows minidump (exception record, faulting module+offset, registers, stack module hits), for crash dumps from the Unity crash handler. |

## Host power / throttle (run on the host, Asahi)

These make the *host* the thing under measurement. They exist to check the claim in [status.md](status.md) that the
frame-rate dips are a host power-budget throttle, and to test for it on your own machine.

| tool | what it does |
|---|---|
| `canary.c` | A fixed-work CPU probe: a dependent integer chain timed every 0.5 s. Run it pinned to a performance core next to the game (`taskset -c 6 ./canary 120`) and its printed nanoseconds-per-run show directly when the core slows down. Build: `gcc -O1 -o canary canary.c`. |
| `powertl.py SECONDS OUTFILE` | Logs the Apple SMC's power/voltage/current/temperature readings twice a second on Asahi (`macsmc_hwmon` + battery). |
| `check-power.py [SECONDS]` | Loads every performance core for ~40 s and prints a verdict: whether the power source (charger/hub/port) is strong enough, and whether the SoC throttles to the adapter limit under load. |


## Wedge and disconnect analysis (`tools/harness/`)

| tool | what it does |
|---|---|
| `timeline.py SECONDS OUTFILE` | Inside the VM: every 0.5 s per-thread CPU ticks and state of the game, UDP receive queues and system counters. |
| `hosttl.py SECONDS OUTFILE` | On the host: memory, reclaim and pressure counters and the CPU time of the VM's threads. |
| `analyze_tl.py LABEL` | Correlates the HUD FPS with per-thread CPU, pressure counters and the UDP backlog from the recordings above. |
| `wedgewatch.py LABEL SECONDS` | Inside the VM as root: watches a running game for a wedge (UDP backlog > 64 KB for 6 s, or the main thread asleep for 25 s) and at that moment triggers a FEX thread snapshot (patch 0006) and copies the snapshot, `maps`, thread names, per-thread syscalls, wineserver's fd table and epoll state and the TCP/UDP socket state. Needs `FEX_PROFILESAMPLEHZ=1` in the game's environment; with `FEX_SIGNALTRACE=1` it also gzips the FEX trace rings. |
| `resolve_snap.py SNAP MAPS [NAMES]` | Turns a snapshot into a per-thread listing: the x86 syscall each thread is blocked in with its arguments (for futex waits also the futex word against the expected value), and the return addresses on its stack as `module!export+offset`. Auto-detects the 640- or 1280-byte record size (patch 0006 writes 80 words; patch 0009 adds the futex-word diagnostics and writes 160). |
| `sigtrace.py GAME_RING [SERVER_RING]` | Decodes `FEX_SIGNALTRACE=1` rings and follows a signal from the sender's `kill`/`tgkill` to the receiver's guest handler. |
| `wakeflow.py RING [--tid T] [--fd F]` | Decodes a `FEX_SIGNALTRACE=1` ring's 16-byte wake-up traffic (`Read16`/`Write16`, the first 8 bytes are Wine's wait cookie) and `epoll_ctl` calls, and flags a 16-byte write whose cookie is never read back — a reply the server wrote that the waiter did not consume. |
| `classify_runs.py PREFIX…` | Classifies saved game logs (time-out, hang, ok, short) per session and arm and runs Fisher's exact test between arms. |
| `wedge_repro.py` | Standalone Windows-Python program that recreates the wedge without the game (see [disconnects.md](disconnects.md)). |
| `stallwatch.py LABEL SECONDS LOGFILE [TRIG] [TRIG2] [BASELINE]` | Inside the VM as root: watches the game's Unity log and, when the Photon region lookup (`Locating best region` → `Got best network region`) has not finished after `TRIG` (12) seconds — healthy launches take 2–10 s — takes `wedgewatch.py`'s full passive capture at `TRIG` and again at `TRIG2` (40) seconds; `BASELINE=1` also captures a healthy session 20 s after world entry. Needs `FEX_PROFILESAMPLEHZ=1`. |
| `poolmon.sh OUTFILE [SECONDS]` | Inside the VM as root: once a second, the number of `IL2CPP Threadpool worker` threads and all threads of `VRChat.exe` (thread names from `/proc` only). A starving pool grows by ~2 threads a second. |
| `wsmem.py [OUTFILE]` | Inside the VM as root: reads wineserver's own per-socket state (`mask`, `pending_events`, `reported_events`, event object, `WSAAsyncSelect` window, queued receives/polls) from `/proc/<wineserver>/mem` and joins it with `ss`. wineserver is a separate process from the game; nothing is attached. Layout of Proton `experimental-11.0-20261001`, every row self-checked. |

## Building the tests

`static-link.py` is a minimal linker for one freestanding x86-64 object. It exists so that the programs in `tests/` can be built
on an aarch64 machine that has a clang with the x86-64 target but no x86-64 linker.

| test | what it is |
|---|---|
| `tests/ptrace-inject/build.sh [clang]` + `run-in-vm.sh` | The faithful-`ptrace` check (28 checks). `run-in-vm.sh` boots a throw-away VM with the patched FEX registered for x86-64; `run.sh` runs the binary with a 60 s timeout where x86-64 binaries already execute (e.g. inside an already-running VM: `muvm -- tests/ptrace-inject/run.sh`). Must print `RESULT: PASS`; under stock FEX it hangs after `PASS: fork`. |
| `tests/signal-mask/build.sh [clang]` | Checks the Linux signal-mask rule around handlers (patches 0007/0008). Run it in the VM too. |
| `tests/signal-regs/build.sh [clang]` | Checks that a handler's register changes survive across a syscall (patch 0008). |
| `tests/seccomp-trap/build.sh [clang]` | Reproduces the seccomp/SIGSYS path Wine's ntdll installs on x86_64 (`install_bpf`/`sigsys_handler`): a `SECCOMP_RET_TRAP` filter that traps raw `syscall`s from a chosen code range, then checks the SIGSYS fields (`si_code`/`si_syscall`/`si_arch`/`si_call_addr`), that the handler can read the number and all six arguments from the ucontext, and that a value written into its RAX resumes the caller, over 1000 repetitions. Fails early with the errno if the filter install is rejected (EINVAL under FEX). Also `build.sh native` for the reference run on a real Linux kernel. Run it in the VM too. |
| `tests/cpuid-fault/build.sh [clang]` | Checks `arch_prctl(ARCH_SET_CPUID/ARCH_GET_CPUID)` (patch 0011): `SET_CPUID(0)` → 0, a later CPUID faults as SIGSEGV/SI_KERNEL at the CPUID instruction, `SET_CPUID(1)` restores it. On a real kernel or an unpatched FEX the faulting checks report SKIP (`-ENODEV`). Re-execs itself with `FEX_ENABLECPUIDFAULTING=1`, the **patch-0011** option. Also `build.sh native`. |
| `tests/proc-status/build.sh [clang]` | Checks the two debugger-facing lines of `/proc/self/status` (patch 0013): `Seccomp:` reflects the guest's own filter (2 after a filter is installed) and `TracerPid:` is 0 for an untraced process; all other lines stay the host file's. Re-execs itself with `FEX_NEEDSSECCOMP=1`, the stock option under which FEX emulates the guest's filters (the setting Wine's seccomp path needs); without it the host process carries the filter and an unpatched FEX happens to pass. Also `build.sh native`. |
| `tests/ptrace-dregs/build.sh [clang]` | Checks the hardware debug registers through `PTRACE_PEEKUSER`/`POKEUSER` (patch 0012): DR0–DR3 validate a canonical address (`-EINVAL` otherwise), DR4/DR5 are `-EIO`, DR7 is validated and stored raw, a slot-unaligned or past-`struct user` offset is `-EIO`. The patch and this test were rewritten to the kernel's layout; the test passes natively and under FEX. |
| `tests/ptrace-regsets/build.sh [clang]` | Checks `PTRACE_GETREGSET`/`SETREGSET` type handling (patch 0014): `NT_PRSTATUS` and `NT_X86_XSTATE` succeed, an unknown type returns `-EINVAL` (never `-EPERM`). Also `build.sh native`. |
| `tests/restart-syscall/build.sh [clang]` | Checks that `restart_syscall` returns `-EINTR` and the process survives (patch 0015); on an unpatched FEX the process dies and no RESULT line prints. Also `build.sh native`. |
| `tests/signal-frame/build.sh [clang]` | Checks the guest signal frame against what Linux writes (patches 0016–0020): single-step via TF and a handler clearing it, the frame's CS/SS (`0x33`/`0x2b`), the XSAVE fpstate (`FP_XSTATE_MAGIC1` in `sw_reserved`, `FP_XSTATE_MAGIC2` at `fpstate + xstate_size`), `ud2` → `ILL_ILLOPN`, `uc_sigmask` on delivery and after a handler rewrite, and a signal on a stack that cannot hold the frame (must die from SIGSEGV). Also `build.sh native`. |
| `tests/sigreturn-eflags/build.sh [clang]` | Checks that `rt_sigreturn` applies EFLAGS a handler wrote while leaving RIP alone (patch 0016): a handler that sets DF resumes with DF set, one that sets TF starts single-stepping with exactly one SIGTRAP (how a debugger or Wine's `SetThreadContext` starts stepping). x86-64 only; run the same binary natively for the reference. |
| `tests/maps-probe/build.sh [clang]` | Reads `/proc/self/maps` and probes every inaccessible (`---p`) mapping under a SIGSEGV handler, the way an anti-tamper scanner walks its address space: every probe must come back (fault delivered, or read allowed) and a faulting probe must report `si_addr` = the probed address. Under FEX the guest's maps also list FEX's own mappings, including the call/ret shadow-stack guard pages (patch 0023). A hang ends in `alarm()` with no RESULT line. x86-64 only; run the same binary natively for the reference. |
| `tests/tf-iret/build.sh [clang]` | Checks where the single-step trap lands when TF is switched on (patch 0025): `POPFQ` (after the next instruction), `IRETQ` loading TF (after the first target instruction), Wine's syscall return `popfq; iretq` (on the IRETQ target itself), and single-stepping across a call and a return into code that has already run (one trap per instruction). 2000 repetitions each, so the translated code is cached and linked. x86-64 only; run the same binary natively for the reference. |
| `tests/pop-fault/build.sh [clang]` | Checks that a faulting `pop` to memory leaves RSP unchanged (`popq (%rax)` into an unwritable page, differential against `mov %eax,(%rax)`, the handler resuming past the instruction the way `ntdll:exception` does). x86-64 only. **Passes only with the withdrawn patch 0021 ([patches.md](patches.md#f-signal-frames-and-faults--patches-00162020)), so it fails on the shipped series |
| `tests/seccomp-trap-noexec/build.sh [clang]` | Checks that a syscall a `SECCOMP_RET_TRAP` filter traps is never executed: a trapped `uname()` must leave its buffer untouched, an untrapped one fills it; 50 consecutive trapped calls stay suppressed. Run with `FEX_NEEDSSECCOMP=1`. Also `build.sh native`. |
| `tests/signal-race/build.sh [clang]` | Two threads: one loops a trapped raw syscall (every call is a SIGSYS), the other hammers it with SIGUSR1 — the shape of Wine's `NtGetContextThread` against a thread in raw NT syscalls. Fails if the worker cannot be stopped afterwards (the hang signature). Run with `FEX_NEEDSSECCOMP=1`. Also `build.sh native`. |
| `tests/bench/build.sh [clang] [output]` | Freestanding x86-64 micro-benchmarks: the code-churn invalidation cost (`EXTRA="-DINVAL_MODE=1 -DINVAL_THREADS=190"`, patch 0004) and the self-modifying-code fault modes (patch 0002). Generates its bytecode table with `gen_bigcode.py`. |
| `tests/bench/tso_probe.c` | A tiny probe for the `prctl(PR_GET/SET_MEM_MODEL)` interface FEX uses for TSO; prints what the kernel/FEX report. Unlike the other tests it is an ordinary aarch64 program: `gcc -O2 -o tso_probe tests/bench/tso_probe.c`, then run it on the host and inside the VM. |

## Native reference — expected values from a real kernel

An emulator is only wrong relative to something. Every test above is written to run on **bare-metal x86-64 Linux** as
well, so the expected values come from a real kernel rather than from FEX's own current behaviour.

```
tests/<name>/build.sh native        # builds for the host's own architecture with the system compiler
./<name>_test                       # in the test's directory; exit status = number of failed checks, 0 = PASS
```

A test that cannot be satisfied on real hardware is the test's bug, not FEX's, and is reported as such. The native run
sets the expected values for the behaviour a patch implements; it does not mean every difference from a real kernel is
a defect to fix (see the scope note in [patches.md](patches.md)). The reference
runs recorded here used an **AMD Ryzen 9 7940HS (Zen 4)**, Fedora 44, Linux 6.19, gcc 16.2.1/clang 22.1.8, in
`~/fex-ref/` on that box only (no sudo, no system packages, no Steam or games). That CPU has `cpuid_fault`, which is
what makes the native `cpuid-fault` reference pass at all; a host without it would report `-ENODEV` and SKIP.

| test | native x86-64 | stock FEX-2610 | patched FEX-2610 |
|---|---|---|---|
| `ptrace-inject` | PASS | hang (timeout) | PASS |
| `signal-mask` | PASS | FAIL | PASS |
| `signal-regs` | PASS | FAIL | PASS |
| `seccomp-trap` (`FEX_NEEDSSECCOMP=1`) | PASS | SIGSEGV | PASS |
| `seccomp-trap-noexec` (`FEX_NEEDSSECCOMP=1`) | PASS | SIGSEGV | PASS |
| `signal-race` (`FEX_NEEDSSECCOMP=1`) | PASS | FAIL | PASS |
| `cpuid-fault` | PASS | FAIL | PASS |
| `proc-status` | PASS | FAIL | PASS |
| `ptrace-regsets` | PASS | FAIL | PASS |
| `ptrace-dregs` | PASS | FAIL | PASS |
| `restart-syscall` | PASS | SIGILL | PASS |
| `signal-frame` | PASS | FAIL | PASS |
| `sigreturn-eflags` | PASS | FAIL | PASS |
| `pop-fault` | PASS | FAIL | PASS |
| `maps-probe` | PASS | hang (timeout; measured on the 0001–0020 build, same code as stock) | PASS |
| `tf-iret` | PASS | FAIL, checks 4–5 (measured on the 0001–0020 build, same code as stock) | PASS |

The three columns ran the **same binaries** (built once, copied to the x86-64 box). Every test fails without the patch
it names: the series was also built one patch at a time and each test flips from FAIL to PASS at its own patch.

All tests build with the x86-64-target clang on any host; most also build natively (`build.sh native`), while
`sigreturn-eflags` and `pop-fault` check x86 behaviour and are run natively as the same x86-64 binary. Two further notes for anyone extending the comparison:

* Wine's own conformance tests (`winetest64`, the daily build from WineHQ's job artifacts; the old
  `test.winehq.org/builds/` URL is gone) run natively under Xvfb and through FEX the same way, and several FEX-only
  divergences show up only against the native run (`ntdll:exception` aborts on FEX but completes natively — the faulting-`pop` case, whose patch was
  withdrawn; `kernel32:process` hangs intermittently under FEX, stock and patched alike, at a rate that tracks guest
  load). Run them the same way on both, and judge an intermittent test by its rate over many runs, never by one run.
* A native result can also **correct** a FEX report: the `tf`/single-step trap storm, the signal-frame CS/SS values
  (`0x33`/`0x2b` native vs `0x30`/`0x00` under FEX) and the zero back-to-back RDTSC delta are confirmed by hardware,
  an apparent `kernel32:process` regression turned out to be the test's own flakiness (same hang rate on stock FEX), while several claims that FEX was "wrong" about `int3`/`ud2` `si_code`
  turned out to be the reports' own expectations and FEX matches the kernel.
