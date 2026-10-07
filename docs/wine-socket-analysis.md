# The pre-join stall: what wineserver's socket state proves

Analysis against the exact Wine in use — ValveSoftware/wine branch `experimental_11.0`, matching Proton
`experimental-11.0-20261001`. File:line references are to that branch. The evidence comes from passive `/proc` captures
of already-saved stall sessions; no debugger is attached to the game (Easy Anti-Cheat reports that).

## What `/proc/<wineserver>/fdinfo/<epfd>` actually reports

A quick experiment pins the encoding (a socket registered `EPOLLPRI` alone reads back `events: 1a`; `EPOLLIN` alone
reads back `19`; both read back `1b`; neither reads back `18`). So for a wineserver-held socket:

```
events = (kernel EPOLL registration) | 0x18        (0x18 = EPOLLERR|EPOLLHUP, always set)
```

`0x1b` → registered for `EPOLLIN|EPOLLPRI`  (healthy "armed for read")
`0x1a` → registered for `EPOLLPRI` only       (NOT armed for read, even with data queued)
`0x18` → registered for neither read nor oob

## Why `0x1a` happens (`server/sock.c`)

`sock_get_poll_events()` (line 1540) computes the registered mask as `sock->mask & ~sock->reported_events`
(line 1543) and, for a connected TCP socket with no queued receive async (line 1620-ish), arms:

* `POLLIN` only if `mask & AFD_POLL_READ`
* `POLLPRI` if `mask & AFD_POLL_OOB`

So `EPOLLPRI` with no `EPOLLIN` is exactly the state where **`AFD_POLL_READ` is set in `sock->mask` but also set in
`sock->reported_events`** — i.e. wineserver has already delivered `FD_READ` to the app, and is waiting for a
`recv_socket` to clear and re-arm it. `AFD_POLL_READ = 0x0001`, `AFD_POLL_OOB = 0x0002`
(`include/wine/afd.h:67-68`).

`FD_READ` is posted by `post_socket_event()` (line 1350), which sets `reported_events |= event` and only calls
`set_event()` when `sock->mask & event`. It is cleared in exactly two handlers:

* `DECL_HANDLER(recv_socket)` line 4029 — the normal re-arm on every `recv`/`WSARecv`/`ReadFile`.
* `DECL_HANDLER(socket_get_events)` (WSAEnumNetworkEvents) clears `pending_events`, **not** `reported_events`
  (line 4197), and `IOCTL_AFD_WINE_MESSAGE_SELECT` clears both only when re-arming a window (line 3015).

**Conclusion: at the stall, both stuck sockets have `FD_READ` reported and not yet re-armed. wineserver signalled
"readable" and is waiting for the game to call `recv`; the game has not, while 49 and 46 bytes sit in the receive queue.**
This is a stalled read-drain in the guest's managed/Wine handshake, not a byte that never arrived.

## Every receive path funnels through `recv_socket`

`dlls/ntdll/unix/socket.c`:
* `sock_recv()` line 917 issues `SERVER_START_REQ(recv_socket)` line 934.
* `sock_read()` line 1017 (ReadFile on a socket) → `sock_recv()`.
* `sock_ioctl_recv()` line 967 (`IOCTL_AFD_RECV` / `WSARecv`) → `sock_recv()`.
* The queued-async completion `async_recv_proc` → `try_recv` runs *server-side*, after `recv_socket` already queued it.

There is **no** client path that reads socket data without first issuing `recv_socket`, so a receive that bypassed the
server's re-arm is ruled out: the missing step is a `recv` that was never issued.

## Healthy vs stall, in the same captures

`wedgewatch.py` writes a `tfd:` line only for wineserver fds that have `Recv-Q > 0` (or a `:2700x` Photon endpoint),
so healthy baselines do carry a few of these lines. Across the captures:

* every *healthy* `*_baseline.txt` shows masks `0x1b` (armed) or `0x18` (neither) — **never `0x1a`**;
* every *stall* `*_wedge*.txt` with data shows `0x1a` with `Recv-Q > 0`: the Photon NameServer (`216.120.180.19:443`)
  and one VRChat API socket (Cloudflare IPv6, `2606:4700::6812:1a24`/`1b24:443`), byte counts identical across stalls
  (49 and 46 B; 92 B in the second snapshot of each pair).

Two sockets in event-select mode go stale **together**, at the same protocol message. That is the signature of one
selector thread that stopped draining both, not of two independent per-socket bugs.

### The sockets stay stuck over ~2 s and are dropped without ever being read

The first stall of the `PROTON_NO_FSYNC` arm (`snof2`) has two snapshots ~2 s apart:

| socket | snapshot 1 | snapshot 2 |
|---|---|---|
| Photon NameServer `216.120.180.19:443` | `ESTAB`, Recv-Q **49**, epoll `0x1a` | **`CLOSE-WAIT`**, Recv-Q **0** — FIN received, 49 B discarded unread |
| VRChat API `[2606:4700::6812:1a24]:443` | `ESTAB`, Recv-Q **46**, `bytes_received` 8925, `lastrcv` ≈ **2.7 s**, epoll `0x1a` | `ESTAB`, Recv-Q **92**, `bytes_received` 9039 (90 more bytes arrived) |

Both stay in `0x1a` with data queued for the whole interval: the reader never drains them, the NameServer eventually
sends FIN, and the 49 queued bytes are thrown away when the socket closes. The API socket gains 90 more bytes over the
same interval and is still not read. So the stall is not a one-tick scheduling hiccup — the read side is genuinely
stalled, and the client recovers only by tearing the connection down and retrying (the ~31 s region retry).

## The two candidate mechanisms still standing

* **The selector thread was not woken (favoured).** `post_socket_event` set the event, but the IL2CPP selector thread
  waiting on it (under fsync, an `NtWaitForSingleObject`/futex wait) never resumed, so it never called
  `WSAEnumNetworkEvents`/`recv` for those two handles. Other sockets whose events fired on a different wait keep
  working. Predicts: `+server`/`+winsock` shows the last `FD_READ`/`set_event` with no following enum/recv for those
  handles, and the selector thread parked in futex.
* **The app observed `FD_READ` and chose not to recv** (a selector that rebuilt its handle set and dropped one).
  Predicts: the trace shows the enum/wait returning for those handles, then no `recv`.

The first is favoured because two independent sockets stall in lockstep. A Wine-level `+winsock`/`+server` trace decides
between them by the single question: **after the last `FD_READ` post, was the app woken and did it act?**

## Race windows to instrument

1. `post_socket_event` (1350) → `set_event( sock->event )` → does the `NtWaitForSingleObject` waiter wake under fsync?
   (Proton's fsync `set_event` path — check the event's `wake_up`/shm signalling.)
2. `IOCTL_AFD_EVENT_SELECT` re-arm (2960-3000): between `sock->mask = mask; sock_reselect()` and the
   `if (event && (sock->pending_events & mask)) set_event(event)` — can a `pending_events` set in that window be lost?
3. `recv_socket` (4029/4041): clear `reported_events`, requeue, `sock_reselect` — can the read async be queued while
   `sock_reselect` still computes a READ-reported mask (ordering between line 4029 and 4041)?
4. `socket_get_events` (4197): clears `pending_events` but not `reported_events`; a second `FD_READ` that arrives
   between the server's read of `pending_events` and the clear could be dropped.

## What this means

The ~60 s bimodal timeout and the shared `AFD_POLL_READ`-reported state on two independent managed sockets place the
fault in the socket-layer read-drain / selector wake, not in the EOS transition (the websocket `CLOSE-WAIT` at the stall
is the Photon NameServer dropping a connection that sat idle through the ~60 s — a consequence, not the cause).
