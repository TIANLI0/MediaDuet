# Architecture

MediaDuet is two native apps that talk over a versioned, authenticated, encrypted
link protocol ([protocol/specification.md](../protocol/specification.md)).

```
┌──────────────────────────── Windows ────────────────────────────┐        ┌──────────────── Android ────────────────┐
│ MediaDuetUI.exe (WinUI 3, on demand)                            │        │ Compose UI (Kyant0 liquid glass)        │
│   media card · mini player · settings · pairing                │        │   now playing · connection · settings   │
│            ▲ named pipe, JSON lines (docs/ipc.md)               │        │            ▲ StateFlow                   │
│ MediaDuet.exe (native host, tray, autostart)                    │        │ Engine (Application singleton)          │
│  ┌───────────────┐ ┌──────────────┐ ┌───────────────────────┐   │ RFCOMM │  ┌────────────┐ ┌────────────────────┐  │
│  │ConnectionMgr  │ │GsmtcSource   │ │PlaybackArbiter (policy│◄──┼────────┼─►│RFCOMM srv  │ │MediaSessionMonitor │  │
│  │ BT first, LAN │ │SmtcBridge    │ │ host, state machine)  │   │  TCP   │  │TCP srv+NSD │ │(NotificationListener│ │
│  │ fallback      │ │Artwork cache │ │AudioActivityMonitor   │   │        │  │LinkSession │ │ access only)       │  │
│  └───────────────┘ └──────────────┘ └───────────────────────┘   │        │  └────────────┘ └────────────────────┘  │
│  LyricsService · LyricsOverlay/TaskbarWidget (D2D) · A2dpSink   │        │  LinkService (FGS connectedDevice), CDM │
└─────────────────────────────────────────────────────────────────┘        └──────────────────────────────────────────┘
```

## Process split on Windows

| | `MediaDuet.exe` host | `MediaDuetUI.exe` |
|-|-|-|
| Tech | C++20, Win32, C++/WinRT, CNG, Core Audio, D2D/DWrite | WinUI 3 (Windows App SDK, C++/WinRT, XAML) |
| Lifetime | always (login autostart, tray) | only while a window is open |
| Memory (measured, idle) | ≈ 7.5 MB private / 45 MB working set | WinUI runtime cost only when shown |

The background service never loads the WinUI runtime. Closing every window keeps
synchronisation, arbitration, the SMTC bridge and the desktop-lyrics overlay alive
(the overlay is drawn by the host with Direct2D on a per-pixel-alpha layered window
so it can be click-through and does not need the UI process).

## Module boundaries (Windows)

| Layer | Directory | Depends on | Notes |
|-------|-----------|------------|-------|
| util, crypto | `windows/core/{util,crypto}` | Win32, CNG | `IScheduler` abstraction makes everything testable with virtual time |
| protocol | `windows/core/protocol` | nlohmann/json | framing, envelope validation, limits |
| link | `windows/core/link` | protocol, crypto | `ITransport`, `Connection` (handshake + records), `LinkSession` (ids, de-dup, re-send), `ConnectionManager` (transport policy) |
| media model | `windows/core/media` | – | snapshot, commands, track id, position extrapolation |
| policy | `windows/core/policy` | media | `PlaybackArbiter` state machine |
| lyrics model | `windows/core/lyrics` | – | LRC / yrc parser (multi-stamp, offset, enhanced word timing), matching, karaoke timing (`Karaoke`), smoothed playback clock + lyrics delay (`LyricsClock`) |
| surface policy | `windows/core/overlay` | – | pure decisions of the desktop surfaces: full-screen rules, taskbar placement, show/hide state, watchdog, probe back-off |
| platform | `windows/platform/*` | WinRT/Win32 | RFCOMM/TCP transports, DNS-SD, GSMTC, SMTC, Core Audio, A2DP sink, loopback, lyrics providers, overlay |
| app | `windows/app` | everything | tray, IPC server, orchestration |
| UI | `windows/ui` | IPC only | thin WinUI 3 client |

Tests (`windows/tests`) link only `md_core` and use `FakeServer`/`FakeTransport`
(a spec-conformant fake Android server) plus `ManualScheduler`; mocks are never
compiled into the product.

## Threading

* Windows host: everything runs on the Win32 message-loop thread
  (`Win32Scheduler`). WinRT/COM callbacks and socket coroutines only marshal
  results back with `post()`. No locks in the core logic.
* The host thread never makes blocking calls into Explorer: the taskbar
  widget's UI Automation queries (and `SHAppBarMessage`) run on a dedicated MTA
  worker thread that owns no windows, with UIA connection/transaction timeouts
  (1 s / 1.5 s); results are posted back, the last good geometry is kept on
  failure. No host window is owned by an Explorer window (that would attach the
  input queues of both threads).
* Android: an `Engine` coroutine scope with a single-threaded dispatcher for
  protocol state; blocking socket I/O on IO dispatchers.

## Link policy (ConnectionManager)

* Windows is always the client (no inbound firewall rules); Android is the server.
* Auto mode keeps **LAN as primary** and Bluetooth as a hot-standby **secondary**
  when both are up (Bluetooth RFCOMM measured noticeably slower on real phones).
  Bulk artwork prefers the LAN connection.
* On primary loss the secondary is promoted immediately; un-answered commands
  younger than 5 s are re-sent with the same id (the peer's de-dup window ensures
  at-most-once execution).
* Switch-back to LAN is immediate if LAN never dropped recently; after drops it
  requires `switchBackDelay × 2^(drops−1)` (≤ 5 min) of stability.
* Reconnect back-off: BT 2 s → 60 s, LAN 1 s → 30 s, ±20 % jitter, reset by real
  events (adapter on, phone ACL connected, DNS-SD found or re-announced, network
  changed, user). A link that lived ≥ 30 s reconnects after ~1 s.
* LAN candidates are ranked: the endpoint that last reached Up, DNS-SD results,
  manual endpoints, then addresses the phone reported in `transport.info`
  (dropped after 10 min unless DNS-SD or a successful link confirms them). Each
  retry cycle starts from the best one. An endpoint whose handshake fails
  (accepts TCP but never authenticates: VPN / cellular / virtual interfaces of
  the phone) is demoted for 5 min. Handshakes race ("happy eyeballs": the next
  candidate starts 300 ms later, the first authenticated link wins) and a hanging
  handshake never blocks the next candidate or cycle.
* Heartbeats: 15 s while media plays, 60 s idle, 120 s on the secondary.

## Desktop lyrics and taskbar widget (host)

Both are per-pixel-alpha layered windows rendered with Direct2D/DirectWrite into
a memory DIB (`platform/overlay/OverlayCommon`); rendering is separate from
presentation (`UpdateLayeredWindow`), so frames can be rendered offscreen
without any window (`tools/overlay_demo`). They render only while something
changes (~60 fps only while a karaoke fill moves or a row cross-fades) and share
one smoothed playback clock (`LyricsClock`) and one lyrics delay per session
(output latency + per-app offset, docs/ipc.md "Lyrics timing").

* Desktop lyrics: glyph-outline text (soft shadow, outline, fill) with a
  feathered karaoke edge; `alternate` / `center` layouts; long lines shrink then
  scroll; always on top (re-asserted on foreground changes and every 2 s, never
  activates); hidden only for exclusive full screen / presentation mode unless
  `hideInFullscreen`; hover toolbar when unlocked; when locked (optionally
  click-through) a small unlock button appears after the cursor rests over the
  lyrics for 600 ms (200 ms cursor poll, only while shown).
* Taskbar widget: an unowned topmost tool window kept above the taskbar by
  re-asserting `HWND_TOPMOST` when the taskbar comes to the front. Geometry from
  the UIA worker (one cached `FindAllBuildCache` per query), re-queried only when
  the taskbar rect, DPI, alignment or the set of task windows changes (or every
  15 s while shown), with back-off after failures. It recovers by itself: on
  `TaskbarCreated` (Explorer restart), when its window was destroyed
  (`WM_NCDESTROY`/`IsWindow` checks), when the XAML taskbar is not ready yet at
  sign-in (retry, never the Windows 10 fallback), and through a watchdog that
  rebuilds window + surface if it should be visible but has not been for 5 s.

## Arbitration (PlaybackArbiter)

Runs only on Windows (single authority, no dueling engines). Inputs: debounced
Core Audio activity (any process, including games and calls), GSMTC sessions,
the phone's snapshots, and user commands. Outputs: pause/resume actions.

States per session: `PLAYING`, `USER_PAUSED`, `POLICY_PAUSED`, `REMOTE_PAUSED`,
`STOPPED`, `UNKNOWN`.

Loop and conflict prevention:

1. Every policy command registers an **expectation**; the matching snapshot
   transition is attributed to policy, not to the user.
2. Pauses without an expectation are `USER_PAUSED` (or `REMOTE_PAUSED` when the
   command came from the other device) and are never auto-resumed.
3. Only transitions trigger decisions; the first snapshot after connecting is a
   baseline.
4. A Windows audio onset within 3 s of our own Windows resume is ignored.
5. A user resuming the phone during a Windows audio episode is respected until
   that episode ends (no fighting).
6. Changing track while policy-paused counts as a user decision.
7. Calls (known VoIP apps, or render + capture by the same process) pause the
   phone in every mode except *off*; the PC is never paused during a call.
8. While the phone's audio is routed through the PC (A2DP sink) arbitration is
   suspended.
9. A policy action that does not take effect within 4 s is logged as failed and
   **not retried**.

Modes: off, Windows first (default), Android first, latest wins, custom
priorities (higher wins, ties → latest).

## Security summary

* Pairing: ECDH P-256 + 6-digit SAS confirmed on both screens; Android accepts
  pairing only in a 5-minute user-opened window.
* Sessions: fresh ECDHE per connection, mutual HMAC authentication with the
  pairing key, HKDF-derived AES-256-GCM per direction, implicit sequence numbers
  (replay/reorder ⇒ connection closed).
* Keys at rest: Windows DPAPI (per user); Android Keystore-wrapped.
* Strict input validation (sizes, string lengths, types), artwork ≤ 1 MiB with
  SHA-256 verification, lyrics ≤ 256 KiB.
* No cloud services. Optional LRCLIB lookups send only title/artist/album/duration.
* Logs never contain keys, MACs or nonces.
