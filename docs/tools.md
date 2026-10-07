# Tools

Everything in `tools/` is plain Python 3 with no dependencies beyond the standard library and libX11/libXtst for the
two X11 helpers. They were written for the measurements in this repository; each has a usage line at the top of the file.

## Game measurements (run where the game's X display is, i.e. inside the VM)

| tool | what it does |
|---|---|
| `hudfps.py [seconds] [interval]` | Reads the DXVK HUD's `FPS: nn.n` from the game window with XGetImage and prints `epoch fps`; uses `xin.py` and the digit templates in `hud_templates.json` (set `DXVK_HUD=fps` in the launch options). |
| `xin.py` | Tiny XTEST input injector and screen grabber (ctypes over libX11/libXtst): `list`, `activate NAME`, `key`, `click`, `drag`, `grab`, ... Used to drive the game menu unattended. |
| `fexstats.py FILE…` | Parses the live statistics FEX publishes with `FEX_PROFILESTATS=1` (per-thread JIT/invalidation/SMC counters). |
| `resolve_samples.py TAG FEX_UNSTRIPPED` | Resolves the output of FEX's in-process sampling profiler (patch 0005) to host symbols and guest modules; see [how-it-works.md](how-it-works.md). |
| `mdparse.py` | Summarises a Windows minidump (exception record, faulting module+offset, registers, stack module hits), for crash dumps from the Unity crash handler. |

## Host power / throttle (run on the host, Asahi)

These make the *host* the thing under measurement. They exist to check the claim in [status.md](status.md) that the
frame-rate dips are a host power-budget throttle, and to test for it on your own machine.

| tool | what it does |
|---|---|
| `canary.c` | A fixed-work CPU probe: a dependent integer chain timed every 0.5 s. Run it pinned to a performance core next to the game (`taskset -c 6 ./canary 120`) and its printed nanoseconds-per-run show directly when the core slows down. Build: `gcc -O1 -o canary canary.c`. |
| `powertl.py SECONDS OUTFILE` | Logs the Apple SMC's power/voltage/current/temperature readings twice a second on Asahi (`macsmc_hwmon` + battery). |
| `check-power.py [SECONDS]` | Loads every performance core for ~40 s and prints a verdict: whether the power source (charger/hub/port) is strong enough, and whether the SoC throttles to the adapter limit under load. |

`scripts/cpu-power.sh` is the *policy* side (root, runtime only): `status`, `cap MHZ` (limit the performance cluster's
maximum clock for a fanless machine), `governor NAME`, `revert`. See [tuning.md](tuning.md).

## Wedge and disconnect analysis (`tools/harness/`)

| tool | what it does |
|---|---|
| `timeline.py SECONDS OUTFILE` | Inside the VM: every 0.5 s per-thread CPU ticks and state of the game, UDP receive queues and system counters. |
| `hosttl.py SECONDS OUTFILE` | On the host: memory, reclaim and pressure counters and the CPU time of the VM's threads. |
| `analyze_tl.py LABEL` | Correlates the HUD FPS with per-thread CPU, pressure counters and the UDP backlog from the recordings above. |
| `wedgewatch.py LABEL SECONDS` | Inside the VM as root: watches a running game for a wedge (UDP backlog > 64 KB for 6 s, or the main thread asleep for 25 s) and at that moment triggers a FEX thread snapshot (patch 0006) and copies the snapshot, `maps`, thread names, per-thread syscalls, wineserver's epoll state, a passive `/proc/<wineserver>/{syscall,wchan}` probe and the TCP/UDP socket state. Needs `FEX_PROFILESAMPLEHZ=1` in the game's environment; with `FEX_SIGNALTRACE=1` it also gzips the FEX trace rings. |
| `resolve_snap.py SNAP MAPS [NAMES]` | Turns a snapshot into a per-thread listing: the x86 syscall each thread is blocked in with its arguments (for futex waits also the futex word against the expected value), and the return addresses on its stack as `module!export+offset`. Auto-detects the 640- or 1280-byte record size (patch 0006 writes 80 words; later builds add the futex-word diagnostics and write 160). |
| `sigtrace.py GAME_RING [SERVER_RING]` | Decodes `FEX_SIGNALTRACE=1` rings and follows a signal from the sender's `kill`/`tgkill` to the receiver's guest handler. |
| `wakeflow.py RING [--tid T] [--fd F]` | Decodes a `FEX_SIGNALTRACE=1` ring's 16-byte wake-up traffic (`Read16`/`Write16`, the first 8 bytes are Wine's wait cookie) and `epoll_ctl` calls, and flags a 16-byte write whose cookie is never read back — a reply the server wrote that the waiter did not consume. This is how the "server never answered the parked thread" finding was made. |
| `classify_runs.py PREFIX…` | Classifies saved game logs (time-out, hang, ok, short) per session and arm and runs Fisher's exact test between arms. |
| `wedge_repro.py` | Standalone Windows-Python program that recreates the wedge without the game (see [disconnects.md](disconnects.md)). |

## Building the tests

`static-link.py` is a minimal linker for one freestanding x86-64 object. It exists so that the programs in `tests/` can be built
on an aarch64 machine that has a clang with the x86-64 target but no x86-64 linker.
