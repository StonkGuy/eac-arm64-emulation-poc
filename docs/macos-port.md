# macOS port

**Status: measured.** VRChat with genuine Easy Anti-Cheat runs on an Apple M2 MacBook Air (16 GB, fanless, macOS 27.0)
in a libkrun guest, through the patched FEX-2610 from `patches/`. The deciding question was whether EAC accepts a
macOS-libkrun guest; it does. Three sessions on 2026-10-08 all reached world entry. Only this one Mac was tested.

## How it works

| layer | what | kind |
|---|---|---|
| host VMM | [steamac](https://github.com/fxgl/steamac) 1.8.1/1.8.2: libkrun 1.19.6 on Hypervisor.framework | outside this repository |
| guest | Valve's ARM64 SteamOS (Steam Frame image, stable 20260922.6101926), rootfs unmodified; Linux 7.2.9-steamac, 4 KB pages, 4 vCPUs (the M2's performance cores), 8 GB RAM | outside this repository |
| graphics | Venus (virtio-gpu) -> virglrenderer -> KosmicKrisp -> Metal | outside this repository |
| x86-64 translation | Steam's own FEX compatibility tool (Steam app 3127680) with Valve's FEX-2607-76-g37265b1 replaced by FEX-2610 + `patches/` (0001–0021), built in the guest with `-DBUILD_STEAM_SUPPORT=ON` | **our patch code** in `patches/`; the tool and Steam's swap mechanism are Valve's |
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

**Tests** (`tests/ptrace-inject`, `tests/signal-mask`, `tests/signal-regs`, run in the guest):

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
pipelines synchronously, so each new pipeline blocks the frame (50-100 ms typical; one took over 60 s). Metal caches
results on disk, so revisits are smooth.

## THE FINALS

The same guest and the same patched FEX were also pointed at **THE FINALS** (Steam 2073850, a UE5 title). **It does
not start yet, and the stop is not in FEX.** This is a record of where it stops, not a supported configuration.

Two measured facts set the frame. First, the title's anti-tamper is **Embark Theia** (in-process, user-mode: the
launcher `Discovery.exe` carries `theia_DEFAULT`/`PageGuardian` symbols and ships `Cerebro.dll` as its VM runtime),
plus **Denuvo Anti-Cheat** (`AntiCheatInstaller.exe --no-gui --install` writes the `DenuvoAntiCheat` registry key and
a `denuvo-anti-cheat.sys`). There is **no Easy Anti-Cheat** in this install: a recursive search finds no
`EasyAntiCheat` file, directory or string.

Second, the game never reaches its renderer. At the stop, the process has loaded 47 modules — all bootstrapper/runtime
(`kernel32`, `ucrtbase`, `winhttp`, `crypt32`, …) — and **no graphics module at all**: no `d3d12.dll`, `d3d11.dll`,
`dxgi.dll`, `vulkan` or DXVK/vkd3d module is loaded, and the install's own `D3D12/x64/D3D12Core.dll` is never
touched. No UE `Saved/` directory or log is ever created. Graphics is therefore ruled out as the cause; the failure is
inside Embark's pre-render bootstrapper and launcher, before any RHI choice (`-dx11` cannot help).

**The measured cause chain** (read-only, no debugger attached to the game):

| step | evidence |
|---|---|
| The game process is Theia-packed: the 317 MB `Discovery/Binaries/Win64/Discovery.exe` imports exactly one DLL, `preloader.dll` (which exports one symbol, `preloader_link_func`); `runtime.dll` sits alongside | static import tables |
| The bootstrapper mints a launch token and spawns the game child: `Discovery.exe -BOOTARGS="@db3sn4nfb4702c6nmj00"` | captured child command line |
| The child dies in the unpacked/`preloader` layer, at a `RIP` outside every loaded module (runtime-generated code), while the game never reaches UE5 init | Theia minidump: 13 modules, fault address in a `MEM_PRIVATE` region |
| Independently, the release build faults *inside FEX's own signal-frame setup*: `SetupFrame_x64` writes the guest signal frame to an unvalidated guest stack pointer that lands in a read-only host mapping (host `ld-linux`), so FEX takes a host `SIGSEGV` | release core: PC in `HandleDispatcherGuestSignal` (inlined `SetupFrame_x64`), fault at `[NewGuestSP+64]` |

The last row is the one FEX-side defect in the chain: `GuestFramesManagement.cpp:365` computes the frame destination
by arithmetic alone and stores with no probing and no fault-safe access. A signal delivered while the guest `RSP` is
not a live guest stack (the anti-tamper moving `RSP`, or the unpacker running on a perturbed frame) writes into a host
mapping and kills the process. The trigger is a signal storm from the anti-tamper workload; the *site* is unambiguous.

**What is fixed:** patch 0010 corrected the `SECCOMP_RET_TRAP`/`rt_sigreturn` re-execution loop that the game's
anti-tamper seccomp path drives (the 1000-traps-stable case in `tests/seccomp-trap` and `tests/seccomp-trap-noexec`):
the handler is entered at the instruction *after* the syscall and `rt_sigreturn` returns to the dispatcher top
instead of re-executing the trapped call, so the Wine `install_bpf` mutual-trap livelock the title's anti-tamper
drives is gone. Patch 0019 validates the signal-frame destination against the guest mapping table before the first
store and, when it cannot be written, kills the thread from `SIGSEGV` the way Linux's `force_sigsegv` does — the
last-row FEX defect is closed (`tests/signal-frame`, the "bad SP" case).

**What remains:** the wall that is not FEX's to fix. Theia's integrity check trips on the emulated environment itself,
before the renderer, and the Denuvo kernel driver cannot load under Wine. **Pending: whether Theia can pass at all on
this stack is not established.** A static analysis confirms the anti-tamper executes raw x86-64 `syscall`
instructions (31 direct-NT stubs in the launcher, each aggregating the kernel status word into a tamper flag) and
probes `IsDebuggerPresent`, `CheckRemoteDebuggerPresent` and `wine_get_version` — the class of check a real kernel
satisfies and emulation must too.

## Known issues and where they are handled

These are steamac-side issues, handled in the [steamac fork](https://github.com/StonkGuy/steamac) (branch `fex-eac`),
not in this repository ([fork notes](https://github.com/StonkGuy/steamac/blob/fex-eac/docs/fork.md)).

| issue | handling in the fork |
|---|---|
| shader-compile stalls | opt-in asynchronous pipeline compilation in KosmicKrisp (`MESA_KK_ASYNC_PIPELINES=1`, set in the launcher's environment); **experimental.** Its use-after-free fix is re-verified (repro exit 139 → 0, ASan clean) |
| microphone start can freeze the guest: steamac's CoreAudio virtio-snd backend starts capture synchronously under a shared lock; over 1 s gives a guest `virtio_snd` control-message time-out, PipeWire is killed and the game freezes. Without macOS microphone permission the capture stream floods the log instead | kernel parameter `virtio_snd.msg_timeout_ms=10000` (workaround) and libkrun patch 0017 (asynchronous capture start); re-verified |
| launcher present path and input | triple-buffered present, lower default log level, Metal HUD library loaded only when needed, direct-to-display presentation, key releases never dropped, HID off the main thread, Retina EDID DPI, a render-scale control (`--render-scale`, default off), plus the audit's stall/pad-port/`framebufferOnly`/Clipboard fixes — all built and re-verified (adversarial repros pass) |
| full-frame scanout copy per present | libkrun patch 0018 (copy only the damage owed to each frame); re-verified (0/256 trials stale) |

A code audit of steamac 1.8.2 also confirmed upstream issues this fork does not fix (kept out of the published bundle).

## Not tested

* Only VRChat, only an M2 MacBook Air, only macOS 27.0, three sessions. THE FINALS was run read-only and does not
  start (above).
* Steam Frame hardware. The Mac guest is the Steam Frame OS image with the same Steam FEX compatibility-tool
  mechanism, so the same swap is expected to apply there; this is unverified ([other-platforms.md](other-platforms.md)).

## Non-goals

No hooking, shimming, patching or replacing `start_protected_game.exe`; no `EOS_USE_ANTICHEATCLIENTNULL`; no hypervisor
masking. The stock, sanctioned Linux/Proton EAC client runs unmodified, and the results above were obtained without
`realism.sh` or any DMI masking. VRChat does not support virtual machines or emulation; see
[Scope and limits](../README.md#scope-and-limits).

See [platform-viability.md](platform-viability.md) for the background on the route.
