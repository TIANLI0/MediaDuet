# Media sessions

## Windows: GSMTC (reading and controlling local players)

`GlobalSystemMediaTransportControlsSessionManager` gives every player that
publishes System Media Transport Controls: browsers (Edge/Chrome/Firefox),
Spotify, NetEase Cloud Music (`cloudmusic.exe`), QQ Music (`QQMusic.exe`),
foobar2000 (with the SMTC component), VLC 4, Groove/Media Player, …

* Fully event driven: `SessionsChanged`, `CurrentSessionChanged`, and per session
  `MediaPropertiesChanged`, `PlaybackInfoChanged`, `TimelinePropertiesChanged`.
  No polling. Snapshot emission is coalesced (60 ms).
* Position: `Position + (now − LastUpdatedTime) × rate` while playing; clamped.
  Many apps update the timeline only on state changes; receivers extrapolate.
* Thumbnails are read once per change, normalised to ≤ 512 px JPEG and
  content-addressed by SHA-256, so the same cover is never re-encoded or re-sent.
* Commands use `TryPlayAsync/TryPauseAsync/…/TryChangePlaybackPositionAsync`
  after checking `PlaybackInfo.Controls`.
* MediaDuet's own SMTC bridge session (`MediaDuet.Host` / `MediaDuet.exe`) is
  filtered out → never mirrored back to the phone (no echo loop).

Apps that do **not** publish SMTC (most games, some players) are invisible to
GSMTC; the Core Audio monitor still sees their audio for arbitration, but they
cannot be paused remotely.

## Windows: SMTC bridge (phone media in the Windows flyout)

Phase 0 showed that a desktop process can register an SMTC session for a hidden
window via `ISystemMediaTransportControlsInterop::GetForWindow` **without
playing audio**, and that it appears in GSMTC with title/artist/timeline.

The host mirrors the phone's active session there (title, artist, album, cover,
status, timeline, enabled buttons). Button presses and seek requests become
`media.command` messages with `origin:"user"`.

Caveats (documented, verified behaviour on Windows 11 build 26340):

* The bridged session can become Windows' *current* session, so keyboard media
  keys control the phone while it plays. It is removed (`Closed` + disabled) when
  the phone stops, disconnects, or the option is turned off (Settings → Windows
  media integration).
* Windows 10 shows the same flyout but the lock-screen/flyout styling differs;
  not verified on Windows 10 hardware in this environment.
* No fake audio stream is created.

## Android: MediaSessionManager via NotificationListenerService

`MediaSessionManager.getActiveSessions(listenerComponent)` requires the user to
grant notification access. MediaDuet uses the listener **only** as the
permission anchor and a system-bound process; it never reads, stores or sends
notification contents.

Players are not assumed to be equal: metadata keys are read defensively
(`TITLE`/`DISPLAY_TITLE`, `ARTIST`/`ALBUM_ARTIST`/`DISPLAY_SUBTITLE`, album art
bitmap or URI), actions come from `PlaybackState.actions`, and position is
extrapolated from `lastPositionUpdateTime`. NetEase Cloud Music
(`com.netease.cloudmusic`), QQ Music (`com.tencent.qqmusic`), Spotify and YouTube
Music all publish standard MediaSessions; real-device verification status is in
[compatibility.md](compatibility.md).

## Android: Windows media on the phone

Shown in the app's Now Playing screen. An optional (default **off**) mirror
creates a MediaSession + MediaStyle notification so Windows media appears in the
system media controls; because a playing session can take media-button routing
from local players, it stays opt-in and is never mirrored back.

## Lyrics

See `windows/platform/lyrics/LyricsService.cpp`. Provider chain: cache → lyrics
pushed by the peer → local `.lrc` folder (UTF-8 or ANSI/GBK) → LRCLIB
(`/api/get`, then `/api/search` with title/artist/duration matching; ±5 s
duration hard limit). Negative results are cached for 72 h. Without lyrics only
the media card is shown. No private NetEase/QQ APIs, scraping or UI automation.
Lyric progress is interpolated locally from the last snapshot (no per-second
cross-device polling).
