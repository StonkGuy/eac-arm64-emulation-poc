# Sketch: a macOS port

**Status: a plan, not a result.** Nothing has been tried on macOS. The repository has run on exactly one host — the
Apple M2 / Fedora Asahi Remix 44 machine described in [setup-asahi.md](setup-asahi.md) — and every claim below about a
macOS host is untested. This page records how that stack could be re-created on macOS and the single experiment that
decides whether it is worth doing. See [platform-viability.md](platform-viability.md) for the evidence behind each claim.

## What carries over, what is rebuilt

The Asahi stack and the macOS stack share a VMM family (**libkrun**: `muvm` here, `libkrun` directly on macOS via
Hypervisor.framework) and the same FEX + Wine/Proton + DXVK layers. Two concrete things carry over unchanged:

| piece | why it carries over |
|---|---|
| `patches/0001–0009` | x86-64 translation on this route is **FEX**, the same binary; nothing in the patches is Asahi- or muvm-specific |
| `tools/harness/*.py` | plain Python; the method (trace the wake-up, A/B the arms, decode the ring) applies wherever FEX runs |

What is **replaced**: the host graphics path. Asahi uses DRM native context (host GPU driver + guest Mesa, near-native);
macOS has no such path for Linux guests, so it would use **Venus (virtio-gpu) → virglrenderer → MoltenVK → Metal**,
which is more feature-limited: DX12 tops out at feature level 11_0 / SM 6.0 on MoltenVK (12_0 on the newer
KosmicKrisp driver, macOS 26+), and neither reaches SM 6.2+ or Tiled Resources Tier 3. What is **dropped**: muvm itself
— Apple virtualisation gives 4 KB pages natively, so no page-size micro-VM is needed.

The `steamac` project is the worked example of this stack (its own claim: libkrun + Venus + FEX + Proton 11 ARM64 +
DXVK, running DX11 titles). A port would either build on it or reproduce its host glue.

## The one experiment that decides it

Everything except EAC is either reported end to end on a Mac (the `steamac` stack, on that project's own account) or
mechanical. The open question is whether **EAC's sanctioned Linux/Proton client accepts a macOS-libkrun guest** the way
it accepts the Asahi-muvm one. That is a controlled A/B, not a build.

### Hypothesis

* **H1:** EAC's Linux client returns `Launcher finished with: 301` in a macOS-libkrun guest, as it does under
  Linux/KVM/muvm — i.e. the host VMM is not a discriminator.
* **H0 (the likely outcome — Epic documents the Anti-Cheat *Client* as unsupported on Linux ARM64 and on VMs):** a
  deterministic non-`301` verdict on the macOS host while the Linux/muvm control gives `301`.

### Three arms

| arm | host | label |
|---|---|---|
| A | native x86-64 Linux (Ryzen, bare metal) | ground truth that `301` is reachable at all |
| B | Fedora Asahi — muvm/libkrun/KVM, 4K guest, patched FEX | control (known-good here) |
| C | macOS Apple silicon — libkrun/Hypervisor.framework, 4K guest, patched FEX | the test |

### Procedure (per host, fresh VM each scored run)

1. Run `tests/ptrace-inject` in the throw-away VM → must print `RESULT: PASS`. If not, the run is **invalid** (stack
   misconfigured), not a verdict.
2. Confirm the runtime is identical on all arms: Proton EasyAntiCheat Runtime present, `PROTON_EAC_RUNTIME` set to the
   runtime **root**, and the launcher's own `System name: 'linux64'`, bootstrapper version and CDN module line recorded
   (so all arms load the same module).
3. Launch through Steam with a clean environment (no `WINEDEBUG` trace channels, no FEX logging knobs).
4. Capture `anticheatlauncher.log` and VRChat's `output_log_*.txt`.
5. Interleave ≥5 runs per arm (this host throttles in bursts; compare per phase).

**Do not** enable `scripts/vm/realism.sh` or any DMI/CPU-hypervisor masking as part of the experiment. It would confound
the result (you would not know whether a `301` came from the stack or from the masking) and it is environment
adaptation, not the thing under test. Leave it off; optionally one arm with it on, only to show it makes no difference.

### Pass / fail criteria

**H1 supported** (EAC accepts the macOS guest) when, with arm B passing:
`Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` **and** the game log shows
`AntiCheat Session Begin: Success` with the `[AntiCheatClient] … bound` lines, **and** there is no `null client`,
`Cannot run under Virtual Machine`, `Forbidden …` or `Unexpected error` string.

**H1 falsified** when the launcher reaches `Starting Wine module mapping` and then returns a deterministic non-`301`
verdict on arm C across repeated runs while arm B gives `301`. The strongest negative points to the exact step where
the arms diverge; a failure at/before `Loader initializing` or the CDN fetch is **configuration**, not an EAC verdict.

### False results to guard against

* **False positive:** a `null client … 301` line means EOS loaded a stub, not EAC — grep for `null client` and require
  the game-side `Session Begin: Success`.
* **False negative (looks like "EAC rejected the VM" but is misconfiguration):** unpatched FEX or a stale `binfmt_misc`
  inode after a rebuild; missing EAC runtime or a wrong `PROTON_EAC_RUNTIME`; low `vm.max_map_count`; guest OOM (the
  host OOM-kills the VM first — check `journalctl -k`); logging knobs left on.
* **Confound:** FEX runs in *both* hosts, so a FEX-fidelity bug would be host-independent. Only a divergence between B
  and C with the same FEX binary and guest image implicates the host VMM.

## If H1 holds

The port is then engineering — but not trivial engineering on the graphics side. The host glue is a stack of **patched,
version-pinned, non-upstream** components, not a package you install: `virglrenderer` is pinned to one revision for the
Venus protocol ABI, the guest Mesa is built from pinned sources (upstream Mesa as packaged is not enough on macOS),
MoltenVK is a patched fork, and there are known 16 KB-blob alignment fixes. The *rendering* path is what `steamac` added
on top of the libkrun/Venus plumbing (without it, a Venus guest is compute-only, with no rendering). So the work is:
reproduce that host glue for a plain Linux guest,
build the patched FEX, wire Steam/Proton, and tune (thermals; the strict-NAT UDP path for VRChat's Photon traffic;
microphone capture — both unproven on this stack).

## If H0 holds

A clean, well-controlled negative is the result to publish: it would show that EAC's VM gate, not FEX fidelity, is the
binding constraint on macOS — consistent with Epic's own documentation, and a useful boundary for anyone attempting an
arm64 port on any host.

## Non-goals

No hooking, shimming, patching or replacing `start_protected_game.exe`; no `EOS_USE_ANTICHEATCLIENTNULL`; no hypervisor
masking as a *requirement* of the experiment. The point is whether the stock, sanctioned Linux/Proton EAC client runs
on this host, not to make it run by defeating a check.
