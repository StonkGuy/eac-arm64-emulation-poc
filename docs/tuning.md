# Performance and memory tuning

A 16 GB machine is tight: the VM's RAM, the GPU buffers (unified memory), Steam's web helpers, the game (4–5 GB) and the
desktop all compete. The main constraint is host memory, not emulation speed: when it runs out, the kernel OOM-kills
the whole VM ("the game crashed"). Order of importance:

## 1. Do not run the host out of memory

| Knob | Where | Why |
|---|---|---|
| `muvm --mem 8192 --vram 4096` | `scripts/vm/steam-vm.sh` | with muvm's defaults the guest gets most of the host's RAM and half of it is reported as VRAM; Unity sizes its caches from that (`SystemInfo.graphicsMemorySize` was 7813 MB). The guest has **no swap** (stripped kernel: no `zram`, no `virtio_balloon`; `/` is virtiofs onto the host disk), so `--mem` below ~8192 lets the guest's own OOM killer kill `VRChat.exe` (~4.7 GB anon) once a world loads. |
| `--passt-args=-m1500` | `scripts/vm/steam-vm.sh` (`VM_MTU`) | the virtio NIC defaults to 65520 MTU against a 1500 path; passt's `-m` advertises 1500 over DHCP so eth0 comes up correct. Single argv token, so no shell quoting. |
| `DXVK_CONFIG_FILE=scripts/dxvk.conf` | launch options | caps the memory heaps DXVK reports. |
| `PROTON_USE_XALIA=0` | launch options | skips Proton's accessibility helper (~350 MB). |
| `vm.watermark_boost_factor=0` | `scripts/host-tune.sh` | the OOM report showed the min watermark boosted from 352 MB to 1.1 GB; boosting makes the kernel reclaim long before memory is short. |
| `vm.swappiness=60`, `vm.page-cluster=0` | `scripts/host-tune.sh` | Asahi's default of 10 throws away page cache before compressing cold anonymous memory. |
| 8 GB zram swap | `scripts/host-tune.sh` | zswap needs a swap *slot* per stored page; an 8 GB swapfile was full when the OOM killer fired. |
| NVMe `read_ahead_kb=256` | `scripts/host-tune.sh` | the distro value was 4096 (4 MB per read-ahead). |
| shut the VM down when idle | — | after the game exits the host keeps the pages the guest freed (balloon reporting is lazy): a VM with 2 GB in use still pinned 5–7 GB. |
| memory watchdog | `scripts/vm/oom-watch.sh` (auto-started by `steam-vm.sh`) | the guest has no swap, so its own OOM killer takes VRChat once memory is short ("joins then dies"). The watchdog posts a desktop notification at the guest and host low-water marks and reports an actual OOM kill. `VM_OOM_WATCH_POLL` (15 s), `OOM_WATCH_LOW_MB`/`CRIT_MB`/`HOST_MB`; `OOM_WATCH_NO_DESKTOP=1` prints only. |

Steam's web helpers (`steamwebhelper`) use ~2 GB inside the VM; `-no-browser` does not stop them with current Steam.

`sudo scripts/host-tune.sh status|apply|revert` — runtime only, undone by a reboot.

## 2. Input

The MacBook's trackpad has *disable while typing*, which freezes the pointer while W/A/S/D are held. Turn it off for the
session (KDE Plasma): `busctl --user set-property org.kde.KWin /org/kde/KWin/InputDevice/event0 org.kde.KWin.InputDevice disableWhileTyping b false`
(`event0` is the touchpad here; check `busctl --user tree org.kde.KWin`).

## 3. What the emulator spends its time on

`FEX_PROFILESTATS=1` makes every FEX process publish `/dev/shm/fex-<pid>-stats` (JIT time, signal time, SMC faults,
lookup-cache misses; 24 MHz ticks). `tools/fexstats.py` parses it. During VRChat's start-up (≈50 s of CPU) the
translator accounts for only ~8 s and there are no SMC storms, so *cold-code translation is not the bottleneck*.

Micro-benchmark (`tests/bench`, M2 P-core, FEX default config):

| test | distro FEX 2604 | patched 2609.1 | patched, `FEX_TSOENABLED=0` |
|---|---|---|---|
| cold translation of 55k instructions | 14 ms | 14 ms | 14 ms |
| 300M-iteration integer loop | 525 ms | 519 ms | 519 ms |
| 768 MB streaming copy | 43 ms | 40 ms | 33 ms |
| 40M dependent loads | 781 ms | 758 ms | 769 ms |
| 120M unpredictable branches | 693 ms | 689 ms | 690 ms |
| 150M SSE mul/add | 329 ms | 328 ms | 328 ms |
| 1M `getpid` | 149 ms | 153 ms | 128 ms |
| 50M seq-cst atomics | did not finish (minutes) | 237 ms | 237 ms |
| 100M indirect calls (4096 targets) | — | 2100 ms | — |

FEX enables Apple's *hardware* TSO mode by itself when the kernel offers `prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)`.
This works on the Asahi host and inside the muvm guest (checked with a 10-line C program on both), so x86 memory ordering
already costs nothing in the JIT output. `FEX_TSOENABLED=0` therefore only changes the cost of the rare helper paths
(`memcpy`-style ops, `getpid` above) and weakens the memory-ordering guarantees; keep the default.

## 4. Code invalidation with many threads

`mprotect` on a data page, 100k calls, 190 parked guest threads: stock FEX 790 ms, with `patches/0004` 32 ms (the same
as with no extra threads; ~25× on this `tests/bench` churn test, ~22× per call: 7.9 µs vs 0.35 µs). Games with hundreds of threads that churn memory mappings (Unity, Wine, DXVK, anti-cheat) pay
for this on every call.

## 5. Per-thread view while playing

Inside the VM, `/proc/<pid>/task/*/stat` gives per-thread CPU (Unity names its threads); `tools/harness/timeline.py` records it
twice a second together with the UDP receive queues, and `tools/harness/analyze_tl.py` correlates it with the HUD frame rate.
FEX's in-process sampling profiler (`patches/0005`, `tools/resolve_samples.py`) shows where the guest time goes without ptrace.
On a 4-vCPU guest the Unity main thread (≈75 %), Wine's `wine64` server thread (≈60 %) and a thread named `Security`
(≈35 %) dominate.

## 6. CPU count: VM cores and reported CPUs

**VM cores.** muvm gives the guest the host's performance cores only (4 on an M2). Adding efficiency cores was measured with
16 CPUs reported to the game in each case, on AC:

| VM cores (`muvm -c`) | time to world | FPS, high phase (p90) | share of time in the low phase |
|---|---|---|---|
| 4 P (default) | 25 s | 44.1 | 74 % |
| 4 P + 2 E | 26 s | 43.5 | 57 % |
| 4 P + 4 E | 47 s | 40.3 | 79 % |

Two efficiency cores change nothing measurable (the low-phase share follows the power state, see section 7); four make
loading almost twice as slow and lower the frame rate. Keep muvm's default.

**Reported CPUs.** `WINE_CPU_TOPOLOGY` (set by `scripts/set-launch-options.sh`) makes Windows code see more CPUs than the
VM has; it is what prevents the pre-join stall ([disconnects.md](disconnects.md#the-pre-join-stall-il2cpp-thread-pool-starvation)).
Reporting 6, 10 or 16 CPUs on the 4-core VM gave the same frame rate as the default 4 (42–45 FPS in the high phase of
every run), and worlds load at least as fast: a median of 25 s over 11 launches with 16 reported against
25–36 s with the default. 16 is the setting, since the thread pool that starves sizes itself from this number.

## 7. Host power and throttling

The frame-rate dips are **not** emulation. The frame rate alternates between ~44 FPS (for ~8–18 s) and ~19–20 FPS (for
~8–13 s), with a dominant period of ~39–44 s. A fixed-work probe (`tools/canary.c`, which prints the nanoseconds each
fixed unit of work takes, so a larger number means a slower core) pinned to a performance core next to the game runs
~4.1× slower in the low phase, while the SoC's system power falls to 0.67× and its heat output to 0.41× (the wall AC draw
stays constant). These are means over the later 240 s captures split by frame-rate mode; the per-dip figures in
[status.md](status.md) (3.2–5.3×, 22.4 → 11.9 W) come from three dips in an earlier capture and agree with them. FEX's own counters (`fexstats`: invalidation/SMC) are flat across a dip and no memory stall is involved,
so nothing in the guest or the translator is implicated. It is a **sub-OS hardware clamp**: the OS's frequency readings
look normal throughout. The trigger is the **power budget**, not temperature: it fires on AC, at only ~56 °C SMC, because
this machine is taking more than the adapter supplies and the firmware clamps it in bursts.

To check your own setup:

* `tools/check-power.py` loads every performance core and reports whether the charger/port/hub is strong enough (a MacBook
  Air M2 under a game wants ~25–30 W; the 30 W charger in the Mac's own port, or MagSafe, no hub and no PC USB port).
* `tools/powertl.py SECONDS OUT` records the SMC power/temperature timeline next to a run. Each line is `P <epoch>
  sys= ac= heat= acv= aci= batv= batw= batI= nand= chg= wifi= soc=`: `sys` is the total system power, `ac` the AC input
  power (staying constant across the dip is the signature), `heat` the heatpipe power and `soc` the battery state of
  charge; `acv`/`aci` are the charger input voltage/current, `batv`/`batw`/`batI` the battery voltage/power/current, and
  `nand`/`chg`/`wifi` temperature sensors.

macOS does closed-loop thermal control for the SoC; Linux does not, so this class of throttle is an OS/firmware behaviour you
have to manage yourself on Asahi. The reliable lever is the **power source**: a stronger supply (the Mac's own 30 W port or
MagSafe, no hub) raises the budget and reduces how often the clamp fires. A frame rate that swings between ~44 and ~20 FPS
with the same period as the power reading is the signature.

## Open issues

See [status.md](status.md) for what remains open and unverified.
