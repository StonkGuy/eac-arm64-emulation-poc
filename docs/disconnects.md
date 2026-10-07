# The "connection timed out" disconnects

Status: **the time-outs are explained and, in the sessions run so far, gone with the signal-mask fix** (`FEX_SIGNALMASKFIX`, patch 0007,
on by default; patch 0008 removes a second bug of the same family). A rarer **pre-join stall** (a silent
~60 s gap, then the region connect fails and the client retries) is a different problem and is **not fixed** — see
[The pre-join stall](#the-pre-join-stall-is-something-else). Written so the investigation can be repeated on another machine.

## Symptom

About every second session that reaches a world ended with `OnDisconnected: ClientTimeout` ("Your connection to VRChat timed
out"), always **25–29 s after `Finished entering world`** (41–50 s after `AntiCheat Session Begin`). Separately, about one launch
in seven hangs ~8 s into the game log (after `Successfully connected to Stomp` or `OnRegionListReceived`, never reaching
`Destination set`) with the main thread asleep and no CPU use.

Measured over the first ~40 sessions on an M2 Air (stock FEX behaviour): 15 timed out, 14 were fine for the whole observation
window (≥ 75 s of log), 6 hung at start-up. Sessions shorter than ~75 s of log cannot show the timeout and must not be counted as
healthy (`tools/harness/classify_runs.py` marks them `short`).

## What it is not

* **Not the power throttle.** The time-out happened with the CPU at full speed and full frame rate (detail in the local notes).
* **Not `yt-dlp.exe`**, although it runs at the same time (it starts when the world has loaded). Sessions where it took 30 s
  were fine and sessions where it took 10 s timed out. The wedge below starts *before* it is launched.
* **Not the measurement tools.** Sessions with no recorder at all time out as often.
* **Not memory pressure.** No swap, reclaim or major-fault differences between healthy and failing sessions.
* Not related to world content timings (instantiate, GC and join times are the same in good and bad sessions).

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
is written for the depth Linux allows. Stock FEX (2609.1, and its main branch when this was written) behaves the same way.

`FEX_SIGNALMASKFIX=1` makes FEX behave like Linux: the handler runs with `interrupted | sa_mask | signal` (minus the signals FEX
needs unblocked), `rt_sigprocmask` inside the handler sees that, and `rt_sigreturn` restores the interrupted mask and re-raises
anything that became unmasked while it was recorded as pending.

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
5 of 8 against 1 of 8 (p = 0.12). The sample is small, but it agrees with the reproducer above and with the mechanism, and no
session with the fix behaved differently from the unpatched ones in any other way.

## The pre-join stall is something else

A **pre-join stall** looks nothing like the time-out and is **not fixed by the mask fix**. A census of **99 sessions**
puts its rate at **~8 in 99 (≈8 %), not "1 in 5"**, and it arrives in **batches** rather than uniformly. In this stall
the VRChat log goes silent for **~60 s** between `Successfully connected to Stomp` and `Destination fetching`
(healthy sessions take ~1 s), then switches region with the Photon client stuck in `current state: Disconnecting`
(healthy: `ConnectedToNameServer`), fails with `Failed to connect to region, status was: Disconnecting`, and retries every
~31 s. Sometimes it resolves slowly and the session proceeds; sometimes it loops. It has **two shapes**: most sessions
stall *before* `Destination fetching`, one observed *between* `Destination fetching` and `Destination set`. The string
`current state: Disconnecting` is a **perfect marker** — exactly the stalling sessions print it, and every other session
opens with `ConnectedToNameServer`. It happens in **both** arms of the A/B (`ab54`/`ab59` control, `ab62` with
`FEX_SIGNALMASKFIX=1`), so it is not the signal-mask bug.

A thread snapshot taken at the stall (no ptrace, `FEX_PROFILESAMPLEHZ=1`) shows:

* the parked threads (a `WebSocketClient` and the `VRCNPServer`) are in `read(fd, buf, 0x10)` on a **pipe** — Wine's
  `wait_select_reply` for a server-call reply — not on a socket;
* a **Cloudflare `:443` socket** of the game has ~5 KB of unread data with **no thread reading it** and no thread in
  `recvfrom`/`recvmsg`;
* wineserver is idle in `epoll_wait` holding that socket, and its per-fd registration for it has gone from
  `events: 1b` (the kernel ORs in `ERR|HUP`, so `1b` = `EPOLLIN|EPOLLPRI`) to `events: 18` (nothing registered) with data
  still queued.

So the client is waiting for a completion that the server never posts. The FEX epoll/eventfd path was ruled out (it is a
plain pass-through: `x64/EPoll.cpp`, and `EPOLLERR|EPOLLHUP` are always ORed in by the kernel, so the `0x18` alone is not
proof of loss). The earlier "start-up hang" snapshots (~140 threads in `NtWaitForAlertByThreadId`, 13 in `futex_waitv`,
none in the reply read) are a third variant of the same "the game's own threads stop making progress at start-up" family.

### Traced: the stall is not a FEX bug

A campaign with the arm that also sets `FEX_SIGNALTRACE=1` (so every FEX process, including the x86-64 wineserver,
writes a ring) caught the stall on the first session. With a corrected snapshot decode (the record grew to 1280 bytes in
this build; `tools/harness/resolve_snap.py` now auto-detects 640 vs 1280) the picture is:

* Only **two** threads are genuinely parked in a read: `WebSocketClient` (fd 294) and `VRCNPServer` (fd 316), both on an
  **empty server-reply pipe** (the game holds the read end, wineserver the write end). Every other thread — including the
  main thread — is in `futex(FUTEX_WAIT)` on Wine's `NtWaitForAlertByThreadId`.
* Every futex-blocked thread has the **futex word equal to the value it is waiting for** (118/118, then 167/167): nobody
  woke it, and there is no "wake-up was lost" signature.
* `SIGUSR1` is fully delivered — 683 of 683 arrivals reached a guest handler (0 masked); only 3 were deferred.
* wineserver's epoll held the game's `:443` socket (`ino 17758`) with 49 bytes unread at the first snapshot and the socket
  in `CLOSE-WAIT` at the second, yet **no 16-byte reply write to either parked pipe appears anywhere in the ring**.

FEX's futex syscall is a raw host pass-through (`Passthrough.cpp:474`, one `svc #0`; the guest address, op, expected
value and bitset are forwarded verbatim, `-EINTR` is returned as-is) — there is no FEX code path between a host
`futex_wake` and the waiter, and none that could swallow a pipe write. So the missing wake-up is **server-side
(wineserver/EOS) or environmental**, not in the ptrace/signal/syscall emulation. The one thing still unproven is
wineserver's own state — a passive `/proc/<wineserver>/{syscall,fdinfo}` probe (no ptrace, so EAC-safe) is now recorded
at each wedge to close that gap. The signal fixes in this series do not remove this stall, and are not expected to.

### Captured in the act: it is a *quiet* 61 s hang at the Stomp→world-fetch hand-off

A socket-level capture of a live stall (launch loop that watches the log for a >12 s quiet gap past
`Beginning room transition`, then snapshots `ss -tinep` + `/proc/net/tcp` + `/proc/net/snmp` inside the guest and
`ss`/SNMP on the host; `sandbox/capture_stall.sh`) narrows the "environmental" side further. The stalled log's **last
mutable line** is `[LogEOSMessaging] Successfully connected to Stomp` (plus one `Websockets API connected`); nothing after
it for 61 s. The socket state at that instant:

* **No flow to the destination is ever opened.** The game holds no connection to the EU region endpoint — the stall is
  between "world info fetched" and "region join", i.e. the client never gets as far as dialling Photon.
* **The only stalled TCP flow is the websocket's own connection**, `192.168.1.102:55580 → 216.120.180.19:443`, in
  `CLOSE-WAIT` (peer — the VRChat/EOS websocket host — sent FIN; the client never closes). Its peer is *not* idle: it is
  the same host as the Photon master (`216.120.180.19`), so a server-side FIN here is an intentional disconnect.
* **A backpressured read is sitting unread**: `[2606:4700::6812:1a24]:443` (Cloudflare) with **Recv-Q 138 and no thread
  in `recvfrom`/`recvmsg`** — data arrived and the reader is parked in the reply wait, not on the socket, exactly the
  documented wedge.
* **The transport is clean, and the dead flows are dead.** Every VM flow on the host is `mss:1448 pmtu:1500 rtt<20ms`.
  Two `FIN-WAIT-1` flows to `2.23.90.177:443` (Akamai) sit at `backoff:8` (`rto:53760`, `lastrcv:lastack≈385 s`) and have
  **no guest-side counterpart in `/proc/net/tcp`** — stale host sockets from a connection the guest has already abandoned,
  not the stall (their `last*` ages predate the stall by minutes).

So the stall is a **cause, not a transport failure**: a server-side/EOS state transition (the websocket FIN, then a join
handshake the client never starts) hits the same client-side reply-pipe wedge. Two candidate triggers, both testable
without ptrace:

1. **`PROTON_NO_FSYNC`** — the reproducer above wedges with Wine fsync (server-side waits) and **not** without it, so the
   EOS/websocket threads that block in server-side async I/O are exactly the ones whose completion is lost. Untested on
   the real game for the *stall* (only for the time-out).
2. **EOS SDK config / Stomp reconnect** — `ScheduleNextSDKConfigDataUpdate … Update Interval: 310.58` is always printed at
   this point; if the update lands during the transition the client may be juggling Stomp and the world fetch at once.

### MTU was necessary but not sufficient (and a WiFi-power-save candidate)

The guest virtio-NIC comes up at MTU 65520 while the real path is 1500, which produces ~43 KB TCP super-segments
(retransmitted) and drops large UDP datagrams; that alone causes a stall-shaped failure and is now fixed at boot
(`passt -m 1500`, or `--passt-args=-m1500` to muvm — a single token so it survives the unquoted `$MUVM_ARGS`
interpolation in `scripts/vm/vm_up.sh`). With the guest at MTU 1500 the transport is clean — UDP to the Photon master
(216.120.180.19) is 0 % loss for 512/1200/1400/1472-byte datagrams, external TCP is `mss:1460 pmtu:1500`, DNS 40/40
and HTTPS 40/40 sub-second — **and the stall still happens**, at either sub-step (`Requesting join token`, i.e. HTTPS,
or `Connecting to realtime network`, i.e. the UDP region connect). So MTU is a real sub-case, not the whole story; the
mechanism above (client parked on a server-reply pipe) is unchanged.

One host-side candidate — WiFi power-save (bursty link latency vs the join deadlines) — was **tested and rejected**. An
interleaved A/B with the network interface bounced between power-save `off` and `on` every launch in a single VM, so any
time-varying server/host condition hit both arms equally: 16 launches gave **power-save on 7 join / 1 stall, power-save
off 7 join / 1 stall** — identical. Every log was re-checked by hand (a stall is `Finished entering world` = 0 with a
`current state: Disconnecting` line; a join is the reverse). The stall rate here was 2/16 (≈ 13 %), in line with the
census, and independent of the interface state. So power-save is not the trigger; the environmental cause is still open.

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
