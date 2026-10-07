# Could this work on macOS?

Short answer: **not today**, and the reason is specific rather than general. The naive three-slot decomposition —
translate DirectX, translate x86-64, emulate the Windows API — is correct, and macOS has good answers for two of the
three. The blocker is a mismatch between *where the fast x86 translator lives* and *where the hardware 3D lives*, plus
the anti-cheat environment the Linux path depends on. Both conditions could change; neither has yet.

This page records the analysis so it is not re-litigated. It is a feasibility note, not a plan.

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
driver now running against a virtualized kernel. VRChat also refuses virtual machines outright (`VRChat cannot run in
Virtual Machine`). Windows-in-a-VM on a Mac is therefore not a path.

The **Linux/Proton** mode is different: there is no kernel driver, and Epic enabled a *native Linux userspace EAC
client* that the Windows EAC binary talks to under Wine/Proton. That is the mode this repository targets, and it exists
**only on Linux**. Windows-in-a-VM does not run it.

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
— are only reachable on the Apple backend, which has no 3D.

### 4. What is *not* the blocker

* **GPU support is not absent anymore.** It was, before UTM 5.0; Venus Vulkan for Linux guests is why this is a nuanced
  answer rather than a flat no.
* **The emulator patches are not macOS-specific or Asahi-specific.** `patches/` only needs FEX-Emu on aarch64 Linux.
* **The tooling is portable.** `tools/harness/*.py` are plain Python; the method (trace the wake-up, A/B the arms,
  decode the signal ring) applies wherever FEX runs.

## On hardening the VM

Virtualisation can be hidden far better than the default, and "harden the guest until the hypervisor is invisible" is a
real line of work. Two things to keep straight:

1. **In the Linux/Proton mode the VM is not the fight.** The anti-cheat there is a *userspace* client, and the reports
   in this repository are about making FEX behave like a real Linux kernel — not about defeating a hypervisor check. A
   hardened guest is compatible with this project's approach; it does not replace it.
2. **Emulating a whole Windows kernel to satisfy the Windows-mode client is the expensive part.** Running Windows under
   any of this stacks a second translation layer (x86 game → Prism → ARM Windows → host) and a virtualized kernel under
   a kernel driver. That cost is what makes the Windows-in-a-VM route slow *and* the one most likely to be detected; the
   Linux userspace path avoids both. This is the reason the project takes the Linux route.

## The libkrun route — closer than it first looked

The stack this repository runs on is **libkrun**: `muvm` (the micro-VM here) is a libkrun front-end ("the `muvm` binary
uses libkrun to create microVMs"). libkrun runs on Linux via KVM and on macOS via Hypervisor.framework — the *same* VMM
on both. And the macOS side is already demonstrated end to end: the `steamac` project (Valve's ARM64 SteamOS in a
libkrun VM on Apple silicon) reports a verified pipeline of

```
x86-64 game → DXVK (Proton 11 ARM64, x86 via FEX) → Vulkan → Venus (virtio-gpu) → MoltenVK → Metal
```

with Vulkan 1.4 and the DXVK feature set present in the guest, running DX11 titles. That is **this project's
architecture on macOS** — same VMM family, same FEX, same Proton/DXVK — with only the host graphics translation
swapped (MoltenVK instead of native Vulkan). The GPU "blocker" in the UTM section above does not apply here: this route
does not use UTM's backends at all, and it does not need Apple's ParavirtualizedGraphics (which is for macOS guests).

Two consequences:

* **The macOS port uses FEX, not Rosetta.** Rosetta is architecturally bound to Apple's `Virtualization.framework` and
  cannot work with libkrun/Hypervisor.framework (Podman discussion #28297). So on the only macOS route that has both
  translation *and* hardware 3D, x86-64 translation is done by **the same FEX binary this repository patches**. Our nine
  patches are therefore the port's prerequisite, not a detail.
* **D3DMetal and Rosetta belong to a different route.** They are for *native macOS Wine* (Game Porting Toolkit /
  CrossOver), which cannot run the Linux/Proton EAC client and is the route EAC blocks. The workable macOS route is the
  Linux-in-libkrun one, and it uses DXVK + Venus + FEX, not D3DMetal + Rosetta. The three-slot decomposition that
  motivates the port still holds; the macOS vendors for those slots on the workable route are DXVK/Venus, FEX and Wine.

The one thing `steamac` lists as out of scope is exactly what this project is about: "anti-cheat systems that block VMs
won't work." But this repository already runs EAC inside a VM — the muvm/libkrun micro-VM — so "it is a VM" is not by
itself fatal; the open question is only whether EAC's Linux client accepts a macOS-libkrun guest as it accepts an
Asahi-muvm one. That is untested, and it is the single real unknown.

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
* **A "small VM" is still a VM.** VRChat refuses virtual machines, and the game would have to be inside the same VM for
  the injection to work, so this is back to emulating everything.

The instinct behind the idea — keep the anti-cheat lean, do not emulate a whole Windows kernel — is already the design
here. The Linux/Proton stack is Wine (a userspace API layer), not a Windows kernel in a VM, and EAC's Linux mode is
**userspace-only with no kernel driver**. The genuine two-part split in EAC is *kernel driver + injected client*
(Windows mode), which Linux does not use and this project cannot reach. The Linux mode collapses it to "userspace
client + injected module," both in the same userspace, and they cannot be separated without severing the injection.

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
