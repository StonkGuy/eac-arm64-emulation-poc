# Setup on Apple-silicon Fedora Asahi Remix (muvm + FEX)

Tested on a MacBook Air M2 (16 GB), Fedora Asahi Remix 44, kernel `7.1.x-asahi`, `muvm` 0.6, distro `fex-emu` 2604,
patched FEX 2609.1, Proton Experimental + Proton EasyAntiCheat Runtime. Other arm64 hosts: see `other-platforms.md`.

## 0. How the pieces fit

```
host (Fedora Asahi, 16K pages)
 └─ muvm microVM (4K-page guest kernel, GPU via virtio-gpu native context)
     └─ FEX-Emu (x86-64 on arm64)      ← patched with patches/*.patch
         └─ Steam (x86-64) → Proton → Wine → VRChat.exe + EAC launcher/client
```

FEX needs 4K pages, which is why everything runs inside muvm on Asahi. Only the x86 interpreter binary (`FEX`) is
replaced; the distro's rootfs, thunks and muvm stay as they are.

## 1. Install the base

```sh
sudo dnf install steam muvm fex-emu
steam            # first run downloads Valve's bootstrap into ~/.local/share/fex-steam and logs you in
```

Install VRChat from Steam and launch it once so Proton creates the prefix and installs the *Proton EasyAntiCheat
Runtime* tool (it appears under Steam → Library → Tools). Set Proton Experimental as VRChat's compatibility tool.

## 2. Build the patched FEX

```sh
scripts/build-fex.sh            # clones FEX at FEX-2609.1, applies patches/, builds (≈15 min on 8 cores)
scripts/vm/install-overlay.sh   # copies the binary to ~/.local/share/vrchat-fex-eac/fex/FEX
```

## 3. Check the ptrace emulation without the game

```sh
cd tests/ptrace-inject
./build.sh clang            # any clang with the x86-64 target (falls back to tools/static-link.py when lld is unusable)
./run-in-vm.sh              # throw-away muvm VM with the patched FEX registered for x86-64; must end with RESULT: PASS
```

It performs the whole EAC-style injection conversation (28 checks). With the stock FEX the conversation fails early.

Two more freestanding tests check what a guest signal handler sees. Build them with clang (x86-64 binaries, to run under
FEX) and run them in the same throw-away VM, or inside the running Steam VM with `muvm -- sh -c '<binary> > file'` (the output
has to go to a file):

```sh
cd tests/signal-mask && ./build.sh clang      # the signal mask a handler runs with (patch 0007)
./signal_mask_test                            # RESULT: PASS; with FEX_SIGNALMASKFIX=0 (stock behaviour) checks 1, 3 and 4b fail
cd ../signal-regs && ./build.sh clang         # registers a handler sets survive its first store and system calls (patch 0008)
./signal_regs_test                            # RESULT: PASS; on stock FEX the tgkill/kill/tkill cases fail
```

Both build natively too (`./build.sh native`, aarch64 or x86-64 host) as the reference run on a real Linux kernel.

## 4. Launch options

Close Steam, then:

```sh
scripts/set-launch-options.sh DXVK_HUD=fps       # drop DXVK_HUD=fps for normal play
```

This writes `env EAC_LAUNCHERDIR=… PROTON_EAC_RUNTIME=… WINEDEBUG=-all PROTON_USE_XALIA=0 DXVK_CONFIG_FILE=… %command%`
into VRChat's launch options. Keep `WINEDEBUG=-all`: Wine trace channels and FEX logging knobs slow the game down
badly and have caused crashes.

## 5. Start Steam in the VM and play

```sh
scripts/vm/steam-vm.sh            # patched FEX inside a muvm VM with 8 GB RAM / 4 GB "VRAM"
steam steam://rungameid/438100    # from another terminal on the host
```

Success looks like: the launcher log
(`…/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat/<product>/<deployment>/anticheatlauncher.log`) contains
`Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'` and VRChat's `output_log_*.txt` contains
`[EOSManager] AntiCheat Session Begin: Success`.

## 6. Recommended host tuning (needs root)

See `tuning.md`; `sudo scripts/host-tune.sh` fixes the memory/readahead defaults that made the host OOM-kill the VM.

## Optional: environment realism

`REALISM=1 scripts/vm/steam-vm.sh` additionally applies `scripts/vm/realism.sh` (fake DMI/PCI/PID 1/hostname inside
the VM). It follows VRChat's VM guide. Whether it is required with the ptrace emulation in place is untested.
