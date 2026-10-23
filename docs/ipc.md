# Windows host ↔ UI IPC

MediaDuet on Windows runs as two processes:

| Process | Tech | Lifetime |
|---------|------|----------|
| `MediaDuet.exe` (host) | C++20, Win32, C++/WinRT, Direct2D | Starts at login (optional), owns the tray icon, all links, media sessions, arbitration, SMTC bridge, A2DP sink and the desktop-lyrics overlay. |
| `MediaDuetUI.exe` (UI) | WinUI 3 (Windows App SDK, C++/WinRT) | Started on demand by the host (tray click, notification, pairing). Exits when its last window closes. Never required for background sync. |

## Transport

* Named pipe: `\\.\pipe\MediaDuet.UI.<user SID string>` (e.g. `S-1-5-21-…`).
  The host creates it with a DACL granting access only to the current user SID
  and `PIPE_REJECT_REMOTE_CLIENTS`.
* Duplex, byte mode. Messages are UTF-8 JSON objects, **one per line** (`\n`
  terminated, no embedded raw newlines). Max 4 MiB per line.
* One UI client at a time. A new client replaces the old one.
* The host sends `hello` then a full `state` immediately after a client connects.
* The UI is started as `MediaDuetUI.exe [--show main|compact|settings|pair] [--pipe <name>]`.
  If the pipe is not available the UI shows a "host not running" screen and
  offers to launch `MediaDuet.exe` from its own directory.

Every message has a `"type"` field. Messages from the UI to the host may carry an
`"rid"` (request id, integer). The host answers such messages with
`{"type":"result","rid":…,"ok":bool,"error":"…"}`.

## Host → UI

### `hello`
```json
{"type":"hello","version":"1.0.0","pid":1234,"language":"zh-CN"}
```

### `state` (full snapshot; sent on connect and on every change, coalesced to ≤ 10/s)
```json
{
  "type": "state",
  "tick": 123456789,
  "self": {"id":"…","name":"DESKTOP-1","app":"1.2.1"},
  "standalone": false,
  "connection": {
    "status": "connected",
    "transport": "bt",
    "secondary": "lan",
    "peer": {"id":"…","name":"OPPO Find X8 Ultra","platform":"android","app":"1.2.1","versionMatch":"equal"},
    "sinceTick": 123400000,
    "rttMs": 41,
    "mode": "auto",
    "lastError": null,
    "bytesIn": 12345, "bytesOut": 6789
  },
  "devices": [
    {"id":"…","name":"OPPO Find X8 Ultra","platform":"android",
     "btAddress":"AA:BB:CC:DD:EE:FF","lanHost":"192.168.1.20:47321",
     "connected":true,"transport":"bt","lastSeen":1760000000000,
     "app":"1.2.1","versionMatch":"equal"}
  ],
  "local":  { "gen": 3, "active": "Spotify.exe", "sessions": [ SESSION… ] },
  "remote": { "gen": 9, "active": "com.netease.cloudmusic#1", "sessions": [ SESSION… ] },
  "focus":  {"side":"remote","key":"com.netease.cloudmusic#1","auto":true},
  "policy": {"mode":"windows_first","enabled":true,"remote":"policy_paused","local":"playing",
             "windowsAudioActive":true,"activeApps":["chrome.exe"],
             "lastAction":{"what":"pause_remote","reason":"windows_audio_active","app":"chrome.exe","atTick":123}},
  "audio": {
    "a2dpSink": {"available":true,"state":"off","device":"OPPO Find X8 Ultra","error":null,"errorCode":null},
    "lanSend":  {"available":true,"state":"off","error":null}
  },
  "pairing": {"state":"idle","sas":null,"peerName":null,"error":null},
  "discovered": [
    {"kind":"bt","id":"Bluetooth#Bluetooth…","name":"OPPO Find X8 Ultra","detail":"AA:BB:…","paired":false},
    {"kind":"lan","id":"192.168.1.20:47321","name":"Pixel 9","detail":"192.168.1.20:47321","paired":true}
  ],
  "lyricsOverlay": {"visible": false, "shown": false},
  "volume": {
    "local":  {"available": true, "level": 0.42, "muted": false},
    "remote": {"available": true, "level": 0.60, "muted": false, "steps": 15, "error": null}
  },
  "audioOutputLatency": {"bluetooth": true, "ms": 180},
  "taskbar": {"supported": true, "shown": true, "reason": null},
  "audioOutputs": [
    {"id": "{0.0.0.00000000}.{guid}", "name": "Headphones (OPPO Enco X4)", "default": true}
  ],
  "settings": { SETTINGS… },
  "handoff": {"apps": {"local": ["netease"], "remote": ["qqmusic", "netease"]},
              "active": {"to": "phone", "app": "qqmusic", "title": "…", "waitingForTap": false}},
  "update": {"phase": "available", "current": "1.5.0", "latest": "1.5.1", "page": "https://github.com/…/releases/tag/v1.5.1",
             "notes": "…", "progress": 0.0, "error": null, "auto": true, "portable": false, "checkedAt": 1760000000000},
  "diagnostics": {"enabled":false}
}
```

* `update` (Settings › About): `phase` ∈ `idle`, `checking`, `current` (up to
  date), `available`, `downloading` (`progress` 0..1), `installing` (the host is
  about to exit), `error` (`error` ∈ `network`, `no_release`, `no_asset`,
  `no_checksum`, `checksum`, `write`, `not_writable`, `launch`). `portable`: this
  copy was not installed by setup.exe, so an update replaces the files of its
  folder (a hidden PowerShell script after the host exits) instead of running the
  installer (`setup.exe /S /UPDATE`, which restarts MediaDuet). Automatic checks
  (60 s after start, then daily) follow `settings.update.auto`; a failed automatic
  check stays `idle`. Downloads are verified against the release's
  `SHA256SUMS.txt`.
* `handoff` (continue a song on the other device, protocol §8.7): `apps.local`
  are this PC's music apps that can take a song (`netease`, `qqmusic`,
  `spotify`: installed and supported), `apps.remote` the phone's (empty while no
  phone with `handoff` is connected). `active` is the running handoff or `null`;
  `waitingForTap`: the phone asks its user to tap a notification. Sessions may
  carry `ids` (protocol §8.3).

`SESSION`:
```json
{
  "key":"com.netease.cloudmusic#1",
  "app":{"id":"com.netease.cloudmusic","name":"网易云音乐"},
  "title":"…","artist":"…","album":"…",
  "trackId":"…",
  "durationMs":245000,
  "positionMs":12345,
  "atTick":123456700,
  "rate":1.0,
  "status":"playing",
  "actions":["play","pause","next","previous","seek"],
  "artworkPath":"C:\\Users\\…\\AppData\\Local\\MediaDuet\\cache\\art\\<sha256>.jpg",
  "kind":"music",
  "policyState":"playing",
  "appVolume":{"available":true,"level":0.8,"muted":false},
  "outputLatencyMs":180,
  "lyricsDelayMs":480
}
```

* Position extrapolation: `atTick` is the host's monotonic clock when
  `positionMs` was valid: interrupt time in ms (`QueryInterruptTimePrecise` / 10 000,
  the time base of `GetTickCount64` at sub-millisecond precision; `GetTickCount64`
  on systems without it). It is system-wide, so the UI computes
  `positionMs + (now - atTick) * rate` with the same clock while `status == "playing"`
  and clamps to `[0, durationMs]`. The UI must not poll the host for progress.
* Sessions may also carry `source` (`smtc` | `compat` | `remote`) and
  `positionEstimated` (bool).
* `artworkPath` points into the host's content-addressed cache and may be `null`.
* `outputLatencyMs`: estimated output latency of the device that plays the
  session (local sessions: this PC's default output, see `audioOutputLatency`;
  remote sessions: the phone's `outputLatencyMs`, protocol §8.3, 0 if absent).
* `lyricsDelayMs`: total delay the UI must subtract from the (smoothed)
  position before looking up lyrics = `outputLatencyMs` (only when
  `lyrics.latency.auto`) + the per-app offset `lyrics.offsets[app.id]`. May be
  negative. The host is the single source of truth; see "Lyrics timing".
  Its presence also tells the UI that the host understands `lyrics.offset`
  (older hosts omit it; the UI then hides the offset control and uses 0).
* `audioOutputLatency` (top level): the current default output of this PC —
  `bluetooth` (Bluetooth A2DP/HFP/LE endpoint) and `ms`, the latency used for
  local sessions (`lyrics.latency.bluetoothMs` for Bluetooth, else 0), so the UI
  can explain the automatic value.
* `volume.remote.error`: the phone's last refusal of a `volume.set`, passed
  through unchanged (e.g. `background_restricted`: Android 17+ does not let a
  background app change the volume), `null` again after a set succeeds. The
  host never retries a refused set.
* `taskbar.reason` (when `shown` is false): `disabled`, `nothing_playing`,
  `fullscreen`, `autohide`, `vertical_taskbar`, `no_space` (expected states) or
  `taskbar_not_ready` / `no_taskbar` / `unavailable` (the host is still measuring
  or recovering, e.g. right after sign-in or an Explorer restart; it retries by
  itself).
* `standalone`: the work mode is "This PC only" (`settings.mode`): no transports,
  discovery, pairing, arbitration or audio sending; commands that need the phone
  answer `result` with `error: "standalone"`. Local sessions, in-app lyrics, the
  desktop overlay and taskbar lyrics keep working.
* `self.app` is this PC's MediaDuet version; `connection.peer.app` /
  `devices[].app` the phone's (`DeviceInfo.app`, protocol §6.2; the last
  version seen is kept with the paired device, `null` if never seen).
  `versionMatch` ∈ `equal`, `patch` (only the patch number differs: show quietly),
  `mismatch` (different major or minor: warn, both apps should be updated),
  `unknown` (no version: no warning).
* `policyState` ∈ `playing`, `user_paused`, `policy_paused`, `remote_paused`,
  `stopped`, `unknown`.
* `connection.status` ∈ `no_peer`, `disconnected`, `connecting`, `connected`.
* `pairing.state` ∈ `idle`, `connecting`, `awaiting_user` (SAS shown, waiting
  for this user), `awaiting_peer` (this user accepted, waiting for the phone),
  `succeeded`, `failed`.
* `pairing.error` is a stable code for the UI to localise: `connect_failed`,
  `unknown_device`, `invalid_address`, `connection_closed`, `timeout`,
  `rejected`, `pairing_disabled`, `mac_mismatch`, `busy`, `protocol`,
  `bad_message`. `connection.lastError` is free-form diagnostic text (English)
  and should be shown only as secondary detail.

### `lyrics` (sent when the lyrics for the focused track change)
```json
{
  "type":"lyrics",
  "trackId":"…","side":"remote","key":"…",
  "title":"…","artist":"…","durationMs":269000,
  "status":"found",
  "source":"lrclib",
  "synced":true,
  "hasTranslation":true,
  "lines":[{"t":12000,"d":3500,"text":"…","tr":"(translated line, optional)","words":[{"t":12000,"d":400,"text":"…"}]}]
}
```
`status` ∈ `loading`, `found`, `notfound`, `disabled`. `words` is present only for
enhanced (word-timed) LRC. Without lyrics the UI shows only the media card — no
placeholder lyrics.

`title` / `artist` / `durationMs` describe the song the lyrics belong to. Players
re-report the same song with a slightly different duration or album (QQ Music
does this often), which changes the `trackId` (protocol §8.3). The UI therefore
matches lyrics to a session by `trackId` **or** by the same song (same normalised
title and artist, durations within 3 s) and must not clear, fade or rebuild the
lyrics view for such a change; the host re-keys the current lyrics to the new
`trackId` without a `loading` phase.

### `diagnostics` (only while enabled, every 2 s)
```json
{"type":"diagnostics","cpuPercent":0.12,"workingSetMB":21.4,"privateMB":9.8,
 "transport":"bt","connectedForSec":350,"bytesIn":1,"bytesOut":2,
 "msgsIn":3,"msgsOut":4,"rttMs":41,"clockErrorMs":3,"lastCommandLatencyMs":120,
 "localSessions":2,"remoteSessions":1,"audioSessionsActive":1,"threads":9}
```

### `handoff` (a handoff ended)
```json
{"type":"handoff","outcome":{"ok":true,"state":"playing","to":"pc","app":"netease","title":"…"}}
```
`state` ∈ `playing` (the song plays on the other device, the source paused),
`opened` (shown there, playback not confirmed; the source keeps playing). On
failure `"ok":false,"error":{"code":…}` with the codes of protocol §8.7. Sent for
handoffs started from the UI and from the phone.

### `show`
```json
{"type":"show","window":"main"}
```
`window` ∈ `main`, `compact`, `settings`, `pair`.

## UI → Host

| type | fields | effect |
|------|--------|--------|
| `media.command` | `side` (`local`/`remote`), `key` (nullable), `action`, `positionMs?` | Executes on Windows (local) or sends `media.command` to the phone (origin `user`). |
| `focus.set` | `side`, `key` (null + `auto:true` = automatic) | Choose which session the cards show. |
| `settings.set` | `patch` (deep-merged object) | Persists and applies settings. |
| `pair.start` | `kind` (`bt`/`lan`), `target` (BT device id or `host:port`) | Starts pairing. |
| `pair.respond` | `accept` (bool) | User accepted/rejected the SAS. |
| `pair.cancel` | — | Abort pairing. |
| `device.forget` | `id` | Revoke trust and delete the pairing key. |
| `device.connect` | `id` | Connect now (resets back-off). |
| `device.disconnect` | `id` | Disconnect and pause auto-reconnect until `device.connect`. |
| `connect.manual` | `host`, `port` | Remember a manual LAN endpoint for the paired phone and connect. |
| `discover` | `enable` (bool) | Start/stop discovery (BT paired phones with the MediaDuet SDP record + DNS-SD). |
| `media.command` extras | `action:"custom"` + `customAction` (id), `action:"playQueueItem"` + `queueId` | The phone app's own buttons (`customActions` in a session: `{id, title, iconPath}`, the icon a cached PNG) and its play queue. Sessions with a queue carry `queue: {count, rev, activeId}`; the host fetches the list from the phone and pushes `{"type":"queue","side":"remote","key":…,"queue":{key, rev, title, activeId, total, offset, items:[{id,title,subtitle}]}}` (or `"queue": null` when it is gone), also right after `hello`. |
| `update.check` | — | Asks GitHub for the latest release now (`state.update`). |
| `update.install` | — | Downloads the available version (installer, or the zip for a portable copy), verifies it and installs it; the host exits and is started again by the installer / script. |
| `handoff.start` | `side` (`local`/`remote`), `key`, `app?` (`netease`/`qqmusic`/`spotify`) | Continues session `key` of `side` on the other device in `app` (default: the source's own app if the other device has it, else its first one), from the current position; the source pauses (protocol §8.7). The `result` arrives when it is done (up to 120 s) with the outcome's error code; the outcome itself also comes as a `handoff` message. |
| `audio.a2dpSink` | `enable` | Open/close Bluetooth audio receive (AudioPlaybackConnection). `state.audio.a2dpSink.errorCode` ∈ `no_device`, `ambiguous`, `timeout`, `denied`, `failed` (same codes the phone gets in `audio.sink.state`). The phone can switch it too (`audio.sink.set`), and the tray menu has a check item while a phone offers Bluetooth audio. |
| `audio.lanSend` | `enable` | Start/stop experimental Windows → Android PCM streaming. |
| `lyrics.overlay` | `visible` | Show/hide the desktop lyrics overlay (rendered by the host). |
| `lyrics.offset` | `appId` (session `app.id`; default: the focused session's), `deltaMs` (int, + = lyrics later) **or** `reset: true` | Adjusts the per-app lyrics offset `lyrics.offsets[appId]` (clamped to ±5000 ms; 0 removes the entry), persists it and updates every session's `lyricsDelayMs`. Answers with `result`. |
| `volume.set` | `side` (`local`/`remote`), `level?` (0..1), `muted?`, `delta?` (steps), `key?` | Set this PC's output volume or the phone's media volume; with `key` (a local session) set only that app's volume (per-app mixer). For `remote`, the `result` is sent when the phone confirms (`ok:true`) or refuses — `error` is then the phone's code unchanged, e.g. `"background_restricted"` (also kept in `state.volume.remote.error`); no answer within 3 s counts as ok. |
| `diagnostics` | `enable` | Toggle diagnostics messages. |
| `host.exit` | — | Quit the host (and the UI). |

## Lyrics timing

Shared by the host surfaces (desktop lyrics, taskbar widget), the WinUI app and
the Android app so that all of them highlight the same character at the same
moment. Reference implementation: `windows/core/{include/md,src}/lyrics/`
(`LyricsClock`, `Karaoke`) with unit tests.

**Clock.** Extrapolate the session position locally with a high-resolution
monotonic clock (`positionMs + (now − atTick) × rate` while playing). When a
new report arrives (new `state`, phone snapshot), compute
`error = reported − predicted`:

* snap (re-anchor) when |error| > 300 ms, on play/pause, rate or track changes
  and after a seek;
* otherwise slew: run the local clock at `rate × (1 + c)` with
  `c = clamp(error / 1000 ms, −5 %, +5 %)` until the error is absorbed (≈ 1 s for
  errors ≤ 50 ms, proportionally longer for larger ones). The shown position
  therefore never moves backwards while playing.

**Delay.** `display = smoothedPosition − session.lyricsDelayMs`; look up lines
and words with `display`.

**Karaoke fill** (per line):

* Word-timed lines (`words` present) are displayed as the concatenation of
  their words; leading/trailing whitespace of the whole line is trimmed (word
  spans shift / clamp accordingly, a whitespace-only word becomes an empty
  span), inner whitespace (also runs of spaces) belongs to the word that carries
  it, and positions are UTF-16 offsets on code-point boundaries (surrogate pairs
  are never split). Inside a word the fill edge moves linearly from the caret
  before it to the caret after it over `[t, t + d)`; between words it holds at
  the next word's start; after the last word the line is complete.
* Line-timed lines sweep evenly from the first to the last caret over
  `min(0.9 × d, max(1200 ms, codePoints × 450 ms))` (`d` = 4000 ms if unknown).
* The edge is continuous with a feathered gradient (~0.5 × font size), sung
  part in the active colour, the rest in the inactive colour. Lines wider than
  the available width may shrink (≤ 15 %) and then scroll horizontally: start
  aligned until the edge passes 60 % of the width, then the edge stays there,
  finally end aligned.

## Lyrics rows

Every surface (app lyrics view, compact window, desktop lyrics, taskbar; Android
lyrics mode, mini player and lyrics notification) shows the same rows
(`md::lyrics::buildRows`, Android `LyricRows`):

* Rows are the sung lines plus instrumental gaps. Blank lines are never shown.
  Leading blank lines are skipped. A blank line becomes a gap only when the
  next line is ≥ 4 s away; a shorter one is dropped, so the previous line
  stays current through it.
* Intro: a gap from 0 when the first sung line starts at ≥ 4 s.
* Silence after a line, before a non-blank next line, is a gap when it lasts
  ≥ 4 s. When the singing ends:
  * word-timed lines: the last word's start + min(its length, 1.5 s). A word
    lasts until the next word; the last word lasts until the next line, or 1 s
    for the last line of the lyrics.
  * line-timed lines: min(next line, max(sweep end, t + codePoints × 450 ms + 2 s)).
* A line row starts at min(line t, first word t), kept ascending.
* End: 3 s after the last line is sung (max(start + min(d, 5 s), singing end)
  + 3 s). After that no row is current. A gap that ends the rows ends the
  lyrics at its end.
* Where only one line fits, nothing is shown before the lyrics, in a gap, or
  after the end, and the title / source takes its place (compact window,
  taskbar, Android mini player and notification). The desktop lyrics instead
  preview the next sung line (not highlighted), or keep the last line (not
  highlighted) after the end. A gap's breathing dots are visible only while
  that gap is current.

## Settings object

Persisted by the host at `%LOCALAPPDATA%\MediaDuet\settings.json`.
Unknown keys are preserved. Defaults:

```json
{
  "language": "auto",
  "theme": "system",
  "autostart": true,
  "mode": "linked",
  "modeChosen": false,
  "artwork": {"online": true},
  "connection": {"mode": "auto", "switchBackDelaySec": 10},
  "policy": {
    "mode": "windows_first",
    "autoResume": true,
    "resumeDelaySec": 5,
    "activationMs": 1500,
    "sensitivity": 0.02,
    "pauseForCalls": true,
    "headsetAssist": true,
    "onlyWithOutputs": [],
    "blacklist": [],
    "whitelist": [],
    "priorities": {}
  },
  "smtcBridge": true,
  "compat": {"neteaseDesktop": true},
  "taskbar": {
    "enabled": true,
    "content": "auto",
    "position": "left",
    "align": "auto",
    "offset": 0,
    "width": 360,
    "fontSize": 13
  },
  "lyrics": {
    "online": true,
    "providers": ["netease", "qqmusic", "lrclib"],
    "translation": true,
    "localFolder": "",
    "latency": {"auto": true, "bluetoothMs": 180},
    "offsets": {},
    "overlay": {
      "visible": false, "locked": false, "clickThrough": false, "autoHide": true,
      "topmost": true, "hideInFullscreen": false, "layout": "alternate",
      "fontFamily": "Microsoft YaHei UI", "fontSize": 32, "opacity": 0.92,
      "karaoke": true, "twoLines": true,
      "colorActive": "#FFFFFFFF", "colorInactive": "#B3FFFFFF",
      "x": null, "y": null, "width": 900
    }
  },
  "ui": {
    "alwaysOnTop": false,
    "animations": true,
    "lowPower": false,
    "main": {"x": null, "y": null, "width": null, "height": null, "maximized": false},
    "compact": {"x": null, "y": null, "alwaysOnTop": true, "snap": true}
  },
  "update": {"auto": true},
  "diagnostics": false
}
```

* `update.auto` (default `true`): check for a new version once a day.
  `ui.updateDismissed`: the version whose "update available" bar the user closed.
* `language` ∈ `auto`, `zh-CN`, `en-US`; `theme` ∈ `system`, `light`, `dark`.
* `ui.main.width` / `ui.main.height` (physical pixels, `null` = default size) and
  `ui.main.maximized` are written by the UI to restore the main window.
* `mode` ∈ `linked` (with a paired phone) or `standalone` (this PC only, see
  `state.standalone`). `modeChosen` is set once the user chose (the UI shows a
  first-run choice while it is `false` and no phone is paired; installs that
  already have a paired phone are set to `linked` silently).
* `artwork.online` (default `true`): when a player's own cover is missing or
  small (< 500 px), look up the song's cover on NetEase Cloud Music, QQ Music and
  iTunes (same matching rules as lyrics; title/artist/album/duration only),
  cached per track; the session's `artwork` then points to the better image.
* `ui.versionNoticeDismissed`: "local|phone" version pair whose mismatch notice
  the user dismissed on the player page.
* `policy.mode` ∈ `off`, `windows_first`, `android_first`, `latest`, `custom`.
* `policy.headsetAssist`: multipoint-headset helper. When a player is paused by the
  headset within ~2.5 s of a user starting it (because the other device still
  holds the headset), the host pauses the other device and resumes the one the
  user just started.
* `policy.onlyWithOutputs`: list of `{id, name}` audio outputs (from
  `state.audioOutputs`). When non-empty, arbitration (auto pause/resume and
  headset assist) is active only while Windows' default output is one of them,
  e.g. your multipoint earbuds; `state.policy.gated` is then `true` otherwise.
* `lyrics.latency.auto` (default `true`): delay lyrics by the output latency of
  the device that plays the session. `lyrics.latency.bluetoothMs` (int 0–1000,
  default 180): the latency assumed when this PC's default output is a
  Bluetooth device (wired / USB / speakers: 0). It is also sent to the phone as
  `outputLatencyMs` of local sessions.
* `lyrics.offsets`: `{appId: ms}` per-app manual offset (int −5000…5000,
  + = lyrics later), keyed by the session's `app.id` (e.g. `cloudmusic.exe`,
  `com.netease.cloudmusic`). Change it with `lyrics.offset` (or a
  `settings.set` patch; `null`/0 removes an entry).
* `lyrics.overlay.topmost` (default `true`): keep the desktop lyrics above all
  other windows (re-asserted when the foreground window changes and every 2 s,
  without taking focus). `lyrics.overlay.hideInFullscreen` (default `false`):
  also hide over borderless full-screen windows; exclusive full screen and
  presentation mode always hide it. `lyrics.overlay.layout`: `alternate`
  (classic two-line KTV: rows alternate left / right, the next line is shown in
  the other row) or `center` (current line centred, next line below). With
  translations shown and available, the overlay always shows the current line
  centred with its translation below.
* `lyrics.providers`: ordered online sources (`netease`, `qqmusic`, `lrclib`);
  remove an id to disable it. `lyrics.online=false` disables all of them.
* `taskbar`: Windows 11/10 taskbar widget drawn by the host inside the taskbar.
  `content` ∈ `auto` (lyrics when available, otherwise title/artist), `lyrics`,
  `player` (title + controls); `position` ∈ `left`, `right` (next to the tray);
  `align` ∈ `auto` (hug the task icons), `left`, `right`: the edge of the free
  area the widget sticks to, and the side its text is anchored to — `left` /
  `right` stay at the steady end of the area (screen edge, Widgets, tray) while
  the task icons change; `offset` shifts it away from that edge in physical
  pixels; `width`/`fontSize` in DIPs.
* `compat.neteaseDesktop`: NetEase Cloud Music for Windows support. Without a
  media session of its own: title from its window, control via its window
  (`"source":"compat"`). With its own media session (its "Windows media
  controls" option, no timeline) the host fills in what is missing. In both
  cases state, position, duration and song id are read from NetEase's audio
  player in `cloudmusic.dll` (located by code signature, `PROCESS_VM_READ`
  only, polled every 0.5 s while playing), and seeking uses NetEase's
  `orpheus://` web command. NetEase's play queue (its local `playingList`
  file, re-read when it changes) is exposed as the session's `queue`: the
  phone fetches it with `media.queue.get` and the UI gets it as a `queue`
  message with `"side":"local"`; `playQueueItem` (`queueId` = song id) plays
  the entry with the same web command. Builds where the player is not found fall back to
  the duration from NetEase's local play queue and a position estimated from
  the time playing since the track changed; those sessions are flagged
  `"positionEstimated":true` and cannot seek.
* `handoff.taskbarResume` (default `true`): while the phone's active session
  plays in NetEase Cloud Music, QQ Music or Spotify (or paused it less than
  10 minutes ago) and this PC has the same app, the host shows a taskbar
  button of its own (the app's icon with a phone badge, the cover as its
  thumbnail). Clicking it runs `handoff.start` for that session into the same
  app (the button shows progress, red on failure). Its thumbnail has previous /
  play-pause / next for the phone's player. "Close window" hides it until
  the track changes or the phone plays it again after a pause. The UI's
  handoff button is likewise only offered for sessions of these apps.
* `policy.priorities` maps an app id (Windows exe name or AUMID, or Android
  package) to an integer priority (higher wins) for `custom` mode.
* `policy.blacklist` / `whitelist`: Windows process names (e.g. `chrome.exe`).
  If `whitelist` is non-empty, only those processes count as Windows audio.
* Window coordinates are physical pixels of the virtual screen; `null` means
  "default placement".
