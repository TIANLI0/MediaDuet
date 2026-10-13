# Compatibility

Legend: ✅ verified · 🧪 verified on emulator/fixture only · 🔧 implemented, needs real-device verification · ⚠️ limited · ❌ not supported

## Operating systems

| Platform | Status | Notes |
|----------|--------|-------|
| Windows 11 (build 26340, x64) | ✅ | development and all end-to-end runs |
| Windows 11 ARM64 | 🔧 | UI builds for ARM64; host ARM64 build not attempted yet (`package.ps1 -Arch ARM64` needs the ARM64 MSVC tools); not run |
| Windows 10 2004+ (19041) | 🔧 | APIs used exist on 19041 (GSMTC, SMTC interop, AudioPlaybackConnection, WinAppSDK 2.x min); not run |
| Windows 10 < 2004 | ⚠️ | A2DP sink unavailable; UI requires 19041 |
| Android 17 (API 37, target) | 🧪 | emulator: engine, NSD, TCP, RFCOMM listen, sessions, pairing/session interop, audio receive |
| Android 16 (API 36) | 🧪 | emulator: full end-to-end with the Windows host (E1–E21) |
| Android 10–15 (API 29–35) | 🔧 | minSdk 29; version-specific code paths compiled and linted (FGS types, CDM presence, notification permission) |

## Media apps

| App | Platform | Status | Notes |
|-----|----------|--------|-------|
| Microsoft Edge (incl. bilibili, YouTube) | Windows | ✅ | read + control (Edge disables Pause for some sites; we honour `Controls`) |
| SMTC fixture (`windows/tools/test_smtc_player`) | Windows | ✅ | full control from the phone |
| Chrome / Firefox | Windows | 🔧 | standard SMTC |
| Spotify | Windows | 🔧 | standard SMTC |
| NetEase Cloud Music (`cloudmusic.exe`) 3.1 | Windows | 🔧 | 3.1 contains an SMTC wrapper (enabled by its server config `preload#SMTCEnable`); when no session is published, the **compatibility adapter** (window title + local queue + Core Audio + WM_APPCOMMAND, exact lyrics by song id) takes over — awaiting a live test |
| QQ Music (`QQMusic.exe`) | Windows | 🔧 | publishes SMTC in recent versions |
| Games / apps without SMTC | Windows | ✅ arbitration only | detected via Core Audio (verified with Delta Force); cannot be paused remotely |
| Test player fixture | Android | 🧪 | full read/control |
| NetEase Cloud Music `com.netease.cloudmusic` | Android | 🔧 | standard MediaSession |
| QQ Music `com.tencent.qqmusic` | Android | 🔧 | standard MediaSession |
| Spotify, YouTube Music | Android | 🔧 | standard MediaSession |

## Features by acceptance scenario

| # | Scenario | Implemented | Automated test | Emulator / fixture e2e | Real device | Notes |
|---|----------|:-:|:-:|:-:|:-:|---|
| 1 | Bluetooth only, no common LAN | ✅ | ✅ (CM: BT-only) | ❌ | 🔧 | emulator BT not reachable from PC |
| 2 | BT + Wi-Fi → BT primary | ✅ | ✅ | ❌ | 🔧 | |
| 3 | BT off → LAN takeover | ✅ | ✅ (exactly-once) | ❌ | 🔧 | |
| 4 | BT back → stable switch-back | ✅ | ✅ (incl. flapping) | ❌ | 🔧 | |
| 5 | NetEase on phone → Windows shows title/artist/cover/state/progress | ✅ | ✅ (wire format) | ✅ with test player | 🔧 NetEase | |
| 6 | Pause on Windows → phone player pauses | ✅ | ✅ | ✅ (E4, E20) | 🔧 | |
| 7 | Phone controls Windows media | ✅ | ✅ | ✅ (E6) | 🔧 | |
| 8 | Windows audio → pause phone | ✅ | ✅ | ✅ (E10, real game) | 🔧 | |
| 9 | Windows stops → resume per policy | ✅ | ✅ | ✅ (E10) | 🔧 | |
| 10 | User pause never auto-resumed | ✅ | ✅ | — | 🔧 | |
| 11 | No bidirectional event loop | ✅ | ✅ | ✅ (expectation attribution, E10/E11) | 🔧 | |
| 12 | Close windows → background sync continues | ✅ | — | ✅ (E21) | — | |
| 13 | Android backgrounded keeps/restores link | ✅ | ✅ (server) | ⚠️ restart path only (E8) | 🔧 | OEM ROM behaviour varies |
| 14 | Permission revoked → correct status, no crash | ✅ | ✅ (Android) | 🧪 (permissions page) | 🔧 | |
| 15 | Windows restart → resumes per settings | ✅ | — | ✅ host restart reloads pairing (E9) | 🔧 login autostart | HKCU Run key |
| 16 | No lyrics → card still works | ✅ | ✅ | ✅ (E19, no lyrics panel) | — | |
| 17 | No Bluetooth adapter → LAN works | ✅ | ✅ | ✅ LAN-only run | — | |
| 18 | Audio forwarding unavailable → control unaffected | ✅ | ✅ | ✅ (audio off in all runs) | — | |

## Audio

| Direction | Mechanism | Status |
|-----------|-----------|--------|
| Android → Windows | Bluetooth A2DP sink (`AudioPlaybackConnection`) | 🔧 implemented; device eligible (Phase 0); needs a person to route audio on the phone |
| Windows → Android | WASAPI loopback → encrypted PCM over TCP → AudioTrack | 🧪 experimental; Android receive verified with peer_client; Windows sender not run against a phone |
| Android → Windows over LAN | AudioPlaybackCapture | ❌ not implemented (consent per session, apps opt out, DRM); protocol reserved |
