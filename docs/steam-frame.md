# Steam Frame

The Steam Frame runs x86 games the same way the macOS port does: Steam runs Proton inside its own FEX compatibility
tool (Steam app 3127680). The fix is the same too. Build the patched FEX on the Frame and swap it into that tool. The
two scripts in [`scripts/steamos/`](../scripts/steamos) do both.

**Status: reported, not tested here.** Another user ran EAC titles this way with no changes beyond replacing Valve's
FEX; performance is the limit. Nobody on this project owns a Frame, so the steps below come from the macOS guest, which
runs Valve's Steam Frame SteamOS image (see [macos-port.md](macos-port.md)), and from Valve's Steam Frame developer
documentation. Please [report](../README.md#bugs-fixes-and-upstreaming) what works and what doesn't.

## What it takes

| you need | why |
|---|---|
| Developer Mode on the Frame | for a shell (SSH or Remote Desktop) |
| the game's compatibility tool set to **Proton Experimental** | ARM64 Steam otherwise picks an Android build where the game has one |
| **Proton EasyAntiCheat Runtime** installed (a Steam tool) | EAC's Proton runtime |
| Steam's FEX tool and the EAC runtime on internal storage | the scripts expect Steam's default library (`~/.local/share/Steam`) |
| clang, cmake, ninja, lld, git, python3 | FEX is built on the Frame itself; the SteamOS image ships them |

## 1. Get a shell

From Valve's [Steam Frame debugging](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging) and
[setup](https://partner.steamgames.com/doc/steamhardware/steamframe/setup) pages:

1. **Steam Settings > System > Enable Developer Mode.**
2. In the **Developer** section, at the bottom, **Set User Password**.
3. From another machine on the same network: `ssh steamos@frame`, with the password from step 2. `frame` is the
   hostname (Steam Settings > System > Hostname); use the IP address if the name doesn't resolve. Remote Desktop
   works too: an Xorg session, user `steamos`, same password.

## 2. Check the toolchain

```sh
clang --version && cmake --version && ninja --version && git --version
```

The image the macOS guest runs (stable 20260922) has them all. If one is missing on your Frame's OS version, Valve's
debugging page describes `sudo steamos-readonly disable` and then `sudo pacman -S <package>`; an OS update reverts such
changes, but the FEX you build lives in your home directory and inside Steam's tool, not in the OS image.

## 3. Build and install

Build on the Frame itself (the build is tuned for the CPU it runs on; don't build elsewhere and copy). It takes a while
and a few GB in your home directory; keep the Frame on its charger.

```sh
git clone https://github.com/StonkGuy/eac-arm64-emulation-poc.git
cd eac-arm64-emulation-poc/scripts/steamos
sh build-fex.sh                  # clones StonkGuy/FEX at the pinned tag, builds, stages into ~/fex-eac-work/stage
sh install-fex-tool.sh           # swaps it into Steam's FEX tool; backs up Valve's files first
sh install-fex-tool.sh status    # which build is installed
sh install-fex-tool.sh restore   # put Valve's build back
```

`install-fex-tool.sh` refuses to swap while a game is running (FEXServer holds the old binaries open): quit games first.

## 4. Launch options

Steam > the game > Properties > Launch Options. For VRChat (app ID `438100`):

```
env EAC_LAUNCHERDIR=$HOME/.local/share/Steam/steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Roaming/EasyAntiCheat PROTON_EAC_RUNTIME="$HOME/.local/share/Steam/steamapps/common/Proton EasyAntiCheat Runtime" WINEDEBUG=-all PROTON_USE_XALIA=0 %command%
```

For another EAC game, put its app ID in place of `438100`. If a game's Wine seccomp filter install fails (an
anti-tamper client that crashes or hangs at a `syscall`), add `FEX_NEEDSSECCOMP=1`, a stock FEX option.

Just in case: if VRChat stalls before joining a world, add `WINE_CPU_TOPOLOGY=16:` followed by 16 CPU ids, the Frame's
real ones (`nproc`) repeated, e.g. `16:0,1,2,3,4,5,6,7,0,1,2,3,4,5,6,7`. It makes IL2CPP size its thread pool for 16
CPUs; the macOS guest needs it, the Frame may not.

## 5. After an update

A Steam update of the FEX tool puts Valve's build back. `sh install-fex-tool.sh status` shows which one is active;
run `sh install-fex-tool.sh` again to restore the patched one.

## Troubleshooting

* `210 'Unexpected error. (#1)'` about 3 s after `Starting Wine module mapping`: the installed FEX is older than patch
  0033; rebuild and reinstall.
* `stage incomplete` / `install incomplete` from `install-fex-tool.sh`: the build did not finish; re-run `build-fex.sh`.
* A tree from an older script version: `build-fex.sh` refuses it and prints the `rm -rf` to run.
* `eac-tests/ptrace-inject` in [StonkGuy/FEX](https://github.com/StonkGuy/FEX/tree/eac-2610/eac-tests) is the quickest
  check that the emulation behaves.

More in [troubleshooting.md](troubleshooting.md). What each patch does: [patches.md](patches.md).
