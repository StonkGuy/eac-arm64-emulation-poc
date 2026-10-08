# The patch series

Twenty-one patches against **FEX-Emu 2610** (base commit `14c9268`). 0001–0010 were developed and measured on 2609.1
(`9fbdc00`) and rebase onto 2610 with no code changes; 0011–0021 were written against 2610. They are not all the same kind of thing, so they are
grouped here by what they are for:

| group | patches | what it is |
|---|---|---|
| **A. Faithful `ptrace`** | 0001 | the change that lets the anti-cheat launcher inject its client at all |
| **B. Signal fidelity** | 0007, 0008, 0010 | guest signal handlers must behave like Linux; **0007** is what fixed the disconnects (0008 is a correctness fix in the same area, 0010 the seccomp/SIGSYS path) |
| **C. Code-invalidation cost** | 0002, 0004 | 0002: without it the anti-cheat client never finishes loading; 0004: performance with ~200 threads |
| **D. Diagnostics** | 0003, 0005, 0006, 0009 | **not fixes** — tools that made the bugs visible without ptrace |
| **E. Kernel-fidelity gaps** | 0011, 0012, 0013, 0014, 0015 | the answers a real kernel gives for `arch_prctl`, the debug registers, `/proc/<pid>/status`, unknown regsets and `restart_syscall` |
| **F. Signal frames and faults** | 0016–0021 | what Windows exception handling under Wine reads from and writes to a signal frame, and RSP after a faulting `pop` |

Patches 0001–0010 are needed for the game (0001 to inject at all, 0002 so the injected client finishes loading, 0007 to
avoid the disconnects, 0010 for the seccomp/SIGSYS path; 0008 is a correctness fix in the same area and 0004 is
performance). Group D is instrumentation you can drop. Groups E and F are not on VRChat's path — they exist because a
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

**Without it:** no anti-cheat session. **Verified by:** `tests/ptrace-inject` (28 checks, passes under FEX; the binary
also targets native x86-64 Linux as the reference, see [tools.md](tools.md)). **Limits** (all off the launcher's path,
none costs anything when unused): only the main thread of a tracee is traced; `PTRACE_ATTACH` to an already-running FEX
process is not emulated; `PTRACE_SINGLESTEP` and `PTRACE_SEIZE`/`INTERRUPT`/`LISTEN` are passed to the host and do not
give a guest-instruction stop; `PEEKUSER`/`POKEUSER` at an in-`struct user` offset with no register handler answer `0`
where a real kernel answers `-EIO` (patch 0012 fixes this for the debug-register offsets the launcher uses).

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
0 of 6 vs 6 of 6 (p = 0.002). **Verified by:** `tests/signal-mask` (checks 1, 3, 4b fail with `FEX_SIGNALMASKFIX=0`).

### 0008 — do not leave `InSyscallInfo` set for a handler entered from a syscall

**Reasoning.** Found while testing 0007: a test that *should* have passed once 0007 was in did not. Every `tgkill`/`kill`/`tkill`
to self — and every signal that finds a thread parked in `futex` or `read` — is delivered while the FEX-internal C++
half of the `syscall` is still on the stack, where the JIT has left `Frame->InSyscallInfo == 0xFFFF` ("all guest
registers are already spilled to the frame"). FEX clears that marker only when the interrupted code was in the JIT, so
the handler ran with a stale marker; the SMC write-fault handler and a second signal in translated code trust it, spill
nothing, and redispatch from stale frame contents — the register changes the handler made are silently reverted.

**What it does.** Clears `InSyscallInfo` for the handler when the interrupted code was not in the JIT, and restores it at
`rt_sigreturn`. **Verified by:** `tests/signal-regs` (raises the signal four ways; the `tgkill`/`kill`/`tkill` cases fail
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

**Without it:** `tests/seccomp-trap` segfaults after installing the filter (1000 trapped syscalls never stabilise).
**Verified by:** `tests/seccomp-trap` and `tests/seccomp-trap-noexec` (14 checks; both pass under FEX with
`FEX_NEEDSSECCOMP=1` and natively, and both fail before this patch — building the series one patch at a time flips
`seccomp-trap-noexec` from FAIL to PASS at exactly patch 0010). **Limits:** the ia32 non-realtime and realtime frame
paths are patched alongside the x86-64 one but no ia32 test exercises them (pending).

---

## C. Code-invalidation cost — patches 0002, 0004

VRChat runs with ~200 guest threads and the injected anti-cheat client toggles page protections constantly, so FEX's
code-range invalidation path is hot.

### 0004 — skip the per-thread walk when the range holds no translated code

**Reasoning.** Every guest `mmap`/`munmap`/`mprotect` invalidated through *every* thread (lock the shared lookup cache,
search the thread's page map, reset its executable-range cache). With ~200 threads that made an `mprotect` of a plain
data page ~22x slower (7.9 µs vs 0.35 µs) and let a process that changes protections in bulk stall its other threads
behind the invalidation locks — seen as **60+ second world joins** and "Failed to connect to region".

**What it does.** Skips the per-thread walk when no code buffer had translated code in the range, and invalidates the
per-thread executable-range caches lazily with an epoch. **Verified by:** `tests/bench` (the churn test,
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
| 0011 | `arch_prctl(ARCH_SET_CPUID/ARCH_GET_CPUID)` | on a CPU with `cpuid_fault`: `SET_CPUID(0)` → 0 and a later CPUID raises `#GP` delivered as `SIGSEGV`/`SI_KERNEL`; `GET_CPUID` returns the live state; without the feature `SET_CPUID` → `-ENODEV` | hardcoded `GET_CPUID` = 1, `SET_CPUID` = `-ENODEV`; CPUID always executed | `tests/cpuid-fault` |
| 0012 | `PTRACE_PEEKUSER`/`POKEUSER` on the debug registers | DR0–DR3 at byte 848 of `struct user`; DR0–DR3 POKE validates the address (`-EINVAL` for a non-canonical value); DR4/DR5 POKE `-EIO`, PEEK 0; DR7 POKE validates and stores the raw value; a slot-unaligned or `>= sizeof(struct user)` offset is `-EIO`; an in-struct offset with no handler reads 0 | accepted and silently discarded every out-of-image POKE, `-EPERM` from PEEK | `tests/ptrace-dregs` |
| 0013 | `/proc/<pid>/status` `Seccomp:` and `TracerPid:` | `Seccomp:` is the process's own mode (0/1/2); `TracerPid:` is the pid of its own ptracer, 0 when untraced | no creator — the guest read FEX's own host seccomp mode and tracer | `tests/proc-status` |
| 0014 | `PTRACE_GETREGSET`/`SETREGSET` type | `ptrace_regset()` returns `-EINVAL` for an unknown type, never `-EPERM`; `NT_X86_XSTATE` (0x202) is a known x86-64 regset and succeeds | only `NT_PRSTATUS`/`NT_PRFPREG` emulated, every other type `-EPERM` | `tests/ptrace-regsets` |
| 0015 | `restart_syscall` | `do_no_restart_syscall()` returns `-EINTR` when no restart is pending | registered as a `SYSCALL_STUB` whose body is `ERROR_AND_DIE` — the call killed the process | `tests/restart-syscall` |

**Status.** All five are verified: each test passes natively and under the patched FEX and fails without its patch
(measured by building the series one patch at a time), and the Linux side is confirmed against kernel source. Known
limits, all off VRChat's path:

* **0011** — reachable only with `EnableCPUIDFaulting=1`, which is **our patch option**, off by default. The
  per-thread flag is inherited across `fork`/`clone` as on Linux.
* **0012** — the registers are validated, stored and round-tripped like the kernel's `struct user` (928 bytes);
  nothing traps on them — see the limits of group F.
* **0013** — the `/proc/<pid>/status` creator is **our patch code**. The difference shows only with
  `FEX_NEEDSSECCOMP=1` (a stock option), where FEX emulates the guest's filters instead of installing them on the host
  process; that is the setting Wine's seccomp path needs, so the test sets it itself.
* **0014** — FEX does not model the xsave vector state, so the `NT_X86_XSTATE` shadow round-trips zero-filled rather
  than synthesising a fake `xstate_bv`. `NT_ARM_*` types stay `-EINVAL` (they do not exist in an x86-64 view).

---

## F. Signal frames and faults — patches 0016–0021

Wine runs Windows exception handling on top of Linux signals: a fault becomes a `SIGSEGV`/`SIGILL`/`SIGTRAP`, Wine's
handler turns the `ucontext` into a Windows `CONTEXT`, runs the SEH chain, and writes the result back into the
`ucontext` before `rt_sigreturn`. Debuggers and Wine's `SetThreadContext` on a suspended thread work the same way. So
every field of the frame that FEX gets wrong, and every edit that `rt_sigreturn` drops, is visible to Windows code.
These patches close the gaps that Wine's own `ntdll:exception` test and a real-kernel comparison exposed. All six are
**our patch code**; none adds an option.

| patch | what Linux does | what FEX did | test |
|---|---|---|---|
| 0016 | `rt_sigreturn` always applies the frame's EFLAGS (`FIX_EFLAGS`, which covers TF and DF) | applied the flags only if the handler had also changed RIP, so a handler that set TF to start single-stepping, or changed DF, was ignored | `tests/sigreturn-eflags` |
| 0017 | the frame and `mov %cs`/`%ss` report `__USER_CS` = `0x33` and `__USER_DS` = `0x2b` (`0x23` for 32-bit code) | a bare GDT index (`0x30`) for CS and 0 for SS | `tests/signal-frame` |
| 0018 | `FP_XSTATE_MAGIC1` in `sw_reserved`, `xstate_size` = the state's size, `FP_XSTATE_MAGIC2` right after it (`fpu/signal.c`) — how a handler (Wine's among them) finds the AVX state | wrote the magic only with AVX enabled and a size that pointed past the trailer | `tests/signal-frame` |
| 0019 | when the frame cannot be written at the interrupted RSP, the thread dies from `SIGSEGV` (`force_sigsegv`) | wrote the frame without checking the guest mapping; if RSP pointed at host memory FEX corrupted itself inside its own signal handler | `tests/signal-frame` (guard, see below) |
| 0020 | the frame's `uc_sigmask` holds the interrupted mask, and `rt_sigreturn` installs whatever the handler left there | never wrote the field (handlers read stack garbage) and ignored a handler's rewrite | `tests/signal-frame` |
| 0021 | a faulting instruction leaves RSP unchanged | `pop r/m` committed the RSP increment before the store that faults, so the frame carried RSP + 8 and a handler resuming past the instruction (`ntdll:exception`'s `popq (%rax)` case) re-faulted forever | `tests/pop-fault` |

**Cost.** Nothing on the common path: 0016, 0017, 0018 and 0020 change what is written to or read from a signal frame;
0019 adds one lookup in the guest mapping table per signal delivered; 0021 changes only `pop` with a memory
destination (a register destination keeps the fused single load).

**Verified by.** Each test passes natively on x86-64 and under the patched FEX and fails without its patch (measured by
building the series one patch at a time). `ntdll:exception` under Proton went from dying before its first check to 8
failures (15 todo). **Limits:** 0019 has no deterministic reproducer — the corruption needs a guest RSP that happens to
land on a host mapping, which was seen in a crash dump but cannot be arranged from a test; `tests/signal-frame` pins
the outcome of the checked path (a stack it cannot write) against the native kernel instead. Hardware
execute breakpoints (DR0–DR3 with DR7) are stored (0012) but never fire; making them fire needs a breakpoint check
inside the JIT, and nothing on this project's path needs it. The remaining `ntdll:exception` failures are in that
area plus segment selectors other than CS/SS and x87/SSE precision corners.

---

## Applying

`scripts/build-fex.sh` clones FEX-Emu 2610 and applies the series in order; it is idempotent (re-running on an already
patched tree is a no-op). See the Quick start in the [README](../README.md).

## Fork it

This is a proof of concept for one game on one machine. The patches are deliberately kept as patch files rather than
proposed to the FEX project, and nothing here is submitted to FEX or Proton. If you want to take it further — another
game, another SoC, another host — please fork it.
