# The "connection timed out" disconnects

Status: **the time-outs are explained and gone with the signal-mask fix** (`FEX_SIGNALMASKFIX`, patch 0007,
on by default; patch 0008 removes a second bug of the same family). This is a one-game, one-machine proof of concept: "gone" means 0 time-outs in the 16-session A/B and in
the 59 sessions run since ([Real-game A/B](#real-game-ab)), not "proved universal". A rarer **pre-join stall** (a ~60 s freeze at the Photon region lookup, then a `Disconnecting` rejoin
loop) is a different problem with a different fix: IL2CPP thread-pool starvation on a 4-vCPU guest, fixed by reporting
more CPUs to the game — see [The pre-join stall](#the-pre-join-stall-il2cpp-thread-pool-starvation). Written so the
investigation can be repeated on another machine.

## Symptom

About every second session that reaches a world ended with `OnDisconnected: ClientTimeout` ("Your connection to VRChat timed
out"), always **25–29 s after `Finished entering world`** (41–50 s after `AntiCheat Session Begin`). Separately, some launches froze at
start-up (after `Successfully connected to Stomp` or `OnRegionListReceived`, before `Destination set`) with the main thread asleep and no CPU use — the pre-join stall, a separate cause covered below.

Measured over the first 35 sessions on an M2 Air (stock FEX behaviour): 15 timed out, 14 were fine for the whole observation
window (≥ 75 s of log), 6 hung at start-up. Sessions shorter than ~75 s of log cannot show the timeout and must not be counted as
healthy (`tools/harness/classify_runs.py` marks them `short`).

## What was recorded

`tools/harness/timeline.py` (inside the VM, twice a second) logs every thread's CPU ticks and state, the receive queue of each
connected UDP socket (`/proc/net/udp`) and, once a queue backs up, every thread's syscall and wait channel.
In a session that timed out:

1. The UDP socket to the Photon game server (remote port 27002) is created together with its receive thread (an
   `IL2CPP_Threadpool` thread). That thread drains packets at ~60 per second.
2. At the moment the world finishes instantiating (≈ 1 s before the forced garbage collection `[GC] Ensuring the heap is at
   least 512MB`, ≈ 3 s before `Finished entering world`) it stops reading. Its receive queue grows by ~30 KB/s to 216 576 bytes
   (the socket buffer) and stays there for the rest of the session, even after the time-out and the rejoin.
3. From then on that thread is in `read()` on a pipe (Wine's wait for a wineserver reply) and uses no CPU. Two other threads
   (a named-pipe server thread and a websocket reader) are in the same state; most other threads wait in `futex_waitv`
   (Wine's fsync waits) or `futex`.
4. The render loop, the other network threads and the wineserver keep running normally (the wineserver sits idle in `epoll_wait`).

So the receive thread's wait for its receive to complete is never satisfied although data is queued on the socket.

The wineserver side was captured in a second session that wedged (`tools/harness/wedgewatch.py`, which reads only wineserver's
`/proc`): 14 s after the socket appears wineserver's epoll set watches the socket with events `0x1b` (IN|PRI|ERR|HUP, a receive
is pending); at the wedge the same registration is `0x18` (ERR|HUP only) with 213 KB queued and it stays that way 24 s later.
So wineserver has no receive pending for the socket while the client thread is still waiting for one: a wake-up (the
completion of the async receive) was delivered to the client's wait protocol and lost on the way. The threads parked in the
reply wait (`read` of the 16-byte `wake_up_reply`) at that moment were the Photon receiver (an `IL2CPP_Threadpool` thread), a
websocket reader and the named-pipe server thread, i.e. exactly the threads that block in server-side asynchronous I/O.

## Reproduced outside the game

`tools/harness/wedge_repro.py` (Windows CPython under Proton) recreates the conditions with no game and no anti-cheat: ~125
threads blocked in the same kinds of waits (events, sleeps, a blocking UDP `recvfrom`, pipe and TCP readers that are poked
every 0.3 s) plus a "collector" thread that does what Boehm GC does at a world stop: `SuspendThread`, `GetThreadContext`,
`ResumeThread` on every thread, back to back. Results on the M2 Air (each run 75 s, ~2 000–7 000 world stops):

| configuration | runs that wedged |
|---|---|
| Wine fsync (Proton default) | 3 of 3 control runs, within 300–2 500 stops |
| fsync, FEX host handlers without `SA_RESTART` | 2 of 2 |
| `PROTON_NO_FSYNC=1` (server-side waits) | 0 of 2 (≈ 14 000 stops) |
| fsync, control with the signal trace on (patches 0001–0007) | 5 of 6 |
| fsync, `FEX_SIGNALMASKFIX=1` (patches 0001–0007) | **0 of 6** (Fisher exact p = 0.015 against the control) |
| fsync, control with the signal trace on (patches 0001–0008) | 6 of 6 |
| fsync, `FEX_SIGNALMASKFIX=1` (patches 0001–0008) | **0 of 6** (p = 0.002) |

All runs in the table used `--period 0` (a world stop immediately after the previous one, ≈ 2 000–7 000 stops in 75 s). With
the default 0.25 s between stops nothing wedged (eight runs, ≈ 270 stops each), so the pressure matters.

Two kinds of wedge show up: a reader thread blocked in `recv` stops waking although it is fed every 0.3 s (the game's
symptom), and the collector blocks forever in `GetThreadContext` because one target never reacts to the suspend signal.
In the wedged state 116 of 127 threads are parked in `read(fd, buf, 16)` (Wine's wait for a wineserver wake-up, where a
suspended thread sits) and nobody has a pending or blocked `SIGUSR1`: the signal was consumed.

Why only with fsync: Wine suspends a thread with `SIGUSR1`. A thread blocked in a server wait can also be stopped by the
server (it answers the wait with a kernel APC), but a thread blocked in an fsync `futex_waitv` can only be stopped by
interrupting the wait with the signal, so fsync exercises FEX's guest signal handling far more.

## The FEX deviation

FEX runs a guest signal handler with the host mask `sa_mask | signal` instead of Linux's `interrupted mask | sa_mask |
signal`, and its guest-visible mask (`CurrentSignalMask`, what `rt_sigprocmask` reads and what Wine's
`server_enter/leave_uninterrupted_section` saves and restores) is never updated when a handler is entered or restored at
`rt_sigreturn`. A handler that saves "the old mask", blocks signals for a server call and then restores what it saved therefore
unblocks the signal it is running for, and `SIGUSR1` can nest inside its own handler while the thread is parked in a suspend.
Wine's server keeps a stack of waits per thread and the client has special code for replies "stolen" by a nested wait, but that
is written for the depth Linux allows. Stock FEX behaves the same way: 2609.1, and FEX-2610 (released 2026-10-07), whose `SignalDelegator.cpp` is unchanged from 2609.1.

`FEX_SIGNALMASKFIX=1` makes FEX behave like Linux: the handler runs with `interrupted | sa_mask | signal` (minus the signals FEX
needs unblocked), `rt_sigprocmask` inside the handler sees that, and `rt_sigreturn` restores the interrupted mask and re-raises
anything that became unmasked while it was recorded as pending. Without the handler's `uc_sigmask`/`rt_sigprocmask`
step, the reproducer above wedges whether or not the trace is on. On a writable build only the control arm is run without it
(`FEX_SIGNALMASKFIX=0`), since the fix and the trace share the one patch whose default is `true`.

`FEX_SIGNALTRACE=1` records every host signal arrival, masking decision, delivery, `rt_sigreturn`, `rt_sigprocmask` made inside
a handler and `kill`/`tgkill` call into `/dev/shm/fex-<pid>-sigtrace`; `tools/harness/sigtrace.py` follows a signal through
the sender's and the receiver's rings. In the first wedged run every one of ~23 000 `SIGUSR1` arrivals had been delivered to a
guest handler (nothing masked, deferred or re-raised), so the loss is not at FEX's masking check.

## Real-game A/B

16 sessions in one VM, strictly alternating, 155 s each, the arm's environment injected through Proton's `user_settings.py`
(`tools/harness/classify_runs.py` classifies the saved game logs). Patches 0001–0007 only (the signal-mask fix is the only
difference between the arms):

| arm | time-out | start-up hang | fine |
|---|---|---|---|
| control | 4 | 1 | 3 |
| `FEX_SIGNALMASKFIX=1` | **0** | 1 | 7 |

Among the sessions that reached a world: time-outs 4 of 7 against 0 of 7 (Fisher exact p = 0.07); counting hangs too, failures
5 of 8 against 1 of 8 (p = 0.12). The A/B alone is small, but it agrees with the reproducer above and with the mechanism.

Since the fix became the default, every session has run with it: **59 sessions** that reached a world and were observed
for at least 40 s after entering it (the time-out always hit 25–29 s in), **none** timed out — against about one in two
before the fix. (They are the sessions of the later A/Bs and censuses; one session timed out 105 s after entering a world,
which is not this failure's signature.)

## The pre-join stall: IL2CPP thread-pool starvation

A **pre-join stall** looks nothing like the time-out and is **not fixed by the mask fix**. In it the game freezes for a
fixed ~60 s (sometimes ~110 s) while looking up the Photon region, then switches region with the Photon client stuck in
`current state: Disconnecting`, fails with `Failed to connect to region, status was: Disconnecting` and retries every
~31 s. Some sessions recover; others keep looping and can end in an abrupt exit. A census of **99 sessions** put its rate
at **~8 %**, arriving in batches.

**Cause:** the game's IL2CPP managed thread pool starts with as many workers as Windows reports CPUs. The muvm guest gets
the host's **performance cores only — 4 vCPUs on an M2** — so the pool starts with 4 workers, where a desktop PC reports
8–32 CPUs. During start-up a burst of work items blocks synchronously on asynchronous work; when the burst
exceeds the pool, the continuation that would post the next receive on the Photon NameServer websocket cannot run, the
NameServer's reply sits unread in the socket, and the main thread waits until a ~60 s timeout gives up. The pool's
starvation monitor adds about two workers a second the whole time, none of which helps.

**Fix:** report more CPUs to Windows code with Proton's `WINE_CPU_TOPOLOGY` (an upstream Proton setting, no patch):

```
WINE_CPU_TOPOLOGY=16:0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3
```

This tells the game it has 16 logical CPUs mapped onto the guest's 4, so the pool starts with enough workers. It does not
change how many CPUs actually run the game. `scripts/set-launch-options.sh` adds it.

### Evidence

**1. The freeze is quantized.** The time from `Locating best region` to `Got best network region` in every saved log:

| sessions | region lookup |
|---|---|
| 152 healthy | 2–10 s (median 4) |
| 25 of 26 stalled | **62–68 s** (18 sessions) or **107–113 s** (7) |

(The 26th stalled session found its region in 4 s and stalled later, at the region connect.) Healthy and stalled
sessions do not overlap, and the stalled values are a fixed timeout (sometimes hit twice), not slow
networking. The whole Unity log is silent for that time.

**2. The game is idle, not busy.** `tools/harness/stallwatch.py` fires when the region lookup has not finished 12 s after it
started, and takes two in-process thread snapshots (patch 0006) 28 s apart. In both, the main thread is in the *same*
timed wait (`NtWaitForAlertByThreadId`, same futex, same timeout argument), and 94 of 96 threads have not moved —
rendering, DXVK and the job system are all parked. Meanwhile **49 new `IL2CPP Threadpool worker` threads** appeared,
~1.75 per second, each going straight into a wait.

**3. Every stall shows the pool exploding.** `tools/harness/poolmon.sh` counts the pool's threads once a second. A stalled
start-up: 6 workers at launch, steady growth of ~2 per second from t ≈ 13 s to t ≈ 61 s (110 workers), then growth stops
at the moment the timeout releases the waits. A healthy start-up stays at 4–11 workers. Earlier stall snapshots
show 46–137 pool threads, growing between its two snapshots; in-world snapshots show 13.

**4. The socket side.** At every captured stall the Photon NameServer socket (`ns.photonengine.io`,
port 443) holds ~49 unread bytes, and wineserver has it registered for `EPOLLPRI` only
(`/proc/<wineserver>/fdinfo` shows `events: 1a`). `tools/harness/wsmem.py` reads wineserver's own `struct sock` state
passively; read during a stall's retry loop (by then the NameServer socket itself is closed), the game's other `0x1a`
sockets have no event-select mask, no queued receive and no poll request of their own — so their `EPOLLPRI` can only come
from a pending `select()` that lists them for errors, not for reading. `0x1a` with queued data therefore means **the
application has no receive outstanding**; wineserver is right not to poll for input. That is what a starved continuation
looks like from the socket side.

**5. The fix, A/B-tested.** Interleaved in one VM boot (the arms alternate every launch, so time-varying server or host
conditions hit both equally), recording the pool size and the region time of every launch:

| arm (interleaved, one VM boot) | launches | stalled | region lookup | peak IL2CPP pool |
|---|---|---|---|---|
| default — 4 CPUs reported | 8 | **5** | 4–7 s when it joined; **64–112 s** when it stalled (this A/B) | 7–11; **96–110** when stalled |
| `WINE_CPU_TOPOLOGY=16:…` — 16 CPUs reported | 6 | **0** | **3 s, every launch** | 13–18 |

Fisher's exact test, one-sided: p = 0.028 (0.09 counting only the six strictly alternating pairs). The default arm's
stall rate in this stretch (5 of 8) was far above the long-run ~8 %, which is the batching seen in every census; the
treatment never stalled and never let the pool grow, and its region lookup was a flat 3 s — the pool never ran short.
Delivered the way it ships — in the launch options written by `scripts/set-launch-options.sh` — the first
confirmation launch joined the same way: region found in 3 s, pool peak 13.

## How to capture and read a wedge

1. Run the game with `FEX_PROFILESAMPLEHZ=1 FEX_PROFILESAMPLEALLTHREADS=1` in its environment (this only enables the
   in-process snapshot support; nothing is sampled until triggered).
2. Inside the VM as root: `python3 tools/harness/wedgewatch.py <label> <seconds>`. It takes a *baseline* of wineserver's
   epoll registration for the game-server socket 14 s after it appears (`snapepoll_<label>_baseline.txt`), and when the socket's
   receive queue stays above 64 KB for 6 s, or the main thread sleeps for 25 s, it triggers a FEX thread snapshot
   (`touch /dev/shm/fex-<pid>-snapshot`) and copies the snapshot, `maps`, thread names, per-thread syscalls and the epoll state at
   the wedge (`snapepoll_<label>_wedge1.txt`).
3. `python3 tools/harness/resolve_snap.py snap_<label>_1.bin snapmaps_<label>_1.txt snapnames_<label>_1.txt` prints, per
   thread, the x86 syscall it is blocked in with arguments, and the return addresses on its stack as `module!export+offset`.
4. For rates: `tools/harness/classify_runs.py <prefix>` over saved game logs; `tools/harness/analyze_tl.py` correlates a
   `timeline.py` recording with FPS and host counters (`hosttl.py`).

`tools/harness/wedge_repro.py` is the standalone Windows-Python program behind the table above (run it under Proton with the
embeddable CPython; `proton run` returns no stdout, so pass `--logfile`). Default `--mode gc` is the world-stop pattern
(blocking UDP receiver, pipe and TCP readers that are poked periodically, idle and sleeping threads, and a collector thread doing
`SuspendThread`, `GetThreadContext`, `ResumeThread` on every thread); `--mode newrecv` repeatedly creates a connected UDP socket
plus a thread doing a blocking receive while data is already flowing. It reports the first reader that stops responding and dumps
every thread's Python stack with `faulthandler`.
