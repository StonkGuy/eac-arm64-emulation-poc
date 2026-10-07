# Troubleshooting

**The game "crashes" and the whole Steam VM disappears.** Check `journalctl -k | grep -i "out of memory"` on the host.
The kernel OOM killer takes the VM first (it is the biggest process). Apply `scripts/host-tune.sh`, cap the VM
(`VM_MEM_MB`, `VM_VRAM_MB`), close the browser, and shut the VM down between sessions.

**Steam asks "Steam Remote Play – we'll stream it to you from a computer named …".** The same Steam account is already
running VRChat on another machine. Cancel; Steam does not start a second copy.

**Holding W stops the mouse.** libinput *disable-while-typing* on the laptop trackpad — see `tuning.md` §2.

**Unity crash dialog / frozen window, `Crashes/Crash_*/crash.dmp` appears.** `tools/mdparse.py crash.dmp` prints the
exception, faulting address and registers. An access violation at `0x14` inside a `/memfd:` mapping is the EAC
in-game client dereferencing its own null singleton (seen once in five launches; likely an init/teardown race inside the
client). After such a crash `UnityCrashHandler64.exe` keeps the process alive, which looks like a freeze: kill the game.

**Disconnect ("Your connection to VRChat timed out") ~25–29 s after "Finished entering world".** Before the signal-mask fix
(patch 0007, on by default) roughly every second session that reached a world ended this way, at a very regular
delay after the join and at full speed. What happens: the UDP socket to the Photon game server
(remote port 27002) starts accumulating unread data when the world instantiation finishes, fills its receive buffer (216 576
bytes) and stays full while the render loop keeps running; the thread that should read it is parked in Wine's wait for the
wineserver's 16-byte wake-up reply and never gets it. The cause is FEX running guest signal handlers with the wrong signal
mask, which loses a wake-up when Wine suspends and resumes threads. Check that you run the patched FEX with the default
`FEX_SIGNALMASKFIX` (not `=0`); `tests/signal-mask` tells you in a second. Evidence, the reproducer and how to capture a wedge:
[disconnects.md](disconnects.md).

**The game hangs ~8 s into the log, right before "Destination set"** (about 1 launch in 8, main thread asleep, no CPU use).
Not fixed: it leaves the same kind of footprint (a couple of threads parked in the wineserver reply read) but survives the
signal-mask fix. Close the game and start it again; [disconnects.md](disconnects.md) has what was captured.

**`Photon … ClientTimeout`, rejoin loops.** The game's UDP receive socket can be left with a full receive queue
(`ss -uanp` shows ~212 KB in Recv-Q) while the game is stalled: the time-out above.

**`Failed to connect to region` / a ~60 s pause after "Fetching world information".** Caused by the cost of invalidating
translated code in a process with ~200 threads; patch 0004 removed it. If it comes back, run with `FEX_PROFILESTATS=1` and look
at the invalidation counters (`docs/how-it-works.md`).

**EAC launcher fails (`Unexpected error (#1)` etc.).** Run `tests/ptrace-inject` first. If it fails, FEX is not the
patched build (note that binfmt handlers pin the interpreter inode: restart the VM after installing a new FEX).
Never leave `WINEDEBUG` trace channels or FEX logging knobs on while testing EAC: they change timing and have crashed the
game.

**EAC says 301, but the game dies ~3 s later and no new `output_log_*.txt` appears.** The Proton log (if enabled) shows
`err:virtual:virtual_setup_exception nested exception on signal stack` while `VRChat.exe` loads `kernel32.dll`. Seen with
the launch options `WINEDEBUG=err+all,+loaddll,+module,+seh PROTON_LOG=1 FEXHOTMEMFD=1`, on three different FEX builds;
restoring the plain launch options (`scripts/set-launch-options.sh`, `WINEDEBUG=-all`) fixed it at once. Check the
launch options first; do not swap FEX builds for this symptom.

**Never attach a debugger, `perf` or an strace-like tool to the running game.** EAC reports it as
"Debugger detected" and keeps reporting; use `docs/how-it-works.md` (in-process sampler) instead.

**Disk full.** Proton prefixes, shader caches and builds add up; btrfs gets slow above ~90 % use.
