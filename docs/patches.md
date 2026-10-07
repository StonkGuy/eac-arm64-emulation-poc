# The patch series

Ten patches against **FEX-Emu 2609.1** (base commit `9fbdc00`). They are not all the same kind of thing, so they are
grouped here by what they are for:

| group | patches | what it is |
|---|---|---|
| **A. Faithful `ptrace`** | 0001 | the change that lets the anti-cheat launcher inject its client at all |
| **B. Signal fidelity** | 0007, 0008, 0009 | guest signal handlers must behave like Linux; this is what fixed the disconnects |
| **C. Code-invalidation cost** | 0002, 0004 | performance in a process with ~200 threads |
| **D. Diagnostics** | 0003, 0005, 0006, 0010 | **not fixes** — tools that made the bugs visible without ptrace |

Only group B and patch 0001 are required to run; group C is performance, group D is instrumentation you can drop.

Everything is measured on **one game (VRChat) on one machine (Apple M2, Fedora Asahi Remix)**. Treat the series as a
starting point to fork, not as a supported port.

---

## A. Faithful `ptrace` — patch 0001

**Reasoning.** The Proton EasyAntiCheat runtime does not decide the verdict in-process: a launcher
(`start_protected_game.exe`) `ptrace`s a child, single-steps it, reads and rewrites its x86-64 registers, pokes code into
its memory and injects the client shared object at the right moment. Under FEX, tracer and tracee are both *aarch64*
processes hosting FEX, so the host kernel would hand the tracer aarch64 registers, host-syscall stops and a `SIGTRAP`
that means something else. Stock FEX passes through a small subset of ptrace (enough for Ubisoft's Wine launcher) and
rejects the rest, so the launcher fails with `Unexpected error (#1)` and no anti-cheat session ever starts.

**What it does.** `LinuxSyscalls/PtraceEmu.{h,cpp}` presents an x86-64 view of the tracee from inside FEX: syscall-entry
and syscall-exit stops with x86-64 numbers/registers, signal-delivery stops for guest `int3`/faults, the requests the
launcher uses (`GETREGS/SETREGS`, `GETREGSET/SETREGSET`, `PEEK*`/`POKE*` with code-cache invalidation, `SETOPTIONS`,
`GETSIGINFO`, `CONT`, `SYSCALL`, `DETACH`), and a `/proc/<pid>/exe` that names the guest binary. The full conversation
is in [how-it-works.md](how-it-works.md).

**Without it:** no anti-cheat session. **Verified by:** `tests/ptrace-inject` (28 checks, passes natively and under
FEX). **Limits:** only the main thread of a tracee; `PTRACE_ATTACH` to an already-running FEX process is not emulated.

---

## B. Signal fidelity — patches 0007, 0008, 0009

This is the group that fixed the *disconnects*. The symptom was a lost wake-up in Wine's server protocol: a thread
suspended by `SIGUSR1` and a socket reader parked in the 16-byte wait-pipe read would never wake although data had
arrived, so Photon timed out ~25–33 s after entering a world and VRChat looped "join → show world → timed out →
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
reproducer that wedged 5–6 of 6 runs. **With it:** 0 of 7 real-game sessions timed out vs 4 of 7 control; the reproducer
0 of 6 vs 6 of 6 (p = 0.002). **Verified by:** `tests/signal-mask` (checks 1, 3, 4b fail with `FEX_SIGNALMASKFIX=0`).

### 0008 — do not leave `InSyscallInfo` set for a handler entered from a syscall

**Reasoning.** Found on the way: a test that *should* have passed once 0007 was in did not. Every `tgkill`/`kill`/`tkill`
to self — and every signal that finds a thread parked in `futex` or `read` — is delivered while the FEX-internal C++
half of the `syscall` is still on the stack, where the JIT has left `Frame->InSyscallInfo == 0xFFFF` ("all guest
registers are already spilled to the frame"). FEX clears that marker only when the interrupted code was in the JIT, so
the handler ran with a stale marker; the SMC write-fault handler and a second signal in translated code trust it, spill
nothing, and redispatch from stale frame contents — the register changes the handler made are silently reverted.

**What it does.** Clears `InSyscallInfo` for the handler when the interrupted code was not in the JIT, and restores it at
`rt_sigreturn`. **Verified by:** `tests/signal-regs` (raises the signal four ways; the `tgkill`/`kill`/`tkill` cases fail
on stock FEX). This is a plain correctness bug, *not* the cause of the disconnects (with 0008 but without 0007 the
reproducer still wedged 6 of 6). Analysis: [signal-registers.md](signal-registers.md).

### 0009 — never block FEX's own signals in the guest sigsuspend set

**Reasoning.** The same family as 0007. `rt_sigsuspend`/`rt_sigtimedwait` take a mask of signals to *wait through*;
`GuestSigSuspend` copied the guest's mask straight into the host `sigsuspend()` set without removing the signals FEX
needs delivered to itself (`SIGSEGV`, `SIGTRAP`, `SIGNAL_FOR_PAUSE`). A guest that suspends with one of those in its
mask — an uncommon but legal thing to do — makes the host kernel hold that signal pending forever, so FEX never sees its
own fault/pause signal while blocked there. `GuestSigProcmask` and the handler-entry path already drop these from the
host mask; this makes the suspend path agree with them. Guest-visible behaviour is unchanged (the guest still thinks it
blocked them); only the host set is adjusted.

**Verified by:** the whole test set with the change applied — `tests/ptrace-inject` (28 checks), `tests/signal-mask`
and `tests/signal-regs` all still `RESULT: PASS`, so it is at least regression-free. **Status: unproven as a fix** — it
was written against the *pre-join stall* (the remaining open issue, [disconnects.md](disconnects.md)), where the parked
thread is in Wine's server-reply pipe read; whether the game actually hits this `rt_sigsuspend` window was never
confirmed. Treat it like 0002: correct-looking, cheap, but A/B it before leaning on it.

---

## C. Code-invalidation cost — patches 0002, 0004

VRChat runs with ~200 guest threads and the injected anti-cheat client toggles page protections constantly, so FEX's
code-range invalidation path is hot.

### 0004 — skip the per-thread walk when the range holds no translated code

**Reasoning.** Every guest `mmap`/`munmap`/`mprotect` invalidated through *every* thread (lock the shared lookup cache,
search the thread's page map, reset its executable-range cache). With ~200 threads that made an `mprotect` of a plain
data page ~20x slower (7.9 µs vs 0.35 µs) and let a process that changes protections in bulk stall its other threads
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
and the fault storm stops. `FEX_SMCHOTPAGEFAULTS=0` restores stock behaviour.

**Status: unproven.** It is here because the fault storm is real and the change is cheap, but whether it moves the frame
rate *on this workload* was never established — treat it as optional, and A/B it before relying on it.

---

## D. Diagnostics — patches 0003, 0005, 0006

These fix nothing. Easy Anti-Cheat detects `ptrace` and `perf` ("Forbidden system configuration (Debugger detected.)"),
so the usual tools were off the table; these three gave the investigation sight lines, in-process and without a
debugger. Include them if you want to reproduce the analysis; drop them for a minimal "just runs" build.

* **0003 — invalidation stats.** `FEX_PROFILESTATS=1` publishes per-thread JIT/invalidation/SMC counters to
  `/dev/shm/fex-<pid>-stats` (parsed by `tools/fexstats.py`). This is what showed the 0004 cost.
* **0005 — in-process sampling profiler.** `FEX_PROFILESAMPLEHZ=<hz>` (+ `FEX_PROFILESAMPLEALLTHREADS=1`) samples
  *recently busy* guest threads, resolved by `tools/resolve_samples.py`.
* **0006 — all-thread snapshot.** `touch /dev/shm/fex-<pid>-snapshot` records every thread's x86 registers and stack
  once, so a *blocked* thread (which the sampler never sees) can be read; `tools/harness/resolve_snap.py` decodes it.
  This is what identified the parked-by-the-wait-pipe threads behind the disconnects.
* **0010 — futex word in the snapshot.** Extends 0006's record from 80 to 160 words: for a thread blocked in
  `futex`/`futex_waitv` it also stores the futex word, the value the wait started with, and the address. That is what
  separates *nobody woke it* (the word still equals the expected value) from *the wake-up was lost* (the word changed
  but the thread still sleeps) — the distinction the pre-join stall hung on. `resolve_snap.py` accepts both record
  sizes. (**Note:** the original known-good build carried this diagnostic as an uncommitted source edit; 0010 makes it
  reproducible from the patch series.)

---

## Applying

`scripts/build-fex.sh` clones FEX-Emu 2609.1 and applies the series in order; it is idempotent (re-running on an already
patched tree is a no-op). See the Quick start in the [README](../README.md).

## Fork it

This is a proof of concept for one game on one machine. The patches are deliberately kept as patch files rather than
proposed to the FEX project, and nothing here is submitted to FEX or Proton. If you want to take it further — another
game, another SoC, folding the diagnostics into FEX's own tooling, or turning 0004/0007 into something upstreamable —
please fork it.
