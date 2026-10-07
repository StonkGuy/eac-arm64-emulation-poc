# Performance and memory tuning

A 16 GB machine is tight: the VM's RAM, the GPU buffers (unified memory), Steam's web helpers, the game (4–5 GB) and the
desktop all compete. The first thing that went wrong was *not* emulation speed but the host running out of memory:
the kernel OOM-killed the whole VM ("the game crashed"). Order of importance:

## 1. Do not run the host out of memory

| Knob | Where | Why |
|---|---|---|
| `muvm --mem 8192 --vram 4096` | `scripts/vm/steam-vm.sh` | muvm defaults to 80 % of RAM for the guest and 50 % of RAM reported as VRAM; Unity sizes its caches from that (`SystemInfo.graphicsMemorySize` was 7813 MB). |
| `DXVK_CONFIG_FILE=scripts/dxvk.conf` | launch options | caps the memory heaps DXVK reports. |
| `PROTON_USE_XALIA=0` | launch options | skips Proton's accessibility helper (~350 MB). |
| `vm.watermark_boost_factor=0` | `scripts/host-tune.sh` | the OOM report showed the min watermark boosted from 352 MB to 1.1 GB; boosting makes the kernel reclaim long before memory is short. |
| `vm.swappiness=60`, `vm.page-cluster=0` | `scripts/host-tune.sh` | Asahi's default of 10 throws away page cache before compressing cold anonymous memory. |
| 8 GB zram swap | `scripts/host-tune.sh` | zswap needs a swap *slot* per stored page; an 8 GB swapfile was full when the OOM killer fired. |
| NVMe `read_ahead_kb=256` | `scripts/host-tune.sh` | the distro value was 4096 (4 MB per read-ahead). |
| shut the VM down when idle | — | after the game exits the host keeps the pages the guest freed (balloon reporting is lazy): a VM with 2 GB in use still pinned 5–7 GB. |

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

FEX cache/JIT options on the indirect-call test (patched 2609.1): default 2100 ms, `FEX_DISABLEL2CACHE=0` 2021 ms (-4 %),
`FEX_DYNAMICL1CACHE=0` 2111 ms, `FEX_MULTIBLOCK=0` 2849 ms (+36 %). None of them is a big lever.

FEX enables Apple's *hardware* TSO mode by itself when the kernel offers `prctl(PR_SET_MEM_MODEL, PR_SET_MEM_MODEL_TSO)`.
This works on the Asahi host and inside the muvm guest (checked with a 10-line C program on both), so x86 memory ordering
already costs nothing in the JIT output. `FEX_TSOENABLED=0` therefore only changes the cost of the rare helper paths
(`memcpy`-style ops, `getpid` above) and weakens the memory-ordering guarantees; it is not recommended.

## 4. Code invalidation with many threads

`mprotect` on a data page, 100k calls, 190 parked guest threads: stock FEX 790 ms, with `patches/0004` 32 ms (the same
as with no extra threads). Games with hundreds of threads that churn memory mappings (Unity, Wine, DXVK, anti-cheat) pay
for this on every call.

## 5. Per-thread view while playing

Inside the VM, `/proc/<pid>/task/*/stat` gives per-thread CPU (Unity names its threads); `tools/harness/timeline.py` records it
twice a second together with the UDP receive queues, and `tools/harness/analyze_tl.py` correlates it with the HUD frame rate.
FEX's in-process sampling profiler (`patches/0005`, `tools/resolve_samples.py`) shows where the guest time goes without ptrace.
On a 4-vCPU guest the Unity main thread (≈75 %), Wine's `wine64` server thread (≈60 %) and a thread named `Security`
(≈35 %) dominate.

## Open issues

See [status.md](status.md): the start-up hang.
