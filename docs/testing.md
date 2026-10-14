# Testing

Three layers: deterministic unit tests (both platforms), cross-implementation
protocol vectors, and real end-to-end runs (Windows host ↔ Android app on an
emulator). Real-hardware checks that could not be done here are listed at the end
with step-by-step procedures.

## 1. Unit tests

| Suite | Command | Result (2026-10-13) |
|-------|---------|--------------------|
| Windows core (doctest) | `cmake -S windows -B windows/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build windows/build && windows\build\md_tests.exe` | **106 / 106 passed, 675 assertions** |
| Android (JUnit, JVM) | `cd android && ./gradlew :app:testDebugUnitTest` | **112 / 112 passed** |
| WinUI logic (headless, doctest) | see `windows/ui/README.md` (`windows/ui/tests/ui_logic_tests.cpp`, built with `cl`, no window) | **22 / 22 passed** |
| Android lint | `./gradlew :app:lintDebug` | 0 errors, 0 warnings (MissingTranslation, HardcodedText, plurals are errors) |

Windows coverage: base64/hex, HKDF (RFC 5869), all protocol vectors, ECDH
invalid points, AES-GCM tampering, framing (byte-by-byte, limits), envelope
validation, snapshot/command validation, position extrapolation, LRC
(multi-stamp, offset, enhanced word timing, garbage), lyric matching, settings
(merge/validation/atomic save), handshake (pairing accept/reject/disabled,
session, not_paired, wrong key, tampered MAC, timeout), link session (dedup,
timeouts, re-send, reset), connection manager (BT preferred, failover, stable
switch-back, flapping BT, exactly-once commands across failover, duplicate
delivery on two transports, resume vs new session, LAN-only/BT-only, exponential
back-off + presence reset, no adapter, revocation, user disconnect, silent-link
heartbeat), arbiter (all modes, user vs policy vs remote attribution, no echo
loops, override, cancelled resume, track change, calls, off/suspended, failed
command not retried, baseline snapshot, link loss, **flap damping**).

The fakes (`windows/tests/FakePeer.h`) implement the *server* side of the spec
independently from the production client and exist only in the test binary.

## 2. Cross-implementation protocol vectors

`protocol/tools/gen_vectors.py` (Python `cryptography`) generates
`protocol/test-vectors/crypto-v1.json`; the C++ (CNG) and Kotlin (JCA)
implementations both reproduce every value (ECDH, transcripts, SAS `410860`,
pairing key, MACs, session keys/IVs, record ciphertexts, audio key, blob chunk,
track ids). CI regenerates the file and fails on any diff.

## 3. End-to-end runs (real host ↔ real app)

Setup: `MediaDuet.exe` (Release) on Windows 11 26340; Android app (debug) +
`testplayer` fixture on an isolated, read-only **Android 16 (API 36)** emulator
(`emulator -avd Medium_Phone -read-only -no-window`), LAN via
`adb forward tcp:47360 tcp:47321`; host driven with `windows/tools/ipc_cli.py`
(same pipe protocol as the UI); phone UI driven with `uiautomator` + `input tap`.
The Android agent ran the same flows on **Android 17 (API 37)** with
`protocol/tools/peer_client.py`.

| # | Flow | Result |
|---|------|--------|
| E1 | Pairing over LAN: SAS shown on both sides (`331 284`), accepted on both | ✅ key stored (DPAPI / Keystore) |
| E2 | Session handshake + encrypted records, Windows client ↔ Android server | ✅ link up in 25 ms |
| E3 | Phone media → Windows: title/artist/status/position/artwork of the test player | ✅ artwork fetched over BLOB, SHA-256 verified, cached |
| E4 | Windows → phone control: pause, seek 60 s, play | ✅ Android MediaSession `PAUSED@60000` then `PLAYING` |
| E5 | Windows media → phone display (Edge/bilibili session incl. position) | ✅ |
| E6 | Phone → Windows control (Windows SMTC fixture player): pause, play, next, pause | ✅ fixture log `BUTTON pause/play/next/pause`, track advanced |
| E7 | SMTC bridge: phone session visible as a Windows system media session | ✅ `MediaDuet.Host` session with title, timeline, buttons; status follows the phone |
| E8 | Phone app killed (`am force-stop`) → relaunched | ✅ host detects loss, reconnects (new session) 14 s after relaunch (back-off) |
| E9 | Host restarted | ✅ pairing loaded from DPAPI, auto-reconnect |
| E10 | Arbitration with a real game producing audio (`deltaforceclient-win64-shipping.exe`, no media session) | ✅ phone paused, attributed `policy_paused`; auto-resumed 5 s after PC audio went idle |
| E11 | Bursty game audio caused pause/resume flapping (found in E10) | 🛠 fixed: flap damping (5 → 10 → 20 … s, ≤ 2 min), unit-tested; not re-run against the game yet |
| E12 | Active-session jump: after pausing, Windows' "current session" switched to another app and the phone followed it | 🛠 fixed: sticky active session; re-verified ✅ |
| E13 | Duplicate command delivery (peer_client) | ✅ executed once, cached result re-sent |
| E14 | Windows → phone PCM audio (peer_client as sender, API 37) | ✅ 400 frames decrypted, AudioTrack created; emulator audio drains slower than real time (85 drops) |
| E15 | Lyrics push Windows → phone; lyrics view, progressive fill | ✅ (screenshots `docs/screenshots/android/*_2_lyrics.png`) |
| E16 | Desktop lyrics overlay progressive fill | ✅ frames rendered offscreen via `MediaDuetOverlayDemo.exe <sec> <dir>` |

| E17 | Phone reinstalled (new identity) → host's old pairing answered `not_paired` | ✅ host removed the stale pairing and stopped retrying |
| E18 | Pairing not confirmed within 120 s | 🛠 was reported as `rejected`; fixed to `timeout` (unit-tested) |
| E19 | Real WinUI UI ↔ real host ↔ phone: UI shows phone track/artwork/progress/connection | ✅ `docs/screenshots/integration/main_real_host.png` |
| E20 | UI buttons (UI Automation `Invoke`) → host → device | ✅ Play/Pause paused the Android session; Next/Prev/Pause reached whichever session the UI was focused on (auto-focus had moved to the playing Windows fixture — verified in the fixture log) |
| E21 | Packaged build (`dist/…zip`): host starts the UI from its own folder; closing the UI keeps the host running | ✅ `packaged_first_run.png` |

| E22 | Real phone (OPPO Find X8 Ultra, NetEase Android) with the installed host over LAN, used daily by the user | ✅ control, arbitration, resume (from the user's host log) |
| E23 | A2DP sink on real hardware ("receiving Bluetooth audio from OPPO Find X8 Ultra") | ✅ from the user's host log |
| E24 | Real phone advertises the MediaDuet RFCOMM record over Bluetooth (`5F3A8C2E…` in SDP) | ✅ probe; Bluetooth name mapping implemented for LAN-paired phones |
| E25 | "Unknown app" in the Windows media flyout | 🛠 fixed: AUMID shortcut + AppUserModelId registration; `shell:AppsFolder\MediaDuet.Host` resolves to "MediaDuet" |
| E26 | Multipoint-headset bounce (phone paused by the earbuds right after Play) | 🛠 headset handover implemented; 6 unit tests; needs a real multipoint headset to confirm |
| E27 | Lyrics hit rate, 30 mixed tracks (`MediaDuetLyricsTest`) | ✅ 30/30 (15 word-timed, 13 with translations = every non-Chinese track) vs 18/30 with LRCLIB only; QQ Music rate-limits bursts (fallback endpoint verified) |
| E27b | Lyric translations: NetEase `tlyric`, QQ Music `PlayLyricInfo.trans` merged per line (±600 ms); `lyrics.push.translation` to the phone; overlay/taskbar second row | ✅ unit test `translations merge by timestamp`; NetEase 13/13 foreign tracks, QQ 5/5 of its hits translated; taskbar frames rendered offscreen. Desktop overlay frames not captured on the dev machine (hidden by design while a full-screen app runs) |
| E28 | Taskbar widget on the Windows 11 taskbar (left and right placement, hover controls) | ✅ frames from the widget surface (`MediaDuetOverlayDemo --taskbar`) |
| E29 | Android round 2: phone/PC source switcher, device + per-app volume, `btName` | ✅ peer_client + UI on an isolated Android 16 emulator |
| E30 | WinUI round 2: settings as a sub-page, volume flyout, new settings, no clipping with long names | ✅ fake host + UI Automation; not yet against the real host |

| E31 | Round 4 (2026-10-13): Android crash on track change (`NullPointerException` in the lyrics fade-out after `next`) | 🛠 reproduced on an API 37 emulator, fixed; PC next/prev bursts and 12 rapid skips keep the process and link alive; crash reports saved locally (Settings › Diagnostics) |
| E32 | Host stuck reconnecting to a phone address that accepts TCP but never handshakes (user log) | 🛠 endpoint ranking/demotion + staggered handshakes; unit test replays the user's sequence (reconnect ≈ 6 s, no host restart); real-phone re-test pending |
| E33 | Taskbar widget hung / restarted Explorer and then disappeared until restart | 🛠 no cross-process owner, UIA on a worker thread with timeouts, `TaskbarCreated` rebuild, watchdog; logic unit-tested; frames rendered offscreen. Live taskbar / Explorer restart **not** exercised (by design) |
| E34 | Phone volume from the PC ignored while the Android app is in the background (Android 17 background-audio hardening) | 🛠 FGS restarted when visible (WIU granted, verified via dumpsys on the emulator), read-back detection → `background_restricted`, notification to re-grant; the blocking itself could not be reproduced on the emulator image — pending on a real Android 17 phone |
| E35 | Lyrics timing: smoothed clock, output-latency (`outputLatencyMs`) and per-app offsets on overlay, taskbar, WinUI and Android | ✅ unit tests on all three (shared karaoke vectors); real Bluetooth latency values pending on hardware |
| E36 | Desktop overlay redesign (always on top, outlined text, alternate/center layout, scrolling long lines, hover toolbar, unlock button) | ✅ frames rendered offscreen (`docs/screenshots/windows-overlay/`); z-order and mouse interaction pending a visible run |
| E37 | WinUI: dithered background (banding), AMLL-style lyrics, offset control, interaction fixes | ✅ Release build, headless logic tests; visual check pending |
| E38 | Android round 4: no bottom tab bar, roomier layout, AMLL-style lyrics, offset control | ✅ headless emulator, screenshots `docs/screenshots/android/r4_*`; audit in `docs/android-ux-audit.md` |

| E39 | 1.2.1 WinUI: double karaoke fill on lines with spaces; in-app lyrics out of sync with the overlay; abrupt lyrics/no-lyrics layout change; lyrics hugging the left when maximized | 🛠 segment-based fill (no caret hit-testing), UI now uses the host's `windows/core` lyrics clock/karaoke code; host-vs-UI timeline test agrees within 7.6 ms (GetTickCount64 granularity), same line 100 %; animated layout transitions; centred responsive layout. Checked with the invisible render harness (`MediaDuetUI.exe --render-test <dir> --pipe <private>`, off-screen, never activated, fake host only) |
| E40 | 1.2.1 Android: Auto source switching too sticky | 🛠 engine-level reducer (most recent start wins, 3 s / 8 s holds, ≤ 1 auto switch per 3 s); 12 resolver tests; emulator: switch on first snapshot / after 2.9 s / held 8 s after a MediaDuet tap |
| E41 | QQ Music lyrics flicker (lyrics vanish and come back) | 🛠 root cause: the GSMTC track id was computed before the timeline was read and changed when the duration arrived / jittered; every surface then reloaded. Fixed in the source (stable duration, blank-read grace) and in both apps (same-song re-keying instead of reload). Core tests (`sameSong`, rekey), Android `LyricsRepositoryTest`; not yet checked against a live QQ Music session |
| E42 | Android: lyrics without a PC (phone looks up NetEase / QQ Music / LRCLIB itself) | ✅ API 37 emulator, fixture track 晴天 / 周杰伦: 63 synced lines from QQ Music, no PC connected |
| E43 | Android round 5: MingCute icons, liquid-glass controls, compact lyrics dock + options popover, landscape player / lyrics, work mode (first-run choice, standalone), developer card | ✅ headless API 37 emulator, light/dark, en/zh-CN, portrait/landscape; 139 JVM tests, lint clean |
| E44 | Windows: HarmonyOS Sans lyrics, work mode, version check, taskbar placement (Win10 / Win11 left-aligned), online covers | ✅ core 127/127, WinUI logic tests, off-screen render harness; taskbar layouts and the cover upgrade covered by unit tests only (no live Windows 10 / left-aligned taskbar here) |

### Measured resource use (Windows 11, 20 logical CPUs, Release x64)

| Process | State | CPU | Memory |
|---------|-------|-----|--------|
| `MediaDuet.exe` host | idle, phone unreachable (retrying) | 0.13 % of one core over 60 s (0.0065 % total) | 7.5 MB private, 45 MB working set, 13 threads |
| `MediaDuetUI.exe` | main window open, paused / idle | ≈ 0 | 86 MB private, 104 MB working set |
| `MediaDuetUI.exe` | lyrics animating | ≈ 45 ms/s (main), 40 ms/s (compact) | — |
| Android app | see `android/README.md` (event driven; no polling) | not measured on hardware | — |

Control latency over loopback LAN: link establishment 25 ms; command round trips
complete well under the 500 ms target (exact per-command latency is shown in the
diagnostics view; not benchmarked on a real Wi-Fi network).

Fixtures used only for testing: `android/testplayer` (MediaSession + sine tone),
`windows/tools/test_smtc_player` (silent SMTC session that logs commands),
`windows/tools/overlay_demo`, `windows/tools/ipc_cli.py`, `protocol/tools/peer_client.py`.

## 4. Not verifiable in this environment (needs real hardware)

| Item | Why not here | Procedure |
|------|--------------|-----------|
| RFCOMM between Windows and a physical phone | emulator Bluetooth is virtual and not reachable from the PC | Install the APK on a phone bonded with the PC (Settings → Bluetooth), grant notification access, open *Settings → Connection → Pairing mode*; on Windows *Settings → Devices → Pair new phone*, pick the phone (Bluetooth). Expect status "Connected via Bluetooth". Then turn Wi-Fi on both: status shows `bt` primary, `lan` secondary. |
| BT → LAN failover / switch-back on hardware | as above | With both up, turn the phone's Bluetooth off: control continues (LAN). Turn it on: after ≤ 10 s stable, primary returns to `bt` (Diagnostics shows it). |
| A2DP sink (phone audio on PC) | needs a person to accept routing on the phone | Windows *Settings → Audio → Receive phone audio*; phone audio should play on the PC; arbitration shows "suspended". |
| NetEase Cloud Music / QQ Music / Spotify / YouTube Music on Android | not installed on emulators | Play a song; check title/artist/cover/progress on Windows; pause from Windows. |
| NetEase / QQ Music Windows clients | not installed here | Same in the other direction. |
| Android 15/16/17 background survival on OEM ROMs | needs physical devices | Pair, lock the phone 30 min, play music, control from PC; see `android/README.md` for battery settings. |
| `ACCESS_LOCAL_NETWORK` enforcement on Android 17 | emulator test used loopback | Deny the permission → LAN connection must fail and the app must show it; Bluetooth keeps working. |
| CompanionDeviceManager presence wake-up | needs a nearby real BT device | Associate the PC, force-stop the app, connect the PC's Bluetooth: the service should start. |
| Windows 10 | only Windows 11 available | Run `MediaDuetProbe.exe`, then the flows above. |
