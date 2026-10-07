# Status

Last updated 2026-10-07. VRChat on Apple M2 / Asahi, patched FEX 2609.1. This is a one-game, one-machine proof of
concept — read "works" and "open" accordingly.

## Works
* EAC launcher completes (`Launcher finished with: 301 …`), `AntiCheat Session Begin: Success`, login, world join,
  rendering through DXVK, UI input, audio device enumeration.
* `tests/ptrace-inject` passes (28 checks). `tests/signal-mask` and `tests/signal-regs` pass on a Linux kernel
  (aarch64 builds) and under the patched FEX in the VM; on stock FEX they fail ([signal-registers.md](signal-registers.md)).
* **No Photon time-outs** in the sessions that ran with the signal-mask fix: 0 of 7 vs 4 of 7 control, and the Wine
  reproducer 0 of 6 vs 6 of 6 ([disconnects.md](disconnects.md)).

## Open
* **A pre-join stall** (a census of 99 sessions puts it at **~8 in 99 ≈ 8%**, not "1 in 5"; it arrives in batches, not
  uniformly). A silent ~60 s gap before the world's destination is fetched (healthy: ~1 s), then the region connect
  fails and the client retries every ~31 s. It has **two shapes** — most sessions stall *before* `Destination fetching`,
  one observed *between* `Destination fetching` and `Destination set` — and `current state: Disconnecting` is a perfect
  marker (exactly the stalling sessions print it; every other one opens with `ConnectedToNameServer`). It happens **with
  and without** the signal-mask fix, so it is a different failure. A traced capture (see
  [disconnects.md](disconnects.md)) shows it is **not a FEX bug**: the parked threads wait on an empty wineserver reply
  pipe, every futex-blocked thread has its futex word equal to the value it waits on (nobody woke it — no lost
  wake-up), `SIGUSR1` is fully delivered, and no 16-byte reply write to those pipes appears in the ring. FEX's futex is
  a raw host pass-through with no path that could swallow a pipe write, so the missing wake-up is server-side
  (wineserver/EOS) or environmental. The corpus cannot separate "lost completion" from "the peer never answered" — both
  leave the same footprint. Patch 0009 (`rt_sigsuspend` no longer host-blocks FEX's own signals) is kept as a
  defensible correctness fix but is **not** claimed to fix this; it was not A/B-tested against the stall.
* **CPU throttling on the test machine** (bursts of ~5× slower for 12–18 s). **Measured and attributed** — a canary
  process (identical work, every second) slows **3.16× / 5.25× / 4.84×** inside the three FPS-dip windows, while the
  SoC collapses from ~22.4 W to ~11.9 W (sys) and heat from ~12.8 W to ~5.4 W. FEX's own counters are flat across a dip
  (`fexstats`: invalidation/SMC unchanged), memory is not implicated (`allocstall` = 0), and it is not GC. It is a
  host-side power/thermal-budget governor — it fires on **AC mains**, at only ~56 °C SMC, so it is not overtemperature
  but a sustained-power budget. Not an emulation artifact and not something FEX tuning can change.
* **Intermittent EAC-client crash** (1 of 5 launches early on): null singleton inside the client at world instantiate.
  Not seen since; the client has produced **one minidump** (`ACCESS_VIOLATION` reading `0x14`, `RIP` inside
  `EACCLIENT+0x297ac`), which is where the "null singleton" reading comes from — it is one event, not a pattern, so the
  cause stays unestablished. A crash-reporting hook (`crashhandler`) was left off after that; see
  [disconnects.md](disconnects.md).
* **In-world video players** work for ordinary videos (an H.264/AAC MP4 resolved by the bundled `yt-dlp` played through
  AVPro/Media Foundation). One recurring failure is **not** a codec or emulation problem: the video used by the test
  world is a YouTube *live* stream that `yt-dlp` refuses ("This live stream recording is not available") on every client,
  so the launcher hands AVPro the raw page URL and it reports "codec not supported". The bundled `yt-dlp` equals the
  latest official release (2026.08.19); swapping it changes nothing. Livestream protocols (RTSP/HLS/MPEG-TS) would need a
  Proton-RTSP build — a separate, optional add-on.

## Unverified
* Patches 0002 (SMC hot pages) and 0003 (invalidation stats) were not A/B-tested for frame-rate effect.
* Whether `scripts/vm/realism.sh` (fake DMI/PCI/PID 1/hostname) is still needed now that `ptrace` is faithful.
  **Partly answered:** the **CPUID/hypervisor axis is not needed** to reach EAC's load step — sessions with no CPUID
  masking at all (hypervisor bit left set, `FEXIFEXIEMU` vendor visible) still completed
  `Launcher finished with: 301 …` 48 times. The **DMI/PCI axis is still unverified**: no controlled
  masked-vs-unmasked A/B was run for it, so it stays an open question, not a claim.
* Whether the signal-mask fix removes *every* real-game time-out (0 of 7 so far); more sessions would firm it up.
* Whether patch 0008 changes the pre-join stall rate.
* Only VRChat has been tested. Other EAC titles use the same launcher/client machinery but may add their own checks.
