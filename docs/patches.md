# The patch series

Sixty-one patches against **FEX-Emu 2610** (base commit `14c9268`). 0001–0010 were developed and measured on 2609.1
(`9fbdc00`) and rebase onto 2610 with no code changes; 0011–0020 were written against 2610; 0022–0026 on top of 0001–0020
(`0021` is withdrawn, see below); 0033 and 0035 were added on top of 0026, 0036–0039 and 0041–0053 (group H) in `eac-2610.3`, and 0054–0070 (group I) in `eac-2610.4`. They are not all the same kind of thing, so they are
grouped here by what they are for.

**Where the code lives.** Each patch is one commit on branch `eac-2610` of [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) (release tag
`eac-2610.4`), and every test named below is in that branch's
[`eac-tests/`](https://github.com/StonkGuy/FEX/tree/eac-2610/eac-tests). `patches/` here is a generated snapshot of
those commits; send changes to the fork.

The groups:

| group | patches | what it is |
|---|---|---|
| **A. Faithful `ptrace`** | 0001 | the change that lets the anti-cheat launcher inject its client at all |
| **B. Signal fidelity** | 0007, 0008, 0010 | guest signal handlers must behave like Linux; **0007** is what fixed the disconnects (0008 is a correctness fix in the same area, 0010 the seccomp/SIGSYS path) |
| **C. Code-invalidation cost and self-modifying code** | 0002, 0004, 0035 | 0002: without it the anti-cheat client never finishes loading; 0004: performance with ~200 threads; 0035: a page the guest makes writable stays writable for other processes (The Division 2) |
| **D. Diagnostics** | 0003, 0005, 0006, 0009 | **not fixes** — tools that made the bugs visible without ptrace |
| **E. Kernel-fidelity gaps** | 0011, 0012, 0013, 0014, 0015, 0033 | the answers a real kernel gives for `arch_prctl`, the debug registers, `/proc/<pid>/status`, unknown regsets, `restart_syscall` and `/proc/<pid>/maps` |
| **F. Signal frames and faults** | 0016–0020 | what Windows exception handling under Wine reads from and writes to a signal frame |
| **G. Second title** | 0022–0026 | what a second title's anti-tamper bootstrapper (THE FINALS) observes: host stack per SIGSYS trap, faults on FEX's own mappings, single-step after `iretq`, a thread exiting on its signal stack |
| **H. Fixes since `eac-2610.2`** | 0036–0039, 0041–0053 | trap, signal, exec and SMC paths that Wine 11, the EAC launcher or a test observed differently from Linux |

Patches 0001–0010 are needed for the game (0001 to inject at all, 0002 so the injected client finishes loading, 0007 to
avoid the disconnects, 0010 for the seccomp/SIGSYS path; 0008 is a correctness fix in the same area and 0004 is
performance). Group D is instrumentation you can drop. Groups E, F and G are not on VRChat's path — they exist because a
real kernel and FEX give different answers there, and a title or Wine itself observes the difference.

Everything is measured on **one game (VRChat) on one machine (Apple M2, Fedora Asahi Remix)**. Treat the series as a
starting point to fork, not as a supported port.

**Scope: a trade-off, not a replica of x86-64 Linux.** The series follows upstream FEX's own approach: emulate exactly
what real software depends on, and stay approximate where exactness would cost every program speed or complexity for no
consumer; its memory-ordering options (`FEX_TSOENABLED`, `FEX_HALFBARRIERTSOENABLED`) let a user make that trade-off
explicitly. A patch here makes a behaviour exact only when something real observes it — the anti-cheat launcher, Wine, a
title's own probes — and it must cost nothing on paths that do not use it. A real x86-64 kernel defines what *correct*
means for the behaviour a patch touches; it is not a target for everything else. Known differences that remain are
listed under each patch's **Limits** and are left there on purpose until something needs them.

---

## A. Faithful `ptrace` — patch 0001

**Reasoning.** The Proton EasyAntiCheat runtime does not decide the verdict in-process: a launcher
(`start_protected_game.exe`) `ptrace`s a child, single-steps it, reads and rewrites its x86-64 registers, pokes code into
its memory and injects the client shared object at the right moment. Under FEX, tracer and tracee are both *aarch64*
processes hosting FEX, so the host kernel would hand the tracer aarch64 registers, host-syscall stops and a `SIGTRAP`
that means something else. Stock FEX passes through a small subset of ptrace (enough for Ubisoft's Wine launcher) and
rejects the rest, so the launcher fails with `Unexpected error. (#1)` and no anti-cheat session ever starts.

**What it does.** `LinuxSyscalls/PtraceEmu.{h,cpp}` presents an x86-64 view of the tracee from inside FEX: syscall-entry
and syscall-exit stops with x86-64 numbers/registers, signal-delivery stops for guest `int3`/faults, the requests the
launcher uses (`GETREGS/SETREGS`, `GETREGSET/SETREGSET`, `PEEK*`/`POKE*` with code-cache invalidation, `SETOPTIONS`,
`GETSIGINFO`, `CONT`, `SYSCALL`, `DETACH`), and a `/proc/<pid>/exe` that names the guest binary. The full conversation
is in [how-it-works.md](how-it-works.md).

**Without it:** no anti-cheat session. **Verified by:** `eac-tests/ptrace-inject` (35 checks, the last six being a stale-exit-stop check after an entry stop resumed with `CONT` and a no-state-file check for a failing `PTRACE_TRACEME`; passes under FEX; the binary
also targets native x86-64 Linux as the reference, see [tools.md](tools.md)). **Limits** (all off the launcher's path,
none costs anything when unused): only the main thread of a tracee is traced; `PTRACE_ATTACH` to an already-running FEX
process is not emulated; `PTRACE_SINGLESTEP` and `PTRACE_SEIZE`/`INTERRUPT`/`LISTEN` are passed to the host and do not
give a guest-instruction stop; `PEEKUSER`/`POKEUSER` at an in-`struct user` offset with no register handler answer `0`
where a real kernel answers `-EIO` (patch 0012 fixes this for the debug-register offsets the launcher uses). Also not
emulated, because the launcher does not reach them: a tracee exec'd without `PTRACE_O_TRACEEXEC` (the launcher sets it),
the `TRACESYSGOOD` option (stops always carry `SIGTRAP|0x80`; the launcher sets it), `waitid` (only `wait4` is
rewritten), and a `clone(CLONE_VM)` child that is not a thread (`posix_spawn`, `vfork`) of a traced process, which
shares the parent's state block and can switch its stops off. A tracer that dies before its `wait4` saw the tracee exit
leaves the tracee's `/dev/shm/fex-ptrace-state-<pid>` behind until the pid is reused.

---

## B. Signal fidelity — patches 0007, 0008, 0010

This is the group that fixed the *disconnects*. The symptom was a lost wake-up in Wine's server protocol: a thread
suspended by `SIGUSR1` and a socket reader parked in the 16-byte wait-pipe read would never wake although data had
arrived, so Photon timed out 25–29 s after entering a world and VRChat looped "join → show world → timed out →
rejoin". Details and the measurements: [disconnects.md](disconnects.md).

### 0007 — run guest handlers with the Linux signal mask

**Reasoning.** On Linux a signal handler runs with `interrupted mask | sa_mask | signal` blocked, and `rt_sigreturn`
puts the interrupted mask back. Stock FEX blocked only `sa_mask | signal` on the host and never updated its
*guest-visible* mask (`SignalInfo.CurrentSignalMask`) at handler entry or return. Wine's `SIGUSR1` suspension handler
saves "the current mask", brackets a server call with a block/restore, and restores what it saved — so under stock FEX
it unblocked `SIGUSR1` while still inside the handler and let `SIGUSR1` nest. Wine's per-thread wait stack and its
"stolen reply" recovery are written for the nesting that Linux permits; a nested suspend mid-wait makes the 16-byte
completion reply be consumed by the wrong wait, and the thread reads forever.

**What it does.** `FEX_SIGNALMASKFIX` (patch 0007, **on by default**; `=0` restores stock behaviour) saves the
interrupted guest mask in the context backup on handler entry, applies the Linux rule to both the guest-visible and the
host mask (minus the signals FEX itself needs), restores the saved mask at `rt_sigreturn`, and re-raises signals that
were recorded pending and have become unmasked. `FEX_SIGNALTRACE=1` adds a lock-free in-memory trace of the signal
paths, decoded by `tools/harness/sigtrace.py`.

**Without it:** Photon time-outs in roughly every second session that reached a world, and a stand-alone Wine
reproducer that wedged 5–6 of 6 runs. **With it:** 0 of 7 real-game sessions timed out vs 4 of 7 control, and none of the 59 sessions run since; the reproducer
0 of 6 vs 6 of 6 (p = 0.002). **Verified by:** `eac-tests/signal-mask` (checks 1, 3, 4b fail with `FEX_SIGNALMASKFIX=0`).

### 0008 — do not leave `InSyscallInfo` set for a handler entered from a syscall

**Reasoning.** Found while testing 0007: a test that *should* have passed once 0007 was in did not. Every `tgkill`/`kill`/`tkill`
to self — and every signal that finds a thread parked in `futex` or `read` — is delivered while the FEX-internal C++
half of the `syscall` is still on the stack, where the JIT has left `Frame->InSyscallInfo == 0xFFFF` ("all guest
registers are already spilled to the frame"). FEX clears that marker only when the interrupted code was in the JIT, so
the handler ran with a stale marker; the SMC write-fault handler and a second signal in translated code trust it, spill
nothing, and redispatch from stale frame contents — the register changes the handler made are silently reverted.

**What it does.** Clears `InSyscallInfo` for the handler when the interrupted code was not in the JIT, and restores it at
`rt_sigreturn`. **Verified by:** `eac-tests/signal-regs` (raises the signal four ways; the `tgkill`/`kill`/`tkill` cases fail
on stock FEX). This is a plain correctness bug, *not* the cause of the disconnects (with 0008 but without 0007 the
reproducer still wedged 6 of 6). Analysis: [signal-registers.md](signal-registers.md).

### 0010 — report `SECCOMP_RET_TRAP` at the instruction after the syscall

**Reasoning.** Wine's ntdll cannot trap guest system calls itself, so on x86_64 it installs a seccomp filter that
returns `SECCOMP_RET_TRAP` for any syscall whose instruction pointer lies in a chosen code range, and handles the
resulting `SIGSYS` with `SA_SIGINFO`. Linux enters that handler with `RIP` and `si_call_addr` pointing just past the
`syscall` instruction (`kernel/seccomp.c` rolls the syscall back, then `kernel/signal.c` sets `si_call_addr` from the
rolled-back `regs->ip`), and `rt_sigreturn` resumes the guest there with the value the handler left in the return
register. FEX reconstructed the `RIP` at the address of the `syscall` itself — the dispatcher advances the guest `RIP`
only after the filter has run — so the `ucontext` handed to the handler equalled the interrupted `RIP`, `rt_sigreturn`
took its fall-through path (resetting the guest to the interrupted state and discarding the handler's register updates),
and the guest re-executed the trapped syscall. For an IP-trapping filter this repeats forever: each delivery queues
another signal, and the nested host signal frames kept consuming the stack.

**What it does.** Reports the address after the `syscall` (both x86-64 `syscall` and `int 0x80` forms are two bytes) in
`si_call_addr`, carries it into the guest signal frame as `SeccompTrapRIP` so the frame's `RIP` differs from the
interrupted `RIP`, and lets `rt_sigreturn` take the path that restores the handler's registers and resumes past the
syscall. `rt_sigreturn` then returns to the top of the syscall dispatcher instead of resuming the interrupted syscall,
so the trapped number is not also executed as a real Linux syscall — which is what Linux does (it never runs a call a
filter traps) and what keeps Wine's `install_bpf` filter, which traps every syscall issued from its preloaded NT stubs,
from re-entering its own dispatcher mid-flight. Patch 0010 is the only patch that touches this path; it is **our patch
code**.

**Without it:** `eac-tests/seccomp-trap` segfaults after installing the filter (1000 trapped syscalls never stabilise).
**Verified by:** `eac-tests/seccomp-trap` and `eac-tests/seccomp-trap-noexec` (14 checks; both pass under FEX with
`FEX_NEEDSSECCOMP=1` and natively, and both fail before this patch — building the series one patch at a time flips
`seccomp-trap-noexec` from FAIL to PASS at exactly patch 0010). **Limits:** the ia32 non-realtime and realtime frame
paths are patched alongside the x86-64 one but no ia32 test exercises them (pending). A trapped syscall while the
guest has `SIGSYS` blocked is deferred instead of killing the process as the kernel does (`force_sig_seccomp`), and the
recorded post-syscall `rip` then stays on the thread until the signal is finally delivered; Wine's `sigsys_handler` never
runs a trapped syscall with `SIGSYS` blocked.

---

## C. Code-invalidation cost and self-modifying code — patches 0002, 0004, 0035

VRChat runs with ~200 guest threads and the injected anti-cheat client toggles page protections constantly, so FEX's
code-range invalidation path is hot.

### 0004 — skip the per-thread walk when the range holds no translated code

**Reasoning.** Every guest `mmap`/`munmap`/`mprotect` invalidated through *every* thread (lock the shared lookup cache,
search the thread's page map, reset its executable-range cache). With ~200 threads that made an `mprotect` of a plain
data page ~22x slower (7.9 µs vs 0.35 µs) and let a process that changes protections in bulk stall its other threads
behind the invalidation locks — seen as **60+ second world joins** and "Failed to connect to region".

**What it does.** Skips the per-thread walk when no code buffer had translated code in the range, and invalidates the
per-thread executable-range caches lazily with an epoch. **Verified by:** `eac-tests/bench` (the churn test,
`EXTRA="-DINVAL_MODE=1 -DINVAL_THREADS=190" ./build.sh`).

### 0002 — stop write-protecting pages that keep faulting

**Reasoning.** The anti-cheat client writes into pages that also hold code it executes (inline-hook trampolines, state
next to code). With the default `SMCChecks=mtrack` every such write costs a `SIGSEGV`, an invalidation and a
re-translation.

**What it does.** Counts faults per page; after `SMCHotPageFaults` (default 32) the page is left writable and the JIT
emits the per-instruction `SMCChecks=full` CRC check for instructions translated from it, so correctness is preserved
and the fault storm stops. `FEX_SMCHOTPAGEFAULTS=0` restores stock behaviour. Both `SMCChecks` and its default are
**stock FEX**; the hot-page counter and the option that disables it are **our patch code**.

**Status: required.** Interleaved launches on FEX-2610, one session each: with `FEX_SMCHOTPAGEFAULTS=0` (stock
behaviour) the anti-cheat launcher stopped at `Starting Wine module mapping`, gave up after about three minutes and the
game never started, in every launch (0 of 4); with the patch active every launch reached a world (4 of 4, 26–34 s).
The fault storm is not a slowdown but a wall while the client is mapped. Once in a world, frame rates matched the
other sessions, so the patch's own cost (the per-instruction check on hot pages) does not show.

### 0035 — code pages the guest makes writable stay writable for other processes

**Reasoning.** On Linux a page a process has mapped writable can be written by another process:
`process_vm_writev()` reaches it with `FOLL_WRITE` and without `FOLL_FORCE`, which succeeds exactly when the VMA is
writable. Under `SMCChecks=mtrack` FEX write-protects the host page of guest code it has translated; the guest's own
writes fault into FEX and resume, but a write from another process takes no fault and fails with `EFAULT`.
`mprotect(RWX)` restores the protection, yet the next translation from the page protects it again. Wine's
`NtWriteVirtualMemory` is wineserver's `process_vm_writev`, so a hook written that way fails with `STATUS_PARTIAL_COPY`
although the guest made the page writable.

**What it does.** `GuestMprotect` notes the executable, non-writable pages of a request that makes them writable and
executable (at most 64 pages) and declares them hot after `TrackMprotect` (`SMCHotPages::MarkHot`, the mechanism of
0002). Hot pages keep the protection the guest asked for and the JIT verifies their code instead; the invalidation that
follows drops code translated before. The page walk runs only for an `mprotect` that asks for `PROT_WRITE|PROT_EXEC`.
The hot-page mechanism is **our patch code** (0002); `SMCChecks` is a stock option.

**Consumer.** The Division 2's protector hooks ntdll with `NtProtectVirtualMemory(PAGE_EXECUTE_READWRITE)` followed by
`NtWriteVirtualMemory`; the write failed, the game reported "Error at hooking API NtProtectVirtualMemory" and exited
before opening a window. Based on yungtry's patch 0028 (`yungtry/steamac`, branch `fex-eac`).

**Test.** `eac-tests/smc-remote-write` (in the tag).

**Status and limits.** The native result is derived from kernel source (`mm/process_vm_access.c`); a bare-metal run of
the test is still pending, and it has not been played on The Division 2 here.

* Only with `SMCChecks=mtrack` (the default) and `SMCHotPageFaults != 0`.
* Pages that were already writable and executable before code ran from them are not covered (no RX to RWX transition
  is seen).
* At most 64 pages per request, and only while the hot set has room (3/4 of its 2048 slots); other pages keep plain
  write protection. A page made hot stays hot for the life of the process.
* `MarkGuestExecutableRange` checks `IsHot` before it takes the VMA lock, so a translation racing with the `mprotect`
  can write-protect the page once; the guest's own writes still work then, a remote write fails until the next
  invalidation.

---

## D. Diagnostics — patches 0003, 0005, 0006, 0009

These fix nothing. Easy Anti-Cheat detects `ptrace` and `perf` ("Forbidden system configuration (Debugger detected.)"),
so the usual tools were off the table; these gave the investigation sight lines, in-process and without a
debugger. Include them if you want to reproduce the analysis; drop them for a minimal "just runs" build.

* **0003 — invalidation stats.** `FEX_PROFILESTATS=1` publishes per-thread JIT/invalidation/SMC counters to
  `/dev/shm/fex-<pid>-stats` (parsed by `tools/fexstats.py`). This is what showed the 0004 cost.
* **0005 — in-process sampling profiler.** `FEX_PROFILESAMPLEHZ=<hz>` (+ `FEX_PROFILESAMPLEALLTHREADS=1`) samples
  *recently busy* guest threads, resolved by `tools/resolve_samples.py`.
* **0006 — all-thread snapshot.** `touch /dev/shm/fex-<pid>-snapshot` records every thread's x86 registers and stack
  once, so a *blocked* thread (which the sampler never sees) can be read; `tools/harness/resolve_snap.py` decodes it.
  The thread-state evidence in [disconnects.md](disconnects.md) comes from it.
* **0009 — futex word in the snapshot.** Extends 0006's record from 80 to 160 words: for a thread blocked in
  `futex`/`futex_waitv` it also stores the futex word, the value the wait started with, and the address. That is what
  separates *nobody woke it* (the word still equals the expected value) from *the wake-up was lost* (the word changed
  but the thread still sleeps). `resolve_snap.py` accepts both record sizes.

`FEX_PROFILESAMPLEHZ` exists **only** because patch 0005 added it.

---

## E. Kernel-fidelity gaps — patches 0011–0015

These are not on VRChat's path. They make the emulated x86-64 process answer the way a real Linux kernel answers on the
same interfaces, so a title that probes them sees Linux rather than FEX's placeholder. Every expectation here was
checked against a bare-metal x86-64 kernel (see the native-reference section in [tools.md](tools.md)); the tests build for the host's
own architecture with `build.sh native` for that reference run.

| patch | interface | what Linux does | what FEX did | test |
|---|---|---|---|---|
| 0011 | `arch_prctl(ARCH_SET_CPUID/ARCH_GET_CPUID)` | on a CPU with `cpuid_fault`: `SET_CPUID(0)` → 0 and a later CPUID raises `#GP` delivered as `SIGSEGV`/`SI_KERNEL`; `GET_CPUID` returns the live state; without the feature `SET_CPUID` → `-ENODEV` | hardcoded `GET_CPUID` = 1, `SET_CPUID` = `-ENODEV`; CPUID always executed | `eac-tests/cpuid-fault` |
| 0012 | `PTRACE_PEEKUSER`/`POKEUSER` on the debug registers | DR0–DR3 at byte 848 of `struct user`; DR0–DR3 POKE validates the address (`-EINVAL` for a non-canonical value); DR4/DR5 POKE `-EIO`, PEEK 0; DR7 POKE validates and stores the raw value; a slot-unaligned or `>= sizeof(struct user)` offset is `-EIO`; an in-struct offset with no handler reads 0 | accepted and silently discarded every out-of-image POKE, `-EPERM` from PEEK | `eac-tests/ptrace-dregs` |
| 0013 | `/proc/<pid>/status` `Seccomp:` and `TracerPid:` | `Seccomp:` is the process's own mode (0/1/2); `TracerPid:` is the pid of its own ptracer, 0 when untraced | no creator — the guest read FEX's own host seccomp mode and tracer | `eac-tests/proc-status` |
| 0014 | `PTRACE_GETREGSET`/`SETREGSET` type | `ptrace_regset()` returns `-EINVAL` for an unknown type, never `-EPERM`; `NT_X86_XSTATE` (0x202) is a known x86-64 regset and succeeds | only `NT_PRSTATUS`/`NT_PRFPREG` emulated, every other type `-EPERM` | `eac-tests/ptrace-regsets` |
| 0015 | `restart_syscall` | `do_no_restart_syscall()` returns `-EINTR` when no restart is pending | registered as a `SYSCALL_STUB` whose body is `ERROR_AND_DIE` — the call killed the process | `eac-tests/restart-syscall` |
| 0033 | `/proc/<pid>/maps` | every user mapping lies below `TASK_SIZE_MAX` ((1 << 47) − 4096); the only line above it is `[vsyscall]`; every file-backed ELF mapping is x86-64 | no creator — the guest read the host file, which also lists FEX, the AArch64 host libraries and the host vdso above 2^47. An EAC launcher module that picks a library from its child's maps to inject into chose a host library, the child died and the launcher reported `210 Unexpected error. (#1)` | `eac-tests/proc-maps` |

**Status.** 0011–0015 are verified: each test passes natively and under the patched FEX and fails without its patch
(measured by building the series one patch at a time), and the Linux side is confirmed against kernel source. Known
limits, all off VRChat's path:

* **0011** — reachable only with `EnableCPUIDFaulting=1`, which is **our patch option**, off by default. The
  per-thread flag is inherited across `fork`/`clone` as on Linux.
* **0012** — the registers are validated, stored and round-tripped like the kernel's `struct user` (928 bytes);
  nothing traps on them — see the limits of group F.
* **0013** — the `/proc/<pid>/status` creator is **our patch code**. The difference shows only with
  `FEX_NEEDSSECCOMP=1` (a stock option), where FEX emulates the guest's filters instead of installing them on the host
  process; that is the setting Wine's seccomp path needs, so the test sets it itself. Without `FEX_NEEDSSECCOMP` the
  line reads 0 whatever filter FEX itself inherited from the host (the guest's own `seccomp()` answers `-EINVAL` then).
* **0014** — FEX does not model the xsave vector state, so the `NT_X86_XSTATE` shadow round-trips zero-filled rather
  than synthesising a fake `xstate_bv`. `NT_ARM_*` types stay `-EINVAL` (they do not exist in an x86-64 view).
* **0033** — the `/proc/<pid>/maps` creator is **our patch code**; it runs only when a guest opens a maps file.
  `smaps`, `smaps_rollup`, `numa_maps`, `task/<tid>/maps` and `map_files/` are still the host's, and FEX's own
  reservations below 2^47 stay listed. Verified live: Mini Royale (Windows build via Proton) went from 210 to 301;
  VRChat stays at 301 and joins a world. The current EAC launcher module looks `libc.so.6` up by name before the
  guest's libc is mapped, so every title whose deployment serves that module needs 0033; VRChat's deployment still
  served the older one. Found independently by yungtry (patch 0027 in `yungtry/steamac`, branch `fex-eac`, which
  filters only a tracee's maps as read by its tracer). The native result is derived from kernel source
  (`page_64_types.h`, `fs/proc/task_mmu.c`); a bare-metal reference run is still to do.

---

## F. Signal frames and faults — patches 0016–0020

Wine runs Windows exception handling on top of Linux signals: a fault becomes a `SIGSEGV`/`SIGILL`/`SIGTRAP`, Wine's
handler turns the `ucontext` into a Windows `CONTEXT`, runs the SEH chain, and writes the result back into the
`ucontext` before `rt_sigreturn`. Debuggers and Wine's `SetThreadContext` on a suspended thread work the same way. So
every field of the frame that FEX gets wrong, and every edit that `rt_sigreturn` drops, is visible to Windows code.
These patches close the gaps that Wine's own `ntdll:exception` test and a real-kernel comparison exposed. All five are
**our patch code**; none adds an option.

| patch | what Linux does | what FEX did | test |
|---|---|---|---|
| 0016 | `rt_sigreturn` always applies the frame's EFLAGS (`FIX_EFLAGS`, which covers TF and DF) | applied the flags only if the handler had also changed RIP, so a handler that set TF to start single-stepping, or changed DF, was ignored | `eac-tests/sigreturn-eflags` |
| 0017 | the frame and `mov %cs`/`%ss` report `__USER_CS` = `0x33` and `__USER_DS` = `0x2b` (`0x23` for 32-bit code) | a bare GDT index (`0x30`) for CS and 0 for SS | `eac-tests/signal-frame` |
| 0018 | `FP_XSTATE_MAGIC1` in `sw_reserved`, `xstate_size` = the state's size, `FP_XSTATE_MAGIC2` right after it (`fpu/signal.c`) — how a handler (Wine's among them) finds the AVX state. Without AVX (`FEX_HOSTFEATURES=disableavx`, no XSAVE in CPUID) the 512-byte FXSAVE frame with no `MAGIC2`, as the kernel writes without XSAVE | wrote the magic only with AVX enabled and a size that pointed past the trailer | `eac-tests/signal-frame`, also with `FEX_HOSTFEATURES=disableavx` |
| 0019 | when the frame cannot be written at the interrupted RSP, the thread dies from `SIGSEGV` (`force_sigsegv`) | wrote the frame without checking the guest mapping; if RSP pointed at host memory FEX corrupted itself inside its own signal handler | `eac-tests/signal-frame` (guard, see below) |
| 0020 | the frame's `uc_sigmask` holds the interrupted mask, and `rt_sigreturn` installs whatever the handler left there, minus `SIGKILL`/`SIGSTOP` (never blockable) | never wrote the field (handlers read stack garbage) and ignored a handler's rewrite | `eac-tests/signal-frame`, `eac-tests/signal-mask` (check 7) |

**Cost.** Nothing on the common path: 0016, 0017, 0018 and 0020 change what is written to or read from a signal frame;
0019 adds one lookup in the guest mapping table per signal delivered.

**Verified by.** Each test passes natively on x86-64 and under the patched FEX and fails without its patch (measured by
building the series one patch at a time). `ntdll:exception` under Proton went from dying before its first check to 8
failures (15 todo). **Limits:** 0019 has no deterministic reproducer — the corruption needs a guest RSP that happens to
land on a host mapping, which was seen in a crash dump but cannot be arranged from a test; `eac-tests/signal-frame` pins
the outcome of the checked path (a stack it cannot write) against the native kernel instead. Hardware
execute breakpoints (DR0–DR3 with DR7) are stored (0012) but never fire; making them fire needs a breakpoint check
inside the JIT, and nothing on this project's path needs it. The remaining `ntdll:exception` failures are in that
area plus segment selectors other than CS/SS and x87/SSE precision corners.

**Withdrawn: `0021` (`pop r/m` leaves RSP unchanged on a fault).** It makes `eac-tests/pop-fault` pass, but its rewrite of the JIT `pop r/m` memory-destination path breaks the normal, non-faulting path. With it installed, the EAC launcher hangs at `Starting Wine module mapping` and never reaches `Launcher finished with: 301` (0001–0020 reach 301; 0001–0021 do not). The patch is kept for reference in [`patches/withdrawn/`](../patches/withdrawn/README.md) and is not part of the series. `eac-tests/pop-fault` therefore fails on the shipped series.

---

## G. Second title — patches 0022–0026

Found by running a second title, THE FINALS, live. Its anti-tamper bootstrapper traps every NT syscall through Wine's
seccomp filter, reads `/proc/self/maps` and probes every inaccessible range, single-steps over a `syscall`, and has
Wine end threads that sit in trapped syscalls. Each patch below is one of those observations; none of them costs
anything on the common path.

| patch | behaviour | stock FEX | patched | test |
|---|---|---|---|---|
| 0022 | a SIGSYS trap delivered from inside the syscall handler returns to the thread's own host-stack level | every trap left the handler's host frames behind (~400 B); a thread died after a few thousand traps | no growth; 100 000 traps on one thread | `eac-tests/seccomp-trap` (check 6) |
| 0023 | a guest read of FEX's call/ret shadow-stack guard page is delivered to the guest as SIGSEGV | treated as a shadow-stack over/underflow and the same load resumed: an endless host fault loop | only a fault where an x25 push/pop lands is a shadow-stack event | `eac-tests/maps-probe` |
| 0024 | `FEX_PROFILESAMPLEMINTHREADS` sets the sampler's thread threshold; `0` stores one snapshot per external `SIGPROF` instead of starting a sampler thread | the sampler started only at 100 threads | for a process that must not grow a thread | — (diagnostic option, group D) |
| 0025 | with TF set, the trap after `popfq; iretq` lands on the `iretq` target, and single-stepping across translated code traps once per instruction | the trap landed one instruction late, or escaped through the lookup caches | dispatcher bounce on `iretq` and on indirect exits while TF is set | `eac-tests/tf-iret` |
| 0026 | a thread that exits from a signal handler running on its alt stack keeps that stack until it has left it | the alt stack was unmapped under the running handler; Wine's thread termination crashed the process | released after the return to the thread's own stack | `eac-tests/seccomp-trap` (check 7) |

**Without them:** the bootstrapper's first thread spins forever in FEX's fault handler (0023), its single-step check
fails and it writes a crash dump (0025), and a seccomp-heavy Wine thread dies of host-stack exhaustion (0022). With
0022–0026 the title gets past its bootstrapper and starts its anti-cheat runtime; it then stopped at exceptions its
anti-tamper code probes, which group I (0054–0070) raises like a CPU. With both groups the title plays.

**Cost.** 0022 is per-trap work only, 0023 per host fault inside the shadow-stack allocation, 0025 only on `iretq` and on
blocks compiled while TF is set, 0026 only on thread exit.

**Verified by.** Each test passes natively on x86-64 and under the patched FEX; `maps-probe` hangs and `tf-iret` fails
checks 4–5 on the 0001–0020 build (same code as stock). A Wine program that terminates a thread spinning in raw
syscalls crashed 3/3 without 0026 and completes 160/160 with it, as natively.

On VRChat the five patches change nothing measurable: in back-to-back idle in-world runs under the same host
conditions, 0001–0020 gave 37.7 and 41.3 fps and 0001–0026 gave 39.0, 48.6 and 41.7 fps (1% lows 17.5–18.7 and
18.4–22.1), every run joined a world, and the EAC launcher finished with `301` on the 0001–0026 build.

## H. Fixes since `eac-2610.2` — patches 0036–0039, 0041–0053

Added in `eac-2610.3`, each through a pull request on the fork with its test in `eac-tests/`. Since they arrive through
merged pull requests, their commit order is not their number order; `patches/` keeps the commit order. 0034 and 0040
are held (no consumer yet) and are not in the release.

| patch | behaviour | stock FEX | patched | test |
|---|---|---|---|---|
| 0036 | a `SECCOMP_RET_TRAP` syscall never runs and its SIGSYS is forced, as Linux does | with SIGSYS blocked the trapped syscall ran as a Linux syscall | rolled back, SIGSYS forced (default action if blocked or ignored) | `eac-tests/seccomp-trap-blocked` |
| 0037 | a code page is write-protected before its translation is published | a second thread compiling from the same page could run stale code after a store | protection first, then publish | `eac-tests/smc-same-page` |
| 0038 | `execve` with a seccomp filter installed keeps the `FEX_EXECVEFD` path for FD execs only | a `#!` script exec failed with `FEX_NEEDSSECCOMP=1` | same condition as without seccomp | `eac-tests/execve-seccomp` |
| 0039 | an FD `execveat` through the FEX loader keeps `argv[0]` | `fexecve()` argv shifted by one | caller's `argv[0]` kept | `eac-tests/fdexec-argv` |
| 0041 | every `rt_sigreturn` restores the interrupted `InSyscallInfo` | the next async signal built its frame from a stale, higher RSP over live stack (Go/.NET preemption) | no stale marker | `eac-tests/sigreturn-syscallinfo` |
| 0042 | bad user pointers in FEX's own syscall handlers return `-EFAULT` | SIGSEGV inside FEX killed the process | `-EFAULT`, as Linux | `eac-tests/syscall-efault`, unittests |
| 0043 | `FEX_SIGNALTRACE`'s `/dev/shm` ring is unlinked on exit (follow-up to 0007) | left behind (16 MiB per process) | removed on a regular exit | `eac-tests/sigtrace-unlink` |
| 0044 | every I/O step of the synthesized `/proc/<pid>/maps` is checked (follow-up to 0033) | a short write or `EINTR` truncated the file | retried; on failure the host file is used | `eac-tests/proc-maps` |
| 0045 | a kernel write (`read`, `recvfrom`, ...) into a page holding translated code succeeds | `EFAULT` or a short count | code invalidated and page made writable first (FEX-Emu#5874) | `eac-tests/smc-kernel-write` |
| 0046 | an x86-64 `munmap` may cross 4 GiB | `EOVERFLOW` (Chromium-based clients abort) | host `munmap`, 32-bit allocator told (FEX-Emu#5847) | `eac-tests/munmap-4g` |
| 0047 | FEX's own SIGBUS alignment stops are hidden from a ptrace tracer | the EAC launcher of The Division 2 saw ~1000 SIGBUS stops and failed with error 210 | hidden like FEX's SIGTRAP/SIGSEGV stops (by yungtry) | `eac-tests/ptrace-sigbus` |
| 0048, 0049 | `prctl(PR_SET_SYSCALL_USER_DISPATCH)` is emulated per thread | passed to the host (`EINVAL` on Arm64), so Wine 11's raw-syscall trap path ran Windows syscalls as Linux ones | SIGSYS with `SYS_USER_DISPATCH`, one branch per syscall when unused | `eac-tests/sigsys-redirect` |
| 0050 | reserved EFLAGS bit 31 stays clear in signal frames taken outside JIT code | the packed overflow flag leaked as bit 31 (`0x80000ad7` vs `0xad7`) into the Windows CONTEXT | bit clear | `eac-tests/sigsys-redirect` |
| 0051, 0052 | only the trap's own SIGSYS (seccomp or Syscall User Dispatch) takes the resume address | an async signal arriving between filter and delivery swallowed the trap | the trap is always delivered | `eac-tests/signal-race` |
| 0053 | a trap frame whose handler switches context instead of returning releases its host stack | ~2.9 KiB host stack leaked per trap; Wine's raw `NtContinue` pattern crashed after ~200 000 traps in a memory-limited VM | reused on the next trap | `eac-tests/sigsys-redirect` (phase C) |

**Cost.** 0048 adds one branch per syscall; 0045 one compare and one VMA lookup per affected syscall when SMC tracking
is on; the others act only on their own path (traps, faults, exec, exit).

## I. Fixes since `eac-2610.3` — patches 0054–0070

Added in `eac-2610.4`, each through a pull request on the fork with its test. Most were found on THE FINALS, whose
anti-tamper code probes exceptions a real CPU raises; 0070 on Hell Let Loose. Three commits only bump the fork's
DiskCache format revision for a codegen change (0057, 0062, 0068) and three update the instruction-count CI after one
(0058, 0063, 0069); they belong to the row of the change that needed them.

| patch | behaviour | stock FEX | patched | test |
|---|---|---|---|---|
| 0054 | IRETQ with RFLAGS.NT = 1 raises #GP(0) before it pops anything | executed as a normal IRETQ | SIGSEGV, `SI_KERNEL`, trapno 13, frame intact | `eac-tests/iret-nt`, `eac-tests/wine-ntflag` |
| 0055, 0057 | MOV to FS/GS in 64-bit mode is a data segment load against Linux's GDT | refused as undecodable (Wine: illegal instruction) | null or user selectors load with base 0; others #GP with the selector as error code | `eac-tests/seg-load64` |
| 0056, 0058 | the disk cache key carries a format version of the fork's own (`0x8000 \| revision << 8 \| upstream`) | upstream's version 30: a cache filled by one build was reused by the next, so blocks translated before a fix kept running | each codegen change bumps the revision (now `0x841E`) | InstCountCI |
| 0059 | an instruction that runs from executable memory into memory that is not reports the end of the executable range as its fetch fault address | RIP reported as `si_addr` | first byte it could not fetch, RIP at the instruction | `eac-tests/insn-straddle` |
| 0060–0063 | instructions longer than 15 bytes, and XSETBV in user mode, raise #GP(0) | SIGILL, a fetch fault, or (immediate/displacement) executed; XSETBV SIGILL with trapno 0 (Wine: "unexpected trap 0") | #GP(0) as a CPU raises it | `eac-tests/insn-toolong` |
| 0064 | a fetch fault takes the range end as its address only when it lies within 15 bytes of RIP | after another thread made the page executable, an address no instruction at RIP can reach (THE FINALS decrypts code pages on execute faults from several threads; Unreal crashed) | otherwise the instruction's first byte faulted, as on hardware | `eac-tests/insn-straddle` |
| 0065 | guest code is read fault-safely while compiling | a read-ahead fault on a page another thread revoked reached the guest handler with the compiler's frames below it, leaking a code-invalidation lock hold (THE FINALS froze in a match) | the decode stops at the unreadable byte | `eac-tests/decode-race` |
| 0066–0069 | two-byte `0F B8/BC/BD` decode with F2/F3/66 prefixes in any order (backport of upstream 68f4c5fbc and its test 95a305bc4) | `F3 66 0F B8` SIGILL, `F3 66 0F BC/BD` as BSF/BSR | POPCNT / TZCNT / LZCNT | `unittests/ASM/FEX_bugs/two_byte_0xbx` |
| 0070 | a store that faults on a page with translated code restarts its instruction only when it can reach the running block (64 bytes around the fault address) | every data store next to running code restarted the instruction from mid-instruction state: `adc`/`rcl`/`rcr` reused the CF they produced, `pop` to memory and `fstp` popped twice (Hell Let Loose: EAC `210 (#2)`) | the store completes; stores into the block still restart | `eac-tests/smc-restart-state` |

**Cost.** 0054 one flag test per IRETQ; 0055 only on MOV to FS/GS; 0060 a length check in the decoder; the others act
only on their own path (faults, decode of unreadable bytes, SMC write faults).

**Verified by.** Each test passes natively on x86-64 (GitHub runners) and under the patched FEX. In the SteamOS guest
on the release code: `eac-tests/run-all.sh` 33/33 expected (the `eac-2610.2`-era build gave 30/31), `sigsys-redirect`
30/30 runs, and VRChat with EAC (flat, Proton Experimental) joined a world with EAC Session Begin on 3 launches (two stayed 10 min, one 2 min); one more launch hit the intermittent world-load freeze that the `eac-2610.2`-era build shows as well (not FEX-specific).

---

## Applying

`scripts/build-fex.sh` clones FEX-Emu 2610 and applies the series in order; it is idempotent (re-running on an already
patched tree is a no-op). See the Quick start in the [README](../README.md).

## Fork it

This is a proof of concept for one game on one machine. The patches are deliberately kept as patch files rather than
proposed to the FEX project, and nothing here is submitted to FEX or Proton. If you want to take it further — another
game, another SoC, another host — please fork it.
