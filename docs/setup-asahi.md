# Setup on Apple-silicon Fedora Asahi Remix (muvm + FEX), A to Z

This is the whole path from a fresh Fedora Asahi Remix install to VRChat in a world with genuine Easy Anti-Cheat. Every
step is needed unless it says *optional*. Section 10 lists the exact versions of the last verified working setup;
section 11 lists what breaks it. Other arm64 hosts: see `other-platforms.md`.

## 0. How the pieces fit

```
host (Fedora Asahi, 16K pages)
 └─ muvm microVM (4K-page guest kernel, GPU via virtio-gpu native context, network via passt)
     └─ FEX-Emu (x86-64 on arm64)      ← patched with patches/*.patch
         └─ Steam (x86-64) → Proton Experimental → Wine → VRChat.exe + EAC launcher/client
```

FEX needs 4K pages, which is why everything runs inside muvm on Asahi. Only the x86 interpreter binary (`FEX`) is
replaced; the distro's rootfs, thunks and muvm stay as they are.

## 1. What you need

* An Apple M1/M2-class Mac with Fedora Asahi Remix 44 (tested: MacBook Air M2, 16 GB). 16 GB is the practical minimum:
  the VM takes 8–10 GB and the GPU buffers come on top.
* **Free disk space: at least 10 GB beyond the installs** (Steam + VRChat + Proton + shader caches are ~17 GB; the FEX
  build tree ~2 GB). btrfs gets slow above ~90 % use.
* A Steam account that owns/has added VRChat, and a VRChat account.
* Build tools for FEX: `sudo dnf builddep fex-emu` (or at least `clang cmake ninja-build git python3 nasm`).
* About an hour (the FEX build alone is ~15 min on 8 cores).

## 2. Base install

```sh
sudo dnf install steam muvm fex-emu
steam      # first run: downloads Valve's bootstrap into ~/.local/share/fex-steam, log in
```

In Steam:

1. Install **VRChat** (appid 438100).
2. VRChat → Properties → Compatibility → force **Proton Experimental**.
3. Launch VRChat once. This creates the Proton prefix (`steamapps/compatdata/438100`), installs the
   **Proton EasyAntiCheat Runtime** tool (Library → Tools), and creates VRChat's entry in `localconfig.vdf` that the
   launch-option script edits. The launch is expected to fail with the stock FEX (the EAC launcher reports
   `Unexpected error (#1)`).
4. Quit Steam completely.

## 3. Build the patched FEX

```sh
scripts/build-fex.sh            # clones FEX at FEX-2609.1 (9fbdc00), applies patches/0001-0010, builds Release
scripts/vm/install-overlay.sh   # copies the binary to ~/.local/share/vrchat-fex-eac/fex/FEX
```

The VM launcher registers this binary as the x86/x86-64 binfmt interpreter **inside the VM**; the host's FEX is not
touched. binfmt pins the interpreter inode, so after installing a new build, **restart the VM** before testing.

## 4. Check the emulator without the game

```sh
cd tests/ptrace-inject
./build.sh clang            # any clang with the x86-64 target (falls back to tools/static-link.py when lld is unusable)
./run-in-vm.sh              # throw-away muvm VM with the patched FEX registered for x86-64; must end with RESULT: PASS
```

It performs the whole EAC-style injection conversation (28 checks). With the stock FEX the conversation fails early.

Two more freestanding tests check what a guest signal handler sees. Build them with clang (x86-64 binaries, to run under
FEX) and run them in the same throw-away VM, or inside the running Steam VM with `muvm -- sh -c '<binary> > file'` (the output
has to go to a file: muvm does not relay the guest's stdout):

```sh
cd tests/signal-mask && ./build.sh clang      # the signal mask a handler runs with (patch 0007)
./signal_mask_test                            # RESULT: PASS; with FEX_SIGNALMASKFIX=0 (stock behaviour) checks 1, 3 and 4b fail
cd ../signal-regs && ./build.sh clang         # registers a handler sets survive its first store and system calls (patch 0008)
./signal_regs_test                            # RESULT: PASS; on stock FEX the tgkill/kill/tkill cases fail
```

Both build natively too (`./build.sh native`, aarch64 or x86-64 host) as the reference run on a real Linux kernel.

## 5. Host tuning (root, strongly recommended on 16 GB)

```sh
sudo scripts/host-tune.sh apply     # zswap/zram/watermarks/readahead: keeps the host OOM killer off the VM
```

See `tuning.md`. Optional: `scripts/cpu-power.sh` (CPU frequency policy; see `tuning.md` for the trade-off).

## 6. Launch options

With Steam **closed** (it rewrites `localconfig.vdf` on exit):

```sh
scripts/set-launch-options.sh                   # add DXVK_HUD=fps as an argument to see an FPS counter
```

This writes, for every Steam user that has VRChat:

```
env EAC_LAUNCHERDIR=<prefix>/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat
    PROTON_EAC_RUNTIME='<steamapps>/common/Proton EasyAntiCheat Runtime'
    WINEDEBUG=-all PROTON_USE_XALIA=0 DXVK_CONFIG_FILE=<repo>/scripts/dxvk.conf %command%
```

`EAC_LAUNCHERDIR` and `PROTON_EAC_RUNTIME` are what make Proton load the EAC runtime. `dxvk.conf` caps the memory DXVK
reports to Unity at 3 GB (a 16 GB unified-memory machine otherwise looks like it has several GB of free VRAM).
**Keep `WINEDEBUG=-all` and add nothing else** — see section 11.

## 7. Start Steam inside the VM

```sh
REALISM=1 VM_MEM_MB=10240 VM_VRAM_MB=3072 scripts/vm/steam-vm.sh     # the verified configuration (section 10)
```

Defaults: 8 GB guest RAM (`VM_MEM_MB`), 4 GB "VRAM" (`VM_VRAM_MB`), guest NIC MTU 1500 (`VM_MTU`), the VM in its own
systemd scope with high CPU/IO weight and `MemoryLow=8G`. The verified run in section 10 used
`VM_MEM_MB=10240 VM_VRAM_MB=3072`; both work. Do not go below 8 GB RAM: the guest has no swap and its OOM killer takes
VRChat once a world loads.

The MTU setting matters: the virtio NIC comes up with a 64 KB MTU, and without the clamp TCP super-segments get
fragmented and retransmitted and large UDP datagrams vanish (stutter and ~60 s stalls at the Photon region lookup).

Wait until Steam is logged in (the Steam window appears).

## 8. Launch VRChat

From another terminal **on the host**:

```sh
steam steam://rungameid/438100
```

or press Play in the Steam window running in the VM. The EAC splash appears, then the game window.

## 9. Check that it really works

All three must hold:

1. **EAC launcher:** `<prefix>/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat/<product>/<deployment>/anticheatlauncher.log`
   ends with `Launcher finished with: 301, 'Easy Anti-Cheat successfully loaded in-game'`.
2. **The game started:** a new `output_log_<date>_<time>.txt` appears in
   `<prefix>/drive_c/users/steamuser/AppData/LocalLow/VRChat/VRChat/` within ~30 s of the splash and keeps growing.
   (EAC's 301 alone is not enough: it is logged before the game finishes initialising.)
3. **In a world:** that log contains `[EOSManager] AntiCheat Session Begin: Success` and `Finished entering world`.

Quick check of the newest log:

```sh
L=$(ls -t ~/.local/share/Steam/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/LocalLow/VRChat/VRChat/output_log_*.txt | head -1)
grep -cE "AntiCheat Session Begin: Success|Finished entering world" "$L"     # expect 2
```

## 10. Verified working configuration

Last verified 2026-10-07: EAC 301, `AntiCheat Session Begin: Success` 12 s after start, world joined 29 s after start,
in-world until the VM was shut down ~100 s later. Dozens of sessions ran with this setup the same day.

| component | version / value |
|---|---|
| host | MacBook Air M2 16 GB, Fedora Asahi Remix 44, kernel `7.1.13-402.asahi.fc44.aarch64+16k` |
| VM | `muvm-0.6.0-3.fc44`, `libkrun-1.19.0-1.fc44`, `libkrunfw-5.5.0-1.fc44`, `passt-0^20260728.gf8df3f1-2.fc44` |
| GPU driver (guest) | `mesa-vulkan-drivers-26.1.8-1.fc44` |
| FEX | FEX-2609.1 (`9fbdc00`) + `patches/0001`–`0010`, Release build, binary used only as the in-VM binfmt interpreter |
| Proton | Proton Experimental `experimental-11.0-20261001` + Proton EasyAntiCheat Runtime |
| Steam runtime | SteamLinuxRuntime_4 `4.0.20260805.254769` |
| VRChat | Steam build id `25738324` |
| launch options | exactly the section 6 string: `WINEDEBUG=-all PROTON_USE_XALIA=0 DXVK_CONFIG_FILE=…` |
| VM size | 10 GB RAM, 3 GB VRAM, MTU 1500 |
| environment realism | on (fake PID 1, DMI, PCI list, hostname — `REALISM=1`, see below); not tested without it since the ptrace emulation landed |
| Proton `user_settings.py` | no settings needed (the verified run set only the in-process profiler at 1 Hz, which is harmless) |

## 11. What breaks it

* **Wine trace channels, `PROTON_LOG=1` or FEX logging knobs in the launch options.** With
  `WINEDEBUG=err+all,+loaddll,+module,+seh PROTON_LOG=1 FEXHOTMEMFD=1` the EAC launcher still reports 301, but
  `VRChat.exe` dies ~3 s later inside `LdrInitializeThunk` while loading `kernel32.dll`, before Unity writes any log;
  the Proton log shows `err:virtual:virtual_setup_exception nested exception on signal stack`. Reproduced with three
  different FEX builds; going back to the section 6 string fixed it at once. Which of the three settings is responsible
  is not isolated — use none of them for play.
* **A FEX build without patch 0001** (or the stock FEX): the EAC launcher ends with `210, 'Unexpected error. (#1)'`.
* **Installing a new FEX without restarting the VM:** the old binary stays registered (binfmt pins the inode).
* **Attaching a debugger, `perf`, `strace` or any ptrace tool to the game:** EAC reports it ("Debugger detected").
* **Starting Steam inside the VM as root** (`muvm --privileged -- …steam…`): FEX cannot reach FEXServer
  (`Couldn't connect to FEXServer socket`) and nothing launches. Use plain `muvm --` or the host `steam` command.
* **Less than ~8 GB guest RAM, or an untuned 16 GB host:** OOM kills (the game or the whole VM).
* **A full disk.**

## Optional: environment realism

`REALISM=1 scripts/vm/steam-vm.sh` additionally applies `scripts/vm/realism.sh` (fake DMI/PCI/PID 1/hostname inside
the VM). It follows VRChat's VM guide. The verified configuration above had it on.
