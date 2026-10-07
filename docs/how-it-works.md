# How it works

Two things stand between VRChat (a Windows/Unity game that ships Epic's Easy Anti-Cheat for Linux/Proton) and an
arm64 machine running it through FEX-Emu:

1. the EAC *launcher* needs a faithful x86-64 `ptrace` view of the process it injects into, and
2. the injected EAC *client* constantly patches code in its own process, which stresses FEX's self-modifying-code (SMC)
   handling.

Everything here is about emulation fidelity. The anti-cheat runs unmodified and produces its own verdict
(`Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'`, later
`AntiCheat Session Begin: Success` in VRChat's log). Nothing in this repository fakes, replays or short-circuits that
result, and no Epic or VRChat binaries are included.

## What the Proton EAC launcher does (observed, not decompiled)

The sequence below was recorded on a native x86-64 Fedora machine with `trace-cmd` raw syscall tracing and then
compared, step by step, with what happened under FEX.

1. `start_protected_game.exe` (a Wine process hosting the EOS launcher module) forks a child. The child calls
   `prctl(PR_SET_DUMPABLE, 0)`, `ptrace(PTRACE_TRACEME)` and stops itself with `SIGSTOP`.
2. The parent (tracer) `wait4`s, sets `PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACEEXEC`, and single-steps the child's
   syscalls with `PTRACE_SYSCALL`, reading registers (`PTRACE_GETREGSET`, `NT_PRSTATUS`) at some stops.
3. At the child's `memfd_create` the tracer lets it run (`PTRACE_CONT`). The child writes the in-game client (an ELF
   shared object, ~6.8 MB) into the memfd and `execve`s the game's `wine64-preloader`.
4. The tracer receives the `PTRACE_EVENT_EXEC` stop, reads `/proc/<pid>/exe`, `PTRACE_GETSIGINFO`, then steps ~30 more
   syscalls through the preloader start-up.
5. Once the preloader has mapped the main executable the tracer reads its ELF header (`PTRACE_PEEKDATA`), plants an
   `int3` at `_start` with `PTRACE_POKEDATA`, continues, catches the `SIGTRAP`, fixes `rip` with `SETREGSET` and restores
   the original bytes.
6. Injection: the tracer walks the target's `ld.so`/libc data structures with ~300 `PEEKDATA`s to find a `dlopen`-like
   function, writes `"/proc/self/fd/3"` and a bogus return address (9) onto the child's stack, points the registers at the
   function with `SETREGSET`, continues, and catches the resulting `SIGSEGV` at the bogus return address. That loads the
   client from the memfd into the game. It repeats this for the client's init call (passing a parameter block), restores
   the registers and finally calls `PTRACE_DETACH`.

In total this is ~530 ptrace calls and ~1 s natively.

## Why this does not work out of the box under FEX

A FEX guest's syscalls are not host syscalls. When tracer and tracee are both FEX guests the host kernel only sees two
aarch64 processes running FEX:

* `PTRACE_SYSCALL` stops would be host-syscall stops of FEX itself, with aarch64 registers and numbers;
* `PTRACE_GETREGSET` returns an aarch64 `user_pt_regs`;
* a guest `int3` is a host `SIGTRAP` with FEX-internal state, not a stop the tracer can resume at a chosen `rip`;
* the child clears its dumpable flag, so the tracer cannot read its `/proc/<pid>/maps` or poke its memory through the
  host before the exec;
* `/proc/<pid>/exe` of the exec'd child points at `FEX`, not at the guest binary;
* memory written with `POKEDATA` is invisible to FEX's translated-code cache.

Stock FEX handles a small passthrough subset of ptrace (enough for Ubisoft's Wine launcher) and rejects the rest.

## The emulation (`patches/0001`)

`PtraceEmu.{h,cpp}` in `Source/Tools/LinuxEmulation/LinuxSyscalls/`:

* **State block.** Each process has a `SharedState` (magic, syscall-stops flag, "attached" flag, pending stop, an x86-64
  `user_regs_struct` image, dirty mask, flush range, exec path). A `PTRACE_TRACEME` child publishes it as a
  file-backed `MAP_SHARED` page under `/dev/shm` so the tracer can reach it while the child is non-dumpable; after the
  child's `execve` the new FEX image's static block is found through `/proc/<pid>/maps` (same binary, so the offset is
  known) and accessed with host `PEEK/POKE`.
* **Syscall stops.** `HandleSyscallImpl` reports entry and exit when the tracer resumed with `PTRACE_SYSCALL`
  (x86-64 number in `orig_rax`, `-ENOSYS` in `rax` at entry, arguments, real result at exit). The tracee parks itself in a
  host `SIGSTOP`; the tracer's `wait4` wrapper rewrites that stop into `SIGTRAP|0x80`.
  The tracer may rewrite `orig_rax`/`rax` at entry (changes the syscall that runs) or `rax` at exit (forges the result).
* **Signal stops.** Guest `int3`/`SIGSEGV` raised while traced become signal-delivery stops with x86-64 registers
  (`rip` after the `int3`, like Linux). The tracer's `SETREGSET` is applied when the tracee resumes and the signal is
  suppressed, which is how the injector resumes at the saved `rip` after its fake-return `SIGSEGV`.
* **Requests.** `GETREGS/SETREGS`, `GETREGSET/SETREGSET` (`NT_PRSTATUS`, plus a shadow `NT_PRFPREG`), `PEEKUSER/POKEUSER`
  into the register image, `POKETEXT/POKEDATA` with code-cache invalidation, `SETOPTIONS`, `GETSIGINFO`, `CONT`,
  `SYSCALL`, `DETACH` (which switches the emulation off in the tracee).
* **`/proc/<pid>/exe`.** `execve` of an attached process records the guest path in the state block; the tracer picks it
  up at `PTRACE_EVENT_EXEC` and `readlink` of `/proc/<pid>/exe` serves it.
* **Safety.** A tracee never parks if no host tracer is attached any more; forked children (without `CLONE_VM`) do not
  inherit tracing.

Limitations: only the main thread of a tracee is traced; register reads/writes work at the stops FEX itself produces,
not at a real `kill(self, SIGSTOP)` stop; `PTRACE_ATTACH` to an already running FEX process is not emulated.
`tests/ptrace-inject` is a freestanding x86-64 program that performs the whole conversation above and must print
`RESULT: PASS` both natively and under FEX.

## SMC hot pages (`patches/0002`)

The injected client keeps toggling page protections and writing into pages that also contain code it executes (trampolines
for its inline hooks, state next to code). With `SMCChecks=mtrack` each write to such a page costs a `SIGSEGV`, a code
invalidation and a re-translation. `SMCHotPages.h` counts faults per page; after `SMCHotPageFaults` (default 32) the
page is left writable and the JIT emits the per-instruction `SMCChecks=full` CRC check for instructions translated from
it, so correctness is preserved and the storm stops. Setting `FEX_SMCHOTPAGEFAULTS=0` restores the stock FEX behaviour.

## Not part of the emulation: environment realism

VRChat's guide "Using VRChat in a Virtual Machine" documents that EAC's VM block is its CPUID hypervisor-vendor check
and lists the hardware data to make look real (SMBIOS/DMI strings, PCI devices, hostname), adding that it does not mind
people doing this in some cases. Because the arm64 setup runs Steam inside a microVM (muvm), `scripts/vm/realism.sh`
ships an *optional* module that exposes plausible DMI/PCI/hostname data to the guest.

Two things to keep straight about it:

* It is **environment adaptation, not emulation-fidelity work** — the same category as the guide, and unlike the FEX
  patches it does not change how faithfully guest code runs. It does not touch the anti-cheat or its result.
* Whether it is **still needed once the ptrace emulation is faithful has not been verified** (see `docs/status.md`),
  and it is not needed to reproduce the emulation work.

The upstream fact that makes this a separate question: **VRChat's own guide reports that EAC's VM block is a check on
the environment, not an absolute one**, and that the recognised-environment route can work ("in some cases… we don't
mind"). So "EAC refuses VMs" is more precisely "EAC's default check refuses a VM it does not recognise." Two cautions
on how far to read that: the quote is from VRChat about EAC's *check*, not a statement that Epic supports VMs — Epic's
documentation still says the Anti-Cheat Client interface "does not support virtual machines" with no Linux exception —
and the cloud-VM cases (VRChat runs on GeForce NOW's cloud VMs) are publisher/provider arrangements, not evidence that
EAC tolerates arbitrary user VMs. VRChat does not support VMs; you can be banned.

## Cheap invalidation for many threads (`patches/0003`, `0004`)

Every guest `mmap`/`munmap`/`mprotect` ends in FEX's code-range invalidation, which used to visit every thread (lock the
shared lookup cache, search the thread's page map, reset its executable-range cache). With ~200 threads that made an
`mprotect` of a plain data page 22x slower (7.9 µs vs 0.35 µs) and let a process that changes protections in bulk stall all
its other threads behind the invalidation locks. `0003` adds live stats (`FEX_PROFILESTATS=1`: invalidation count, cycles
and dropped call-return stacks per thread); `0004` skips the per-thread walk when no code buffer had translated code in the
range and invalidates the per-thread executable-range caches lazily with an epoch. `tests/bench` has the churn test
(`EXTRA="-DINVAL_MODE=1 -DINVAL_THREADS=190" ./build.sh`).

## Profiling a game that detects debuggers (`patches/0005`)

Attaching `ptrace`/`perf` to a process running Easy Anti-Cheat is detected at once ("Forbidden system configuration
(Debugger detected.)") and must not be attempted. `FEX_PROFILESAMPLEHZ=<hz>` (with `FEX_PROFILESAMPLEALLTHREADS=1` and
`FEX_LIBRARYJITNAMING=1`) starts an in-process sampler once the process has more than 100 guest threads; create
`/dev/shm/fex-<pid>-sample-on` inside the VM to sample, delete it to stop, then resolve
`/dev/shm/fex-<pid>-samples` with `tools/resolve_samples.py` (needs `/tmp/perf-<pid>.map`, the process's `maps` and an
unstripped FEX binary).

What the samples mean: in all-threads mode only threads whose CPU time advanced since the previous round are signalled
(a process has hundreds of idle threads), so the result describes what *recently busy* threads were doing at the
instant of the signal, not wall-clock shares. In a first run, most of the Unity main thread's samples sat in the
`futex_waitv`/`futex` wrappers (it alternates short bursts of work with waiting for worker and render threads) and about
a third in translated code. The perf map names code by the file it was mapped from, so the game's own modules do not
show up: Wine reads PE images into anonymous memory when the section alignment does not fit the host's 16 KB pages.

### Thread snapshot

For a thread that is blocked rather than busy (a wedged receiver, say) the sampler is blind, because it only signals threads that
used CPU. `touch /dev/shm/fex-<pid>-snapshot` makes the profiler signal *every* thread once; each handler stores the thread's
x86 registers (so the syscall number and arguments it is blocked in) and 480 bytes of its guest stack into
`/dev/shm/fex-<pid>-snap`. `tools/harness/resolve_snap.py SNAP MAPS [NAMES]` decodes the syscall and walks the stack words that point
into executable mappings, naming them `module!export+offset` for the ELF and PE files it can read. Like the sampler it needs
`FEX_PROFILESAMPLEHZ=1` (any value > 0) in the process's environment and ≥ 100 guest threads, and it uses no ptrace.

## Signal masks (`patches/0007`)

A guest signal handler must run with `interrupted mask | sa_mask | signal` blocked and `rt_sigreturn` must put the interrupted mask
back, because code in handlers saves and restores "the current mask" (Wine does it around every server call, including inside
its `SIGUSR1` thread-suspension handler). Stock FEX blocks only `sa_mask | signal` and never updates its guest-visible mask
(`SignalInfo.CurrentSignalMask`) at handler entry or return. `FEX_SIGNALMASKFIX` (on by default; `=0` restores the stock behaviour) stores the guest mask in the context backup
on entry, applies the Linux rule to both the guest-visible and the host mask, restores the saved mask at `rt_sigreturn` and
re-raises signals that were only recorded as pending and became unmasked. `FEX_SIGNALTRACE=1` adds a lock-free in-memory trace
of the signal paths (`/dev/shm/fex-<pid>-sigtrace`, decoded by `tools/harness/sigtrace.py`). `tests/signal-mask` checks the mask
semantics with a freestanding program (all checks pass on a Linux kernel and on the patched FEX; checks 1, 3 and 4b fail with `FEX_SIGNALMASKFIX=0`).
Evidence and the reproducer: [disconnects.md](disconnects.md).

## Registers inside signal handlers (`patches/0008`)

A guest signal handler that starts while its thread is inside a system call (every `tgkill`/`kill`/`tkill` to self, and every
signal that finds a thread parked in `futex` or `read`) used to run with `Frame->InSyscallInfo == 0xFFFF`, the JIT's marker for
"all registers are already spilled to the frame" left behind by the interrupted `Syscall` op. The SMC write-fault handler and a
second signal arriving in translated code trust that marker, spill nothing, and redispatch from stale frame contents, so the
handler's own register changes are reverted. The patch clears the marker for the handler and puts it back at `rt_sigreturn`.
`tests/signal-regs` is the regression test; the analysis is in [signal-registers.md](signal-registers.md).
