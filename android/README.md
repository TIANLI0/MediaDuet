# MediaDuet for Android

The Android half of MediaDuet: two-way media control between a Windows PC and an Android phone.
Android is the protocol **server** (see [`../protocol/specification.md`](../protocol/specification.md)).
It listens on Bluetooth RFCOMM and TCP, and exposes the phone's media sessions to the PC. It also
shows and controls what the PC plays, with synced lyrics, arbitration status and optional PC→phone
audio.

## Toolchain

| Component | Version |
|-----------|---------|
| Android Gradle Plugin | 9.4.1 (built-in Kotlin) |
| Kotlin / Compose compiler plugin | 2.4.21 |
| Gradle (wrapper) | 9.8.1 |
| Compose BOM | 2026.09.00 (UI 1.12.1, Material3 1.4.0) |
| Liquid glass | `io.github.kyant0:backdrop:2.0.1` + `io.github.kyant0:shapes:1.2.1` (Apache-2.0) |
| kotlinx.coroutines / serialization-json | 1.11.0 / 1.11.0 |
| compileSdk = targetSdk | 37 (Android 17), minSdk 29 (Android 10) |
| JDK for Gradle | 21 (JDK 25 also works with Gradle 9.8; CI should use 21) |

## Project layout

```
android/
  app/                          app.mediaduet (the product)
    src/main/kotlin/app/mediaduet/
      protocol/   frames (incremental decoder, limits), envelope + strict validation, messages, trackId
      crypto/     P-256 ECDH, HKDF, HMAC, AES-256-GCM records, key schedule, Keystore-wrapped trust store
      link/       connection (CONTROL-before-BLOB send queue), server handshake (pairing + session),
                  pairing controller (5-min window, SAS), link sessions (resume, multi-transport,
                  primary, de-dup window 1024, cached command results), heartbeat
      transport/  TCP 47321 (ephemeral fallback), RFCOMM "MediaDuet", NSD _mediaduet._tcp, BT/network monitors
      media/      NotificationListenerService (access only), MediaSessionMonitor (event driven),
                  snapshot model + suppression, artwork processor (≤512 px JPEG q85, SHA-256, LRU)
      remote/     PC state (position interpolation, artwork reassembly, lyrics, policy), LRC parser,
                  optional mirror MediaSession
      audio/      experimental PCM receiver (TCP, AES-GCM, ~80 ms jitter buffer, AudioTrack)
      service/    Engine singleton, foreground service, boot receiver, companion presence service,
                  CDM helper, settings, per-app language, permission checks, notifications
      ui/         Compose + liquid glass (Root, player, lyrics, mini player, settings pages)
    src/test/     JVM unit tests (crypto vectors, framing, envelope, handshake, sessions, ...)
  testplayer/                   app.mediaduet.testplayer: TEST FIXTURE ONLY (not shipped)
```

## Build

```bash
cd android
./gradlew :app:assembleDebug :app:assembleRelease :app:testDebugUnitTest :app:lintDebug :testplayer:assembleDebug
```

* Unit tests read the normative vectors from `../protocol/test-vectors/crypto-v1.json`. The path is
  passed as the system property `mediaduet.vectors` by `app/build.gradle.kts`.
* Lint treats `MissingTranslation`, `HardcodedText`, `ExtraTranslation`, `MissingQuantity` and
  string-format checks as errors. Every user-visible string lives in `res/values` with a complete
  `res/values-zh-rCN` translation.
* **Release signing.** If `android/keystore.properties` exists, release builds are signed with it:

  ```properties
  storeFile=../my-release.jks
  storePassword=...
  keyAlias=mediaduet
  keyPassword=...
  ```

  Without it `assembleRelease` produces an **unsigned**
  `app/build/outputs/apk/release/app-release-unsigned.apk` (R8 minified and resource-shrunk). Sign it
  with `apksigner` before installing. `keystore.properties` and `*.jks` are git-ignored.

## Install & set up

```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
# Media session access (normally granted by the user in Settings):
adb shell cmd notification allow_listener app.mediaduet/app.mediaduet.media.MediaListenerService
```

In the app:

1. **Settings › Permissions & background.** Grant each item:
   * media session access (notification-listener access)
   * notifications
   * Nearby devices (Bluetooth)
   * Local network (Android 17+)

   Sideloaded builds on Android 13+ need *App info › ⋮ › Allow restricted settings* before
   notification access can be granted.
2. **Settings › Connection.** Turn on *Pairing mode* (5 minutes), start pairing on the PC, and
   compare the 6-digit code. Accept on both sides.
3. Optionally *Associate PC* (companion device). Android then starts MediaDuet when the PC comes into
   Bluetooth range and relaxes background limits.

### Permissions and what happens without them

| Permission | Used for | If denied |
|------------|----------|-----------|
| Notification-listener access | `MediaSessionManager.getActiveSessions` only. Notification contents are never read, stored or sent. | Phone media is not shared (empty snapshots); PC control still works |
| `BLUETOOTH_CONNECT` (31+) / `BLUETOOTH` (≤30) | RFCOMM server | LAN only |
| `ACCESS_LOCAL_NETWORK` (37+, runtime, *dangerous*) | LAN access + NSD advertisement | Bluetooth only. The TCP socket still listens (reachable via `adb forward`/USB) |
| `POST_NOTIFICATIONS` (33+) | Status, pairing prompt, mirror controls | Silent; FGS still runs |
| `FOREGROUND_SERVICE_CONNECTED_DEVICE` | Keep-alive service | Engine runs inside the listener process |
| `REQUEST_COMPANION_*` | Presence wake-ups, background FGS start, battery exemption | No presence wake-ups |

Every state is shown live on the Permissions page and the settings summaries. Revoking anything at
runtime degrades the related feature; nothing crashes.

## Background behaviour

An `Engine` singleton (owned by the `Application`) holds all state and runs while the process lives.
The process is kept alive by:

1. the **NotificationListenerService**, which the system binds while access is granted
   (`onListenerConnected` starts the engine);
2. the **`connectedDevice` foreground service** (`LinkService`), started from the activity, at
   `BOOT_COMPLETED`/`MY_PACKAGE_REPLACED`, from companion presence events, and attempted from the
   listener (a `ForegroundServiceStartNotAllowedException` is caught and logged; the engine keeps
   running unprivileged);
3. **CompanionDeviceManager presence**: `ObservingDevicePresenceRequest` on API 36+, address-based
   observation on API 31–35.

No WorkManager/AlarmManager keep-alive tricks, no AccessibilityService, no root.

| Android | Notes |
|---------|-------|
| 10 (29) | FGS type declared; no runtime BT permission; no presence observation (CDM association still works). No RenderEffect: glass falls back to opaque translucent surfaces. |
| 11 (30) | Same as 10; listener detail settings deep link available. |
| 12/12L (31/32) | `BLUETOOTH_CONNECT` runtime permission. Background FGS starts restricted (listener start may be refused, which is handled). Presence via address. Glass blur available. |
| 13 (33) | `POST_NOTIFICATIONS` runtime permission. "Restricted settings" for sideloaded notification access. Lens refraction (RuntimeShader) available. Per-app language via `LocaleManager`. |
| 14 (34) | FGS type `connectedDevice` requires `FOREGROUND_SERVICE_CONNECTED_DEVICE` + one of BT/network permissions (`CHANGE_NETWORK_STATE`/`CHANGE_WIFI_MULTICAST_STATE` are declared, so it is always satisfied). |
| 15 (35) | `BOOT_COMPLETED` may not start some FGS types; `connectedDevice` is still allowed. `onTimeout` is handled (no limit applies to this type). |
| 16 (36) | `ObservingDevicePresenceRequest` / `onDevicePresenceEvent`. |
| 17 (37) | `ACCESS_LOCAL_NETWORK` runtime permission requested in-app; LAN degrades gracefully. Predictive back enabled. |

Doze can still defer network delivery when the screen is off. A CDM association usually exempts the
app; otherwise, guide the user to *Battery › Unrestricted* (the app opens that list, it does not
bypass it).

### OEM notes

Xiaomi/HyperOS (Autostart + "No restrictions"), Huawei/Honor (App launch › manage manually),
OPPO/OnePlus/realme (allow background activity + auto launch), vivo/iQOO (background power
consumption), Samsung ("Never sleeping apps"). These are shown in-app under *Permissions &
background*.

## UI

* Kyant0 **liquid glass** (`drawBackdrop` with vibrancy + blur + lens) everywhere. The page background
  (current artwork, heavily blurred, or a solid neutral colour; no gradients) is the backdrop layer
  for cards. Background plus page content is the backdrop of the floating mini player, sheets and
  dialogs.
* The `LiquidToggle`, `DampedDragAnimation` and drag inspector are adapted from the
  Backdrop catalog (Apache-2.0, attribution in each file).
* **Navigation** (no tab bar): **Now Playing** is the home screen and uses the full height, laid
  out like Apple Music: artwork, title, progress, transport, volume and an action row with
  Lyrics · playback source (with a caption; opens the "Playing on" sheet) · Settings. Sliders are
  thin theme-coloured tracks that grow while dragged and own their gesture. **Settings** is pushed on top (slide, predictive back follows the gesture) with
  its own sub-pages; a floating glass mini player is docked at the bottom of the Settings stack
  only. Back order: sheet → lyrics mode → sub-page → Settings → Now Playing → leave the app (the
  task moves to the back, state survives).
* **Now Playing** is one full-screen player. The quote-bubble toggle (or tapping the artwork) morphs
  it in place into **lyrics mode**: the artwork and title fly into a compact header via
  `SharedTransitionLayout`, the volume row folds away and synced lyrics fill the middle.
  * Lyrics follow a smoothed clock (`SmoothedClock`: snap on seek / play / pause / track change,
    otherwise ±5 % slew, never backwards) minus the lyrics delay (output latency + per-app offset),
    see `docs/ipc.md` "Lyrics timing" (same rules as Windows).
  * AMLL / Apple-Music style: the current line sits at ~32 % with a continuous feathered fill (word
    timing per word, line-timed lines sweep over `min(0.9·d, max(1200, codePoints·450))` ms), the
    unsung part dimmer but brighter than other lines, a spring scale-up; other lines dim (and blur
    lightly on API 31+) with distance; instrumental gaps ≥ 4 s show breathing "•••"; translations
    under each line. The fill is drawn in the draw phase (no per-frame recomposition).
  * A manual scroll pauses auto-follow (4 s after the finger lifts) and shows "Back to current
    line"; tapping a line seeks.
  * A stopwatch button opens a small glass control for the per-app lyric offset (−0.1 s / +0.1 s /
    reset). Settings › Lyrics: automatic Bluetooth delay compensation (with the detected route),
    the Bluetooth delay, and the list of per-app offsets.
  * Plain lyrics are shown statically; without lyrics the toggle is hidden.
* **Control target** (like the desktop): the player shows and controls either **this phone's** media
  sessions or the **PC's** sessions.
  * A liquid-glass "Playing on" sheet lists *Automatic*, *This phone* sessions and *<PC name>*
    sessions, with app names and status.
  * *Automatic* (the default) follows whichever source started playing most recently.
  * The choice is remembered; the mini player and the blurred background follow it.
  * Phone sessions are controlled directly through their `MediaController` transport controls. PC
    sessions use `media.command`.
  * Lyrics for either side come from the host's `lyrics.push`, keyed by `trackId`.
* **Volume**: a liquid-glass volume slider for the selected device (tap the speaker to mute).
  * Phone: `AudioManager` STREAM_MUSIC. Changes are applied without system UI. Devices with fixed
    volume show the slider disabled with an explanation; DND refusals are caught.
  * PC: `volume.set` to the host, showing its `volume.state`. When the PC session reports its own
    app volume (`volume` in the snapshot), a second slider "<app name>" sends `volume.set` with
    `key`.
* **Interaction model** (see `../docs/android-ux-audit.md`):
  * Play/pause and seek are optimistic, with rollback and a toast on failure.
  * Auto follows the most recently started playing source (PC or phone), decided in the engine
    from snapshots with timestamps (also in the background). It holds the current target for 8 s
    after you control it in MediaDuet and for 3 s after it pauses by itself, switches at most once
    per 3 s, and keeps the last target while nothing plays.
  * Swipe the artwork for next / previous.
  * The source sheet, lyrics mode, Settings and its sub-pages share one back stack, with
    predictive back.
  * Lyric translations (`lyrics.push.translation`, merged ±600 ms) are shown under each line, with a
    toggle.
  * Haptics follow system settings.
* On the Settings stack a liquid-glass **mini player** is docked at the bottom. Tap or swipe up to
  return to Now Playing; swipe left / right for next / previous.
* **Crash reports** stay on the phone: an uncaught exception writes the stack trace plus the last
  200 log lines (newest 5 reports kept); Settings › Diagnostics offers Share / Copy once.
* **Settings** is an iOS-style grouped list with live summaries and sub-pages:
  * Connection
  * Permissions & background
  * Playback arbitration
  * Lyrics & display
  * Audio
  * Language
  * Diagnostics
  * About

  Sub-pages slide in and support predictive back.
* Per-app language: system / 简体中文 / English (`android:localeConfig`; `LocaleManager` on 33+, a
  wrapped base context below). Respects "Remove animations" (animator duration scale 0).
* Screenshots: [`../docs/screenshots/android/`](../docs/screenshots/android/) (light/dark × zh/en).

## Testing

* `./gradlew :app:testDebugUnitTest`: JVM tests. They cover:
  * every crypto vector (public keys from scalars, ECDH, transcripts, SAS `410860`, MACs, session
    keys, records, audio key, blob chunk, trackIds);
  * framing with partial reads and limits;
  * envelope validation;
  * the de-dup window;
  * handshakes against an independent test client over loopback sockets (pairing accept, reject and
    disabled; wrong MAC; unpaired; version mismatch);
  * link sessions (resume, transport switching via `link.primary`, duplicate commands across
    transports executed once, artwork BLOBs, revocation);
  * snapshot mapping and suppression, the LRC parser and position interpolation.
* `../protocol/tools/peer_client.py`: an independent Python implementation of the **client** role
  for interop testing over `adb forward tcp:47321 tcp:47321`:

  ```bash
  python -I ../protocol/tools/peer_client.py --state peer.json pair --yes          # prints the SAS
  python -I ../protocol/tools/peer_client.py --state peer.json session --pause --dup --artwork art.jpg
  python -I ../protocol/tools/peer_client.py --state peer.json session --demo cover.jpg --lyrics song.lrc --policy --listen 600
  python -I ../protocol/tools/peer_client.py --state peer.json session --audio 5 --adb <path-to-adb>
  ```
* Volume interop: `session --volume-set 0.3`, `--delta 2`, `--mute`, `--unmute`.
  `session --demo …` acts as a PC with a device volume and a per-app session volume and answers the
  phone's `volume.set`.
* `testplayer/` (**test fixture**): a tiny app with a real `MediaSession` playing a generated sine
  tone (title/artist/album/artwork/duration; play, pause, next, previous, seek). Use it to exercise
  the MediaSessionManager path on an emulator:
  `adb shell am start -n app.mediaduet.testplayer/.PlayerActivity --ez autoplay true`.

## Known limitations

* **RFCOMM** was started on the emulator (virtual Bluetooth), but an actual PC↔phone RFCOMM link
  needs real bonded hardware and has not been tested end-to-end.
* **ACCESS_LOCAL_NETWORK** (Android 17): the emulator connection used `adb forward` (loopback),
  which this permission does not govern. Real-LAN behaviour with the permission denied was not
  observed.
* **Companion device**: association needs a real nearby Bluetooth device; on the emulator only the
  UI path exists. Presence wake-ups are unverified.
* **Audio receive** (experimental): decryption and playback were verified (all records decrypted,
  `AudioTrack` USAGE_MEDIA created). On the emulator the audio sink drained slower than real time,
  so the bounded jitter queue (~300 ms) dropped packets. Latency and drift on real devices are
  untested. No resampling or drift correction is applied.
* Capabilities are announced at handshake time. Enabling audio receive applies to new connections.
* Media-session access is required to *share* phone media. Players that report no actions are shown
  but cannot be controlled (`action_unsupported`). Remote `http(s)` artwork URIs are not downloaded.
* With several paired PCs connected at once, the UI focuses on the most recently attached session.
* Volume: Android has no per-app volume, so a `volume.set` with `key` sent *to* the phone answers
  `unsupported`. While STREAM_MUSIC is muted, `AudioManager` reports index 0, so `volume.state`
  carries `level: 0, muted: true`; the level returns on unmute.
* `btName` in `transport.info` is sent only when `BLUETOOTH_CONNECT` is granted. It is re-announced
  when the Bluetooth state changes.
