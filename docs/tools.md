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
| `tests/bench/build.sh [clang] [output]` | Freestanding x86-64 micro-benchmarks: the code-churn invalidation cost (`EXTRA="-DINVAL_MODE=1 -DINVAL_THREADS=190"`, patch 0004) and the self-modifying-code fault modes (patch 0002). Generates its bytecode table with `gen_bigcode.py`. |
| `tests/bench/tso_probe.c` | A tiny probe for the `prctl(PR_GET/SET_MEM_MODEL)` interface FEX uses for TSO; prints what the kernel/FEX report. Unlike the other tests it is an ordinary aarch64 program: `gcc -O2 -o tso_probe tests/bench/tso_probe.c`, then run it on the host and inside the VM.
