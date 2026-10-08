# Registers lost inside a signal handler

Status: reproduced on stock FEX 2609.1 with `tests/signal-regs`; `patches/0008` fixes it (the test passes on the patched build).

## Symptom

A guest signal handler that is entered while its thread is inside a system call can lose the register values it sets
itself: after the handler's first store into a page that also holds translated code (or after a second signal arrives
inside it) the registers it had changed hold the values of the interrupted code again.

`tests/signal-regs/signal_regs_test` shows it with a handler written in assembly that sets `r8` and `r9`, increments a counter
that sits in the same RWX section as the handler, makes two system calls and then reports `r8` and `r9`. The signal is raised in
four ways, each twice:

| how the signal is raised | handler keeps `r8`/`r9` on stock FEX 2609.1 |
|---|---|
| `tgkill(self)`, `kill(self)`, `tkill(self)` | **no**, both registers hold the interrupted code's values |
| `setitimer`, asynchronous, arrives in translated code | yes |

All three go through the same path: the signal is delivered while the thread is still in the C++ half of its own `syscall`.
Measured with `tests/signal-regs` on unpatched FEX-2609.1 (and on Fedora's FEX 2604 package): all six `tgkill`/`kill`/`tkill`
runs fail, as do the four `tgkill`/`kill` runs whose handler takes a `SIGSEGV`; with patch 0008 every case passes.

A second set of cases uses a handler that takes a `SIGSEGV` of its own (a load from address 0, stepped over by a `SIGSEGV`
handler) before its first system call. That is the other consumer of the stale marker (a nested signal, no self-modifying code
involved) and fails the same way on stock FEX when the outer signal was raised by `tgkill`/`kill` to self.

On Linux every case passes (the same source builds for aarch64 as a reference run: `tests/signal-regs/build.sh native`).

The same thing breaks a compiled handler: with `-O1` clang keeps the handler's `sig` argument in `r8d`
across the whole body — the stores to its globals and six system calls before the last read of `r8d`
(`movl %edi, %r8d` … `movslq %r8d, %rdx`) — and `signal_mask_test` then sends `tgkill(pid, tid, <pid>)` instead of
`tgkill(pid, tid, SIGUSR1)`, which is why its checks
4a, 4b and 6 failed on FEX with the signal-mask fix enabled and passed again as soon as the handler had one more instruction
in front of the store.

## What decides it

* `FEX_SMCCHECKS=none` or `full` hides it, and so does `FEX_MAXINST=1` (single-instruction blocks). The default `mtrack`
  mode shows it.
* `kill`, `tkill` and `tgkill` to self arrive while FEX is inside the C++ half of the guest `syscall` (the signal is delivered as the
  host system call returns), `setitimer` signals arrive in translated code. Only the former fail.
* The first run of a handler (it has to be translated first) and later runs behave the same.

## Cause

The JIT's `Syscall` op spills every guest register into the thread frame, stores `InSyscallInfo = 0xFFFF` ("everything is
spilled") and calls the C++ syscall handler. `HandleDispatcherGuestSignal` clears `InSyscallInfo` only when the interrupted
code was in the JIT. When it was in the C++ half of the syscall, the guest handler starts with `InSyscallInfo == 0xFFFF` and keeps
it until the handler's own first system call has completed.

Two consumers use that value as the mask of registers that are already in the frame and need no spilling:

* the SMC write-fault handler (`SyscallsSMCTracking.cpp`: `SpillSRA(Thread, ucontext, InSyscallInfo & 0xFFFF)`), which is
  entered when the handler writes to a page containing translated code, and spills, then redispatches to a single-instruction
  block;
* `HandleDispatcherGuestSignal` for a second signal that arrives in translated code.

With a stale `0xFFFF` both spill nothing, and the redispatch reloads the registers from the frame, which still holds the values
from the moment the handler was entered. Everything the handler had changed in registers since then is gone.

## Fix

`patches/0008`: when the interrupted code was not in the JIT, clear `InSyscallInfo` before the handler runs (remembering the old
value in the context backup) and put it back at `rt_sigreturn`, because the syscall handler that was interrupted continues and a
signal can still arrive while the `Syscall` op refills the registers.

## Does it matter for Wine and VRChat?

Wine delivers `SIGUSR1` to threads that are parked in system calls (`futex_waitv`, `read` on the server pipe), so its handlers
start on exactly this path, and with the unfixed signal mask `SIGUSR1` can also nest inside its own handler, which is the second
trigger above. It is nevertheless not the cause of the Photon time-outs: with patch 0008 and without `FEX_SIGNALMASKFIX` the
Wine reproducer of [disconnects.md](disconnects.md) still wedged in every control run (6 of 6), and with the signal-mask fix it did
not in any of 6, with or without 0008. It is fixed because it is a plain correctness bug.
