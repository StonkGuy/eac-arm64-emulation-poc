# Status

Last updated 2026-10-08. VRChat on Apple M2 / Asahi, patched FEX 2610 (developed on 2609.1). This is a one-game, one-machine proof of
concept — read "works" and "open" accordingly.

## Works
* EAC launcher completes (`Launcher finished with: 301 …`), `AntiCheat Session Begin: Success`, login, world join,
  rendering through DXVK, UI input, audio device enumeration.
* `tests/ptrace-inject` passes (28 checks). `tests/signal-mask` and `tests/signal-regs` pass on a Linux kernel
  (aarch64 builds) and under the patched FEX in the VM; on stock FEX they fail ([signal-registers.md](signal-registers.md)).
* **No Photon time-outs** with the signal-mask fix: 0 of 7 vs 4 of 7 control in the A/B, 0 in the 59 sessions run since
  (before the fix about one in two), and the Wine reproducer 0 of 6 vs 6 of 6 ([disconnects.md](disconnects.md)).
* **No pre-join stall** with `WINE_CPU_TOPOLOGY` reporting 16 CPUs (set by `scripts/set-launch-options.sh`): 0 of 6
  launches vs 5 of 8 default launches interleaved in the same VM boot, region lookup a flat 3 s. The stall — a ~60 s
  freeze at the Photon region lookup and a `Disconnecting` rejoin loop, ~8 % of launches long-run — is IL2CPP thread-pool
  starvation on a 4-vCPU guest, not a FEX or Wine bug ([disconnects.md](disconnects.md#the-pre-join-stall-il2cpp-thread-pool-starvation)).
  It is a configuration fix; the evidence behind the mechanism is in that section.

## Open
* **The guest can be OOM-killed** with a small `--mem`. The muvm guest kernel is stripped (no `zram` module, no
  `virtio_balloon` driver) and has **no swap**; its `/` is virtiofs onto the host disk, which is typically too full for a
  swapfile. VRChat's working set alone is ~4.7 GB anon, so a `--mem 7168` guest reaches its ceiling after world entry and
  the **guest's own OOM killer** kills `VRChat.exe` (`oom-kill: ... task=VRChat.exe`). With `--mem 8192` it holds, but
  available memory in-world is still only a few hundred MB — the 16 GB host + swap-less, balloon-less guest is genuinely
  tight. Never returned to the host either: with no balloon driver, freed guest pages stay resident in the VM process
  until it is restarted.
* **A host power clamp, not CPU contention** (bursts of ~4–5× slower work lasting ~8–18 s). **Measured and attributed** — a
  canary process (identical work, every second) takes **3.16× / 5.25× / 4.84× longer** to finish inside the three
  FPS-dip windows of one early capture while the SoC collapses from ~22.4 W to ~11.9 W (sys) and heat from ~12.8 W to ~5.4 W: the host stops
  doing work rather than competing for a running core, so this is a machine-wide clamp rather than guest CPU starvation.
  FEX's own counters are flat across a dip
  (`fexstats`: invalidation/SMC unchanged), memory is not implicated (`allocstall` = 0), and it is not GC. It is a
  host-side power-budget governor — it fires on **AC mains**, at only ~56 °C SMC, so it is not overtemperature
  but a sustained-power budget. Not an emulation artifact and not
  something FEX tuning can change.
* **Intermittent EAC-client crash** (1 of 5 launches early on): null singleton inside the client at world instantiate.
  Not seen since; the client has produced **one minidump** (`ACCESS_VIOLATION` reading `0x14`, `RIP` inside
  `EACCLIENT+0x297ac`), which is where the "null singleton" reading comes from — it is one event, not a pattern, so the
  cause stays unestablished. It has not recurred, and no crash-reporting hook is installed; see
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
* Only VRChat has been tested. Other EAC titles use the same launcher/client machinery but may add their own checks.
