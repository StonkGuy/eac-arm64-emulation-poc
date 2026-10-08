# Could this work on macOS?

Short answer: **not today**, and the reason is specific rather than general. The naive three-slot decomposition —
translate DirectX, translate x86-64, emulate the Windows API — is correct, and macOS has good answers for two of the
three. The blocker is a mismatch between *where the fast x86 translator lives* and *where the hardware 3D lives*, plus
the anti-cheat environment the Linux path depends on. Both conditions could change; neither has yet.

It is a feasibility note, not a plan.

## How to read this page

Nothing here was measured on macOS. Every claim below is one of four kinds, and the load-bearing ones are tabulated so
you can check them rather than take our word:

* **measured here** — reproduced on this machine (Apple M2, Fedora Asahi Remix); the command or test is named.
* **cited** — stated by a primary source (a project's own docs/repo, a maintainer, a vendor). The link is given, but we
  did **not** independently reproduce it — in particular, the `steamac` claims are **that project's own reporting**, and
  no third party has reproduced them.
* **inferred** — our reasoning from cited facts; no source states it directly.
* **unverified** — plausible, no evidence either way.

| load-bearing claim | status | check it at |
|---|---|---|
| `muvm` is a libkrun front-end | cited | muvm README |
| libkrun runs on Linux/KVM **and** macOS/Hypervisor.framework | cited | libkrun README |
| the `steamac` stack (libkrun + Venus + FEX + Proton 11 ARM64) runs on a Mac | cited, **self-attested** | its own repo/README |
| a libkrun guest is 4 KB-page | cited | `libkrunfw` `config-libkrunfw_aarch64` (`CONFIG_ARM64_4K_PAGES=y`) |
| Rosetta-for-Linux needs Apple's `Virtualization.framework` (so not libkrun) | cited | Apple docs; Podman/libkrun issues |
| Rosetta cannot `ptrace` translated binaries | cited | seclab-bonn write-up (link below) |
| Apple gives a **Linux** guest only virtio-gpu **2D**, no 3D | cited | Apple WWDC22; UTM maintainer |
| EAC's Linux/Proton client is an x86-64 ELF; there is **no aarch64 build** | cited | Steam appid 1826330 (no ARM depot) |
| Epic lists the Anti-Cheat *Client Interface* as unsupported on **Linux ARM64** | cited | Epic EOS Anti-Cheat docs (link below) |
| upstream FEX cannot run EAC out of the box | cited | FEX issue #4348 (open) |
| the gap is `ptrace` fidelity — the EAC launcher injects its client with `ptrace` | inferred | no upstream source states this; it is this repository's finding (`patches/0001`, [how-it-works.md](how-it-works.md)) |
| a macOS-libkrun guest **can** run the patched FEX (4 KB pages by construction) | inferred | the `libkrunfw` config above |
| "~35–50 FPS" for VRChat through this stack | inferred (extrapolated) | §*Performance expectation* below |
| whether EAC's Linux client **accepts a macOS-libkrun guest** | **unverified — the open question** | the experiment in [macos-port.md](macos-port.md) |

The last row is the whole point: everything else is cited, inferred, or mechanical, and it is the one thing nobody has
tried.

## The stack on both sides is the same shape

The working Asahi stack and the proposed macOS stack are architecturally identical — same three slots, different
vendors:

| slot | Asahi (what works here) | macOS |
|---|---|---|
| Windows API → host | Wine / Proton | Wine (Game Porting Toolkit / CrossOver) |
| x86-64 → arm64 | **FEX-Emu** | **Rosetta 2** (inside an arm64 Linux guest) or Windows' Prism |
| DirectX → GPU | DXVK → **Vulkan** | **D3DMetal** → Metal |

So "port it to macOS" is not a category error: it is this project with the translators swapped.

## Why the pieces don't line up

### 1. The Windows route is out (kernel-mode EAC)

EAC has two modes. The **Windows** mode loads a signed kernel driver (ring 0) and uses kernel callbacks. Running that
inside an ARM Windows 11 VM means stacking three things EAC is built to reject: a hypervisor guest, the x86-64 game
going through Windows' own Prism translation *inside* ARM Windows *inside* a VM (double translation), and a kernel
driver now running against a virtualized kernel. In a Windows guest, EAC also refuses the default Hyper-V hypervisor
vendor string (VRChat's own VM guide; `VRChat cannot run in Virtual Machine`). Windows-in-a-VM on a Mac is therefore not a path.

The **Linux/Proton** mode is different: there is no kernel driver, and Epic enabled a *native Linux userspace EAC
client* that the Windows EAC binary talks to under Wine/Proton. That is the mode this repository targets: the Proton
EasyAntiCheat Runtime Valve ships (Steam appid 1826330) is **x86-64 only**, and VRChat ships no Arm64-aware EAC
bootstrapper, so on an arm64 host the EAC client that runs is x86-64 code under the translator (FEX here).
Windows-in-a-VM does not run it. (An **unverified** aside for completeness — read from SDK release notes, not checked here: Epic has added Linux **Arm64** support to the *EOS SDK*
— noted in SDK 1.16.4 and 1.17.1.3 — but the EOS anti-cheat docs still list the Anti-Cheat Client Interface as
unsupported on Linux ARM64, and it is a per-title opt-in that no VRChat user has reported being in use.)

### 2. Fast translation and hardware 3D live on different UTM backends

UTM (the practical way to run a Linux arm64 guest on a Mac) has two backends, and the two things a port needs are split
across them:

* **Rosetta requires the Apple Virtualization backend.** UTM's documentation and discussion #4939 state it: Rosetta is
  registered through `binfmt_misc` from a VirtioFS mount and is "only available in the Virtualization.Framework
  backend". The QEMU backend cannot use it.
* **The accelerated Linux graphics are the QEMU backend's** — virtio-gpu with Mesa's VirtIO-**Venus** driver (Vulkan
  1.3), plus OpenGL 4.1 via a Core OpenGL backend. These are UTM 5.0.x **beta** features.

So today you get **Rosetta OR hardware 3D, not both**. The exact configuration a port needs — a Linux guest with
Rosetta *and* 3D — is an open UTM feature request (**#7921**), described in its own words as wanting "Rosetta, nested
virtualization, and 3d acceleration all at once". It is a wish, not a shipping option.

| backend | x86-64 → arm64 | GPU for a Linux guest | outcome |
|---|---|---|---|
| Apple Virtualization | **Rosetta** (fast; hardware TSO; Apple-supported) | none | the game cannot render |
| QEMU | no Rosetta → you would run **FEX** in the guest | **Venus** Vulkan (beta) | this project rebuilt on macOS, on a beta GPU stack |

### 3. The QEMU path rebuilds this project rather than porting it

The only macOS configuration that runs the Linux/Proton EAC stack is a **Linux arm64 guest**, and if that guest has no
Rosetta it uses **FEX** — i.e. exactly what this repository already patches. So the work transfers, but the *gain* over
Asahi is only the removal of the muvm micro-VM (Apple's virtualisation runs 4 KB pages natively, so FEX no longer needs
a 4 KB guest). You pay for that with a beta Vulkan stack and a *slower* translator, because you are using FEX instead of
Rosetta. Rosetta's advantages — hardware total-store-ordering (x86 memory semantics nearly free) and 16 KB-page support
— are only reachable on the Apple backend, which has no 3D. (The page size is a property of the **guest kernel you boot**,
not a limit the hypervisor imposes: a Linux guest is 4 KB or 16 KB because the kernel was built with
`CONFIG_ARM64_4K_PAGES` or `CONFIG_ARM64_16K_PAGES`. `libkrun` bundles its own kernel and builds it 4 KB
(`config-libkrunfw_aarch64`), so a libkrun guest is 4 KB **by construction**; Apple's `Virtualization.framework` imposes
no page-size rule and its own sample boots Fedora's 4 KB `aarch64` kernel; UTM/QEMU boots whatever kernel image you give
it. The 16 KB case is only the **Asahi bare-metal host**, which is why *this* machine needs muvm at all.)

### 4. What is *not* the blocker

* **GPU support is not absent anymore.** It was, before UTM 5.0; Venus Vulkan for Linux guests is why this is a nuanced
  answer rather than a flat no.
* **The emulator patches are not macOS-specific or Asahi-specific.** `patches/` only needs FEX-Emu on aarch64 Linux.
* **The tooling is portable.** `tools/harness/*.py` are plain Python; the method (trace the wake-up, A/B the arms,
  decode the signal ring) applies wherever FEX runs.

### Rosetta is *doubly* out, not just GPU-less

Rosetta is not merely on a backend without 3D — it also **does not support `ptrace` for translated binaries** (attempts
fail with "Couldn't get CS register: Input/output error"; blamed on being architectural, not a fixable bug:
<https://abe.seclab-bonn.de/2026/posts/gdb_macs_2/>). That is fatal here for a second, independent reason: **patch 0001
of this repository exists to emulate x86-64 `ptrace`**, because the EAC launcher injects its client into the game with
it. A translator that cannot support `ptrace` cannot run that launcher regardless of graphics. So FEX is not a
"slower-but-workable" substitute for Rosetta on this workload — it is the only translator that could support the
launcher's `ptrace` injection at all. (Rosetta also fakes `uname`/`/proc/cpuinfo` as `VirtualApple`, and Apple has said
general-purpose Rosetta runs only through macOS 27; the Linux-VM carve-out past that is not guaranteed. All further
reasons not to build on it.)

Translator speed on x86-64 → ARM64, as fractions of native (estimates from
<https://tnk4on.github.io/libkrun-rosetta/> and a 7z cross-check at <https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/>;
**estimates, not controlled benchmarks**):

| translator | vs native | GPU route it can use | `ptrace`? |
|---|---|---|---|
| Rosetta for Linux | ~70–80% | VZ only — **no 3D** | **no** |
| FEX-Emu (this project) | ~50–70% | libkrun + Venus — **3D** | **yes** (patch 0001) |
| Box64 | ~40–57% | libkrun + Venus — 3D | untested here |

### Performance expectation (extrapolated, wide bands)

**No VRChat-specific benchmark exists under FEX+Proton on ARM64, and no FEX-vs-Rosetta head-to-head exists on identical
M-series hardware.** The following is composed from measured building blocks and should be read as an order-of-magnitude
estimate, not a prediction:

* **CPU translation:** FEX ≈ 50–80% of native on M-series (Rosetta ≈ 70–80%). Ordering — native > Rosetta > FEX+Proton —
  is well supported; the magnitudes are not. FEX's own worst case is far worse than Rosetta's (a documented ~9× cliff
  when TSO emulation is left fully on in a lock-heavy scene: <https://github.com/FEX-Emu/FEX/discussions/5349>).
* **The reason Rosetta wins is hardware TSO.** Apple silicon implements x86's total-store-ordering in hardware, so
  Rosetta does not pay FEX's software-TSO tax (hardware-TSO cost on M1 ≈ 8.9%: Wrenger, JSA 2024). On a system *without*
  Rosetta, FEX gets hardware TSO only if the guest kernel can switch the CPU into TSO mode (`prctl(PR_SET_MEM_MODEL)`,
  Asahi's kernel series). On Asahi this works on the host and inside the muvm guest (measured with
  `tests/bench/tso_probe.c`, so FEX pays nothing for it there). On macOS, libkrunfw's guest kernel carries the same
  series, but libkrun itself never sets the TSO bit, so it works only if Hypervisor.framework lets the guest set it — not
  verified by anyone. If it does not, FEX falls back to software TSO, the single largest emulation cost.
* **GPU layer:** no quantified MoltenVK-vs-native-Metal or D3DMetal-vs-DXVK FPS figures exist publicly; treat graphics
  overhead as unmeasured.
* **Thermals:** a fanless MacBook Air M2 sustains ~10–25% below peak under 20–30-minute loads, so a session settles
  below its initial rate regardless of the stack. (This page's own testing was on the battery-powered Apple M2 host in
  [setup-asahi.md](setup-asahi.md); the range is an estimate, not a benchmark.)

A native-x86 scene that runs 60 FPS on a comparable desktop extrapolates to roughly **35–50 FPS typical** through
FEX+Proton on M-series (band ~25–55; floor ~15–25 in a TSO-heavy, throttled case). All of this is extrapolated from
other games and microbenchmarks — see the sources gathered for this section (FEX "Scourge of emulation", box86.org
benchmark table, TOSTING, Proton-11-ARM game reports).

## On hardening the VM

Virtualisation can be hidden far better than the default, and "harden the guest until the hypervisor is invisible" is a
real line of work. Two things to keep straight:

1. **In the Linux/Proton mode it is not clear the VM is the fight — but that is an inference, not a fact.** The
   anti-cheat there is a *userspace* client, and this repository's work is about making FEX behave like a real Linux
   kernel, not about defeating a hypervisor check. But be precise about the evidence: **EAC's Linux-mode VM checks are
   undocumented and have not been publicly reverse-engineered** — Epic's "the Anti-Cheat Client Interface does not
   support virtual machines" is a blanket statement with no Linux carve-out — and the *only* known data point of EAC's
   Linux client running inside a Linux guest VM is this repository's own Asahi/muvm run, on one machine, where whether
   `realism.sh` (DMI/PCI/hostname) is still load-bearing is itself flagged unverified. So "the VM is not the fight" is
   our reading of a single case, not an established property of the Linux client. The Windows kernel-mode client's
   anti-VM checks, by contrast, *are* well documented (see §1).
2. **Emulating a whole Windows kernel to satisfy the Windows-mode client is the expensive part.** Running Windows under
   any of this stacks a second translation layer (x86 game → Prism → ARM Windows → host) and a virtualized kernel under
   a kernel driver. That cost is what makes the Windows-in-a-VM route slow *and* the one most likely to be detected; the
   Linux userspace path avoids both. This is the reason the project takes the Linux route.

## The libkrun route — closer than it first looked

The stack this repository runs on is **libkrun**: `muvm` (the micro-VM here) is a libkrun front-end ("the `muvm` binary
uses libkrun to create microVMs"). libkrun runs on Linux via KVM and on macOS via Hypervisor.framework — the *same* VMM
on both. The macOS side is reported end to end by the `steamac` project (Valve's ARM64 SteamOS in a libkrun VM on
Apple silicon; created 2026-10-04, Apache-2.0, independent of Valve). Its README reports — **its own claim, no
independent reproduction found** — a verified pipeline of

```
game (DX9/10/11) → DXVK (Proton 11, x86 via FEX) → Vulkan
  └─ guest Mesa Venus → virtio-gpu ─ libkrun ─ virglrenderer (Venus) ─ MoltenVK | KosmicKrisp ─ Metal
```

with `Virtio-GPU Venus (Apple M4 Max)`, Vulkan 1.4, the DXVK feature set present in the guest, and DX11 titles
running (*Death's Door*, *Heroes of Might and Magic: Olden Era*). Its guest kernel is
**4 KB-page** and enables **Apple TSO for FEX via `PR_SET_MEM_MODEL`** — so it is this project's architecture on macOS,
same VMM family, same FEX, same Proton/DXVK, with only the host graphics translation swapped (MoltenVK instead of native
Vulkan). The GPU "blocker" in the UTM section above does not apply here: this route does not use UTM's backends at all,
and it does not need Apple's ParavirtualizedGraphics (which is for macOS guests).

Two consequences:

* **The macOS port uses FEX, not Rosetta.** Rosetta-for-Linux is exposed only through Apple's `Virtualization.framework`
  (`VZLinuxRosettaDirectoryShare`; Apple's "Running Intel Binaries in Linux VMs"); no supported or documented mechanism
  drives it from a raw Hypervisor.framework VMM like libkrun, and none has been implemented. So on the only macOS route
  that has both translation *and* hardware 3D, x86-64 translation is done by **the same FEX binary this repository
  patches**. Our nine patches are therefore the port's prerequisite, not a detail. (Strictly: "no supported path
  exists"; a hypothetical reverse-engineered shim is not ruled out, but nothing supports one.)
* **D3DMetal and Rosetta belong to a different route.** They are for *native macOS Wine* (Game Porting Toolkit /
  CrossOver), which cannot run the Linux/Proton EAC client and is the route EAC blocks. The workable macOS route is the
  Linux-in-libkrun one, and it uses DXVK + Venus + FEX, not D3DMetal + Rosetta. The three-slot decomposition that
  motivates the port still holds; the macOS vendors for those slots on the workable route are DXVK/Venus, FEX and Wine.

### What `steamac` does *not* establish

The stack is real; the anti-cheat question is not answered by it, and its author disclaims it:

* Its **only** statement about anti-cheat is "anti-cheat systems that block VMs will not work." There is no EAC-specific
  work, no VM-identity masking, and no issue or discussion suggesting otherwise.
* **DX12 is capped at feature level 11_0 / shader model 6.0 on MoltenVK** (the more mature driver), **12_0 on
  KosmicKrisp** (the newer Mesa Vulkan-on-Metal driver, macOS 26+); neither has SM 6.2+ or Tiled Resources Tier 3. Many
  modern EAC titles need more. The cap is the host driver's, not the protocol's: Venus nominally exposes Vulkan 1.4, but
  the guest's effective feature set is whatever MoltenVK/KosmicKrisp implements (Venus serializes commands; the host
  driver runs them).
* **The host graphics stack is patched, not upstream.** `steamac` reports all the DXVK 3.x features visible *only* with a
  **forked MoltenVK** (UTM's, adding geometry shaders and `robustness2`) plus a patched Mesa in the guest. Upstream
  MoltenVK still does not implement geometry shaders (open since 2022), so the fork is a requirement, not a convenience.
* It does **not pin a FEX or EAC version** — FEX is Valve's, shipped inside Proton 11 ARM64, and whether the ARM64
  SteamOS image even carries Valve's EAC runtime is unaddressed.
* It is **new** (created 2026-10-04), demonstrates only three or four games in short offline sessions, and has no compatibility list.
  The guest image is Valve's ARM64 SteamOS (the Steam Frame build), downloadable from Valve; its EULA forbids
  modifying or redistributing it.

Three sub-questions remain unanswered by every source found, and they are the ones that decide a port: (a) does EAC's
Linux/Proton runtime — there is **no aarch64 build** of it (Steam appid 1826330, the Proton EasyAntiCheat Runtime, is
x86-64 only; Epic's ARM EAC client is Windows-on-Arm only), so it is an x86-64 ELF — run under the guest's FEX? (b) does EAC's VM checks flag a Hypervisor.framework
guest (CPUID hypervisor bit, virtio devices, `systemd-detect-virt`)? (c) is EAC's runtime present in the ARM64 SteamOS
image at all? This repository already runs EAC inside a micro-VM (muvm/libkrun), so "it is a VM" is not by itself fatal —
but that was an Asahi host, and whether EAC tolerates a *macOS* libkrun guest the same way is untested. It is the single
real unknown, and it is a question about EAC's detection, not about the stack.

## Splitting the anti-cheat from the game: why it does not work

A natural idea is to run the anti-cheat in its own small VM or container — a clean, real-looking environment — while
only the (heavy) game is translated. It does not work, for a reason that is structural rather than a limitation of any
one VM or translator:

* **EAC's client module is injected into the game process.** The launcher starts the game suspended and injects the EAC
  module into its address space with `ptrace`, then resumes it — this is exactly what patch 0001 of this repository
  emulates. The anti-cheat then watches the game's memory, hooks and integrity **from inside that process**. A
  `ptrace`-based injection cannot cross a VM or container boundary, so the two halves must share an address space and a
  kernel view. Put them in different environments and EAC has nothing to attach to.
* **A container adds nothing.** To inspect the game it needs a shared PID namespace and `ptrace` rights — the opposite
  of isolation — and EAC fingerprints cgroups, namespaces and `/proc` anyway.
* **A "small VM" is still a VM.** The game would have to be inside the same VM for the injection to work, and EAC's
  client interface is documented as unsupported in VMs, so this is back to emulating everything.

The instinct behind the idea — keep the anti-cheat lean, do not emulate a whole Windows kernel — is already the design
here. The Linux/Proton stack is Wine (a userspace API layer), not a Windows kernel in a VM, and EAC's Linux mode is
**userspace-only with no kernel driver**. The genuine two-part split in EAC is *kernel driver + injected client*
(Windows mode), which Linux does not use and this project cannot reach. The Linux mode collapses it to "userspace
client + injected module," both in the same userspace, and they cannot be separated without severing the injection.

## Prior art, and why this repo is not that

There is an older, separate line of work called `vrc-eac-emulator` (originally `ShimadaNanaki/vrc-eac-emulator`, now
continued at `VRC-Emulator/vrc-eac-emulator`). It is **not** the same approach and nothing from it is used here:

* Its own description is "PoC of Semi-Emulated EAC **bypass**". It builds a `version.dll` bootstrapper beside
  `VRChat.exe` and a shim named `EOSSDK-Win64-Shipping.dll`, hooks EAC/EOS calls with **MinHook**, and redirects them to
  a Linux VM.
* Its setup guide's load-bearing step is a `.vmx` full of **hypervisor-hiding and hardware-spoofing** (SMBIOS/vendor/
  serial masking, avoiding the `00:50` MAC prefix), explicitly to "avoid VM detection from EAC".
* Its original repository is archived; the continuation lives under a separate organisation and has no releases on
  GitHub. (Its repository does carry a step-by-step `SETUP_GUIDE.md`; how it is distributed beyond that was not
  checked.)

That is a bypass posture: **replace** the anti-cheat's calls with hooks, and **spoof** the hardware identity to hide the
VM. This repository is the opposite in kind. It runs EAC's **sanctioned Linux/Proton mode** — which Epic enabled
deliberately, with no kernel driver — unmodified, and makes **FEX behave like a real Linux kernel** so that the official
anti-cheat client works. Nothing here hooks, shims, intercepts or short-circuits EAC's result.

One honest qualification, because it is the nearest thing here to the prior art's disguise step: `scripts/vm/realism.sh`
*does* present plausible SMBIOS/DMI/PCI/hostname data to the guest — environment adaptation, in the same category as
VRChat's own published VM guide ([README](../README.md), "What this is not"), not emulation fidelity. It is optional,
unverified as necessary, and deliberately kept out of the port experiment so it cannot confound a result. The difference
from the prior art is what the two rest on: that project's disguise exists to make a *bypass* land; this one is a
tolerated-but-unsupported tweak a user can drop, and the anti-cheat that then runs is the stock client, doing its own
checks. The "shim and hide" route is also brittle — an EAC update breaks it — which is part of why this project does not
take it.

## Verdict

| | Feasible now? |
|---|---|
| macOS host, Linux arm64 guest in **libkrun**, FEX + Wine/Proton + Venus | **closest** — prerequisites exist (`steamac`); the unknown is whether EAC's Linux client accepts a macOS-libkrun guest |
| macOS host, Linux arm64 guest in UTM (QEMU backend), FEX + Venus | **partially** — plausible, beta GPU stack, same EAC unknown |
| macOS host, Linux arm64 guest with Rosetta + 3D (UTM Apple backend) | **no** — UTM backend split (#7921); and Rosetta cannot work with libkrun at all |
| macOS host, native Wine (GPTK/CrossOver) with D3DMetal | **no** — cannot run the Linux/Proton EAC client |
| macOS host, Windows-in-a-VM, Windows-mode EAC | **no** — kernel-mode EAC under a hypervisor, double translation, VRChat refuses VMs |
| EAC in its own small VM/container, game translated separately | **no** — EAC is injected into the game process; the halves cannot be split by a VM boundary |

## What would change the answer

* **Someone tries the libkrun route and reports whether EAC's Linux client runs in a macOS-libkrun guest.** This is now
  the decisive, and only remaining, question for a macOS port — everything else is demonstrated or mechanical.
* **UTM gains virtio-gpu 3D on the Apple Virtualization backend** (#7921), which would make the UTM-backed variant
  first-class — though it still cannot use Rosetta with libkrun.
* **A native macOS build of VRChat with native EAC** (Epic supports native macOS builds of games; VRChat has none). That
  removes emulation entirely and is the only path with no translation layer — out of scope here.
* **VRChat shipping an Arm64-aware EAC bootstrapper.** VRChat's first-party answer for arm64 today is the **Android
  build** on standalone headsets (their own Steam Frame page: "the PC version of VRChat is not supported as a standalone
  option on the Steam Frame"). If they ever ship the Windows/PC build with an Arm64-aware EAC client — which Epic's SDK
  now makes possible per-title — the emulation question changes entirely.

## Sources for the cited claims

Pointers, not endorsements — we read these, we did not reproduce most of them:

* muvm (libkrun front-end, 4 KB micro-VM) — <https://github.com/AsahiLinux/muvm>
* libkrun (KVM on Linux, Hypervisor.framework on macOS; virtio-gpu/Venus/native-context) — <https://github.com/libkrun/libkrun>
* `libkrunfw` aarch64 kernel config, `CONFIG_ARM64_4K_PAGES=y` — <https://github.com/libkrun/libkrunfw/blob/main/config-libkrunfw_aarch64>
* `steamac` (Valve ARM64 SteamOS in a libkrun VM on Apple silicon) — <https://github.com/fxgl/steamac>
* Apple `Virtualization.framework` running Linux (virtio-gpu **2D**; no 3D for Linux guests) — <https://developer.apple.com/documentation/virtualization>
* Apple `ParavirtualizedGraphics` (Metal for **macOS** guests only) — <https://developer.apple.com/documentation/paravirtualizedgraphics>
* Rosetta-for-Linux is exposed only via `Virtualization.framework` — Apple, "Running Intel Binaries in Linux VMs"
* Rosetta cannot `ptrace` translated binaries — <https://abe.seclab-bonn.de/2026/posts/gdb_macs_2/>
* FEX page-size constraint (4 KB host), microVM as the fix — FEX issues #3496, #1921 ("not planned"), #1650 ("muvm")
* FEX cannot run EasyAntiCheat out of the box; `ptrace` fidelity is the gap — <https://github.com/FEX-Emu/FEX/issues/4348> (open)
* Proton EasyAntiCheat Runtime is x86-64 only (no ARM depot) — Steam appid 1826330
* Epic EOS Anti-Cheat Interfaces: client "does not support virtual machines (VM)" (blanket); unsupported platforms include "Linux ARM64 (Anti-Cheat Client Interface)" — <https://dev.epicgames.com/docs/epic-online-services/trust-and-safety/anti-cheat-interfaces/anti-cheat-interfaces>
* Steamworks Proton doc: kernel-space anti-cheat "not currently supported… user-space components for Wine" — <https://partner.steamgames.com/doc/steamhardware/proton>
* Windows-mode EAC anti-VM checks (CPUID hypervisor bit + leaf `0x40000000`, MSR/timing/descriptor, TPM/HWID) — <https://rstforums.com/forum/topic/112809-how-anti-cheats-detect-system-emulation/>, <https://github.com/goldzik1/eac-eos-driver-analysis>
* VRChat, "Using VRChat in a Virtual Machine" (EAC's VM block is the CPUID hypervisor-vendor check; tolerated but unsupported) — <https://docs.vrchat.com/docs/using-vrchat-in-a-virtual-machine>
* VRChat on the Steam Frame (first-party: PC build not supported standalone; the Android build is) — <https://help.vrchat.com/hc/en-us/articles/55751011246995-Is-VRChat-supported-on-the-Steam-Frame>
* Valve Steam Frame compatibility (Proton + FEX default for Windows x86 titles) — <https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility>
* Translator speed estimates — <https://tnk4on.github.io/libkrun-rosetta/>, <https://box86.org/2022/03/box86-box64-vs-qemu-vs-fex-vs-rosetta2/>
* FEX software-TSO worst case — <https://github.com/FEX-Emu/FEX/discussions/5349>
