# macOS port

**Status: measured.** VRChat with genuine Easy Anti-Cheat runs on an Apple M2 MacBook Air (16 GB, fanless, macOS 27.0)
in a libkrun guest, through the patched FEX-2610 from [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) (tag `eac-2610.4`). The deciding question was whether EAC accepts a
macOS-libkrun guest; it does. Three sessions on 2026-10-08 all reached world entry. Only this one Mac was tested.

## Current route

| item | state |
|---|---|
| app | [StonkGuy/steamac](https://github.com/StonkGuy/steamac), a fork of steamac 1.9: `main` and release [v1.9-eac.1](https://github.com/StonkGuy/steamac/releases/tag/v1.9-eac.1) (ad-hoc signed DMG, GPL sources attached) for players, `fex-eac` for development |
| FEX | the same patched build as here, pinned to tag `eac-2610.4` by the fork's `fex-eac/build-fex.sh`, installed into Steam's FEX tool in the guest |
| graphics | two paths, chosen in the app: *Vulkan* (DXVK -> Venus -> KosmicKrisp, upstream steamac's path) and *DXMT* (Direct3D 11/10 through [DXMT](https://github.com/3Shain/dxmt), its Metal calls run on the Mac over a dedicated virtio-gpu context) |
| VRChat with EAC | `301`, session and world on both paths; about 29-44 fps on Vulkan and 76-78 fps on DXMT on the same M2 (single runs, title-specific) |
| VR | experimental, on the fork's `vr-polish` branch (Quest 3 over WiVRn) |

The measurements in the rest of this page were taken on steamac 1.8.1/1.8.2 on the Vulkan path, before the DXMT path
existed. Current numbers and limits are in the fork's
[docs/eac-performance.md](https://github.com/StonkGuy/steamac/blob/main/docs/eac-performance.md).

## How it works

| layer | what | kind |
|---|---|---|
| host VMM | [steamac](https://github.com/fxgl/steamac) 1.8.1/1.8.2: libkrun 1.19.6 on Hypervisor.framework | outside this repository |
| guest | Valve's ARM64 SteamOS (Steam Frame image, stable 20260922.6101926), rootfs unmodified; Linux 7.2.9-steamac, 4 KB pages, 4 vCPUs (the M2's performance cores), 8 GB RAM | outside this repository |
| graphics | Venus (virtio-gpu) -> virglrenderer -> KosmicKrisp -> Metal | outside this repository |
| x86-64 translation | Steam's own FEX compatibility tool (Steam app 3127680) with Valve's FEX-2607-76-g37265b1 replaced by FEX-2610 + patches 0001–0026, 0033, 0035–0039 and 0041–0070 ([StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610) tag `eac-2610.4`), built in the guest with `-DBUILD_STEAM_SUPPORT=ON` | **our patch code** in `patches/`; the tool and Steam's swap mechanism are Valve's |
| game | the Windows depot (build 25738324) under x86-64 Proton Experimental (Wine 11.0) | stock |

Steam on ARM64 installs VRChat's Android build by default; forcing the compatibility tool to Proton Experimental
selects the Windows depot. The chain seen in Steam's console is `fex-compat-tool run` -> Steam Linux Runtime ->
`Proton - Experimental`, so Proton runs inside Steam's FEX tool and the swapped-in FEX is what executes it. The swap
replaces the tool's binaries, `FEXCompatTool`, `VERSIONS.txt` and `ConfigTemplate.json`; Valve's copies are backed up
and one command restores them. FEX-2610's Steam config template enables DiskCache (a stock FEX option). The image
ships clang 19, cmake, ninja, lld and glibc 2.39, so FEX builds natively in the guest.

The launch options are the stock Proton settings from [setup-asahi.md](setup-asahi.md) (`EAC_LAUNCHERDIR`,
`PROTON_EAC_RUNTIME`, `WINEDEBUG=-all`, `PROTON_USE_XALIA=0`, `WINE_CPU_TOPOLOGY=16:...` for the pre-join stall). The
Proton EasyAntiCheat Runtime (1826330) must be installed explicitly. Setup, scripts and exact launch options:
[`fex-eac/README.md`](https://github.com/StonkGuy/steamac/blob/fex-eac/fex-eac/README.md) in the
[steamac fork](https://github.com/StonkGuy/steamac) (branch `fex-eac`).

## What was verified

**Hardware TSO works inside the Hypervisor.framework guest.** The guest kernel logs `CPU features: detected: TSO
memory model (Apple)`; `prctl(PR_SET_MEM_MODEL, TSO)` succeeds and reads back 1; `FEXGetConfig --tso-emulation-info`
reports Hardware TSO for GPR, memcpy and vector memory ordering. `getconf PAGESIZE` is 4096.

**Tests** (`eac-tests/ptrace-inject`, `eac-tests/signal-mask`, `eac-tests/signal-regs`, run in the guest):

| FEX | ptrace-inject | signal-mask | signal-regs |
|---|---|---|---|
| patched FEX-2610 (also the Steam-mode build) | PASS | PASS | PASS |
| stock FEX-2610 | hangs after `PASS: fork` | FAIL | FAIL (tgkill/kill/tkill) |
| Valve's FEX-2607-76 | hangs after `PASS: fork` | FAIL | FAIL (tgkill/kill/tkill) |

Stock and Valve's builds fail exactly as they do on Asahi.

**EAC and the game** (three sessions): the launcher reports `System name 'linux64'`, bootstrapper 1.9.3, `Starting
Wine module mapping`, then `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` within about
5 s, every session. The game logs `[AntiCheatClient] ... bound`, `AntiCheat Session Begin: Success` and `Finished
entering world`, with no `null client`. `scripts/vm/realism.sh` and DMI masking were not used.

| measurement | result |
|---|---|
| region lookup (`Locating best region` -> `Got best network region`) | 4-6 s, no pre-join stall |
| join -> world, first run (cold shader cache) | 86 s |
| join -> world, second run (warm caches and FEX DiskCache) | 32 s |
| Photon time-outs in the minutes after world entry | none |
| FEX micro-benchmark, 50M seq-cst atomics | 219-233 ms (Asahi: 237 ms) |
| FEX micro-benchmark, cold translation | 14 ms |

## Performance and its limits

The game is **GPU-bound**. `powermetrics` shows the GPU 100% active at about 1.21 GHz and ~3.6 W: the chip is
power-limited and its top GPU state (1.40 GHz) is not reached. The game renders 1280x800 and holds ~26-30 fps steady in
a light world. The high phase is 20-30% below the Asahi result on the same M2 (native driver there), but without
Asahi's deep dips. The CPU is not saturated (game main thread ~23% of a core, vCPUs ~55% each), and the FEX
micro-benchmarks match Asahi, so emulation is not the limit.

**Shader-compile stalls.** Venus exposes no `VK_EXT_graphics_pipeline_library` and KosmicKrisp compiles Metal
pipelines synchronously, so each new pipeline blocks the frame (50-100 ms typical; a single compile has been observed
over 60 s). The GPU goes fully idle while the host compiles: the fork's stall monitor logged a **349 s** GPU-idle stall
at world load (`Warming shaders`), with two more of 211 s and 217 s, and runtime stalls of 5.5-6.5 s whenever a new
area or menu brings up shaders.

The stall is not made shorter by cache reuse, because **the pipeline caches do not persist across runs**: after a
session the DXVK state cache and the Steam FOSSILIZE bucket (`fozpipelinesv6`) are still empty and the Venus/Mesa
shader cache holds a 157-byte header, so every launch recompiles from scratch. That is why the stall profile swings
between launches, and it is the dominant cause of the reported freezes. Memory pressure (zram swap, ~1.6 GB in use on
the 16 GB host) is a secondary contributor; the CPU is not the limiter (the emulator is not involved).

## THE FINALS

THE FINALS (Steam 2073850, a UE5 title) reaches its main menu and plays matches on this setup with `eac-2610.4`, on the
steamac fork's DXMT graphics path. Its protection is **Embark Theia** (user-mode, in-process) together with **Denuvo
Anti-Cheat**; the install has no Easy Anti-Cheat.

* What it needed from FEX is group I of [patches.md](patches.md) (0054–0070): its anti-tamper code probes exceptions
  that stock FEX did not raise like a CPU (IRETQ with NT set, MOV to FS/GS, instructions over 15 bytes, XSETBV, the
  address of a fetch fault), and a freeze in matches came from FEX's decoder reading guest code that the protection had
  just re-protected (0065).
* Launch options: `FEX_NEEDSSECCOMP=1 %command%` (a stock FEX option; Wine installs a seccomp filter for the title's
  syscall traps).
* Graphics: the DXMT path in a VM window. On the Vulkan (DXVK) path one run lost the device about 95 s into the game.
* Keep Wine logging light: channels such as `+seh` or `+sync` slow the protection's own code past its watchdog, which
  looks like a stall.
* Steam's idle screensaver took focus from the game a few minutes after launch, about when the menu appears, so menu
  clicks went to Steam. The steamac fork turns it off (see below).

## Known issues and where they are handled

These are steamac-side issues, handled in the [steamac fork](https://github.com/StonkGuy/steamac) (branch `fex-eac`),
not in this repository ([fork notes](https://github.com/StonkGuy/steamac/blob/fex-eac/docs/fork.md)).

| issue | handling in the fork |
|---|---|
| shader-compile stalls | opt-in asynchronous pipeline compilation in KosmicKrisp (`MESA_KK_ASYNC_PIPELINES`, off by default and not recommended, set in the launcher's environment). It crashed the render encoder under VRChat; KosmicKrisp patch 0047 addresses that crash in code, but no live run on record includes 0047. An idle in-world run with `=1` was clearly worse than off, so no stutter improvement is claimed |
| microphone start can freeze the guest: steamac's CoreAudio virtio-snd backend starts capture synchronously under a shared lock; over 1 s gives a guest `virtio_snd` control-message time-out, PipeWire is killed and the game freezes. Without macOS microphone permission the capture stream floods the log instead | kernel parameter `virtio_snd.msg_timeout_ms=10000` (workaround) and libkrun patch 0017 (asynchronous capture start); re-verified |
| launcher present path and input | triple-buffered present, lower default log level, Metal HUD library loaded only when needed, direct-to-display presentation, key releases never dropped, HID off the main thread, Retina EDID DPI, a render-scale control (`--render-scale`, default off), plus the audit's stall/pad-port/`framebufferOnly`/Clipboard fixes — all built and re-verified (adversarial repros pass) |
| full-frame scanout copy per present | libkrun patch 0018 (copy only the damage owed to each frame); re-verified (0/256 trials stale) |
| Steam's idle screensaver: Steam does not count mouse and keyboard input routed to a game, so after its idle timeout its screensaver takes focus from the game and clicks go to Steam | guest `steam-idle-screensaver` turns it off once per Steam installation (a screensaver chosen later in Steam's settings stays); verified live with THE FINALS |

A code audit of steamac 1.8.2 also confirmed upstream issues this fork does not fix (kept out of the published bundle).

## Not tested

* Only VRChat and THE FINALS, only an M2 MacBook Air, only macOS 27.0.
* Steam Frame hardware. The Mac guest is the Steam Frame OS image with the same Steam FEX compatibility-tool
  mechanism, so the same swap is expected to apply there; this is unverified ([other-platforms.md](other-platforms.md)).

## Non-goals

No hooking, shimming, patching or replacing `start_protected_game.exe`; no `EOS_USE_ANTICHEATCLIENTNULL`; no hypervisor
masking. The stock, sanctioned Linux/Proton EAC client runs unmodified, and the results above were obtained without
`realism.sh` or any DMI masking. VRChat does not support virtual machines or emulation; see
[Scope and limits](../README.md#scope-and-limits).

See [platform-viability.md](platform-viability.md) for the background on the route.
