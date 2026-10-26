<p align="center"><img src="windows/app/resources/app_512.png" width="96" alt="MediaDuet icon"></p>

<h1 align="center">MediaDuet</h1>
<p align="center"><strong>Connect playback on your Windows PC and Android phone.</strong><br>
Remote controls, automatic pause and resume, and synced lyrics.</p>

<p align="center">
  <a href="https://github.com/TIANLI0/MediaDuet/releases/latest"><strong>Download</strong></a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="https://tianli0.github.io/MediaDuet/">Website</a> ·
  <a href="README.md">中文</a>
</p>

MediaDuet lets you control your phone's playback from your PC, or your PC's playback from your phone. Your usual music and video apps handle playback.

For example, listen to music on your phone while working at your PC: skip tracks without picking up the phone, and automatically pause phone music when PC video or game audio starts. Music can resume when the PC goes quiet.

## Features

| What you want to do | What MediaDuet offers |
| --- | --- |
| Control the other device | Track information, artwork, play/pause, skipping, seeking and volume, where supported by the player |
| Avoid overlapping playback | Automatic pause and resume with PC-first, phone-first, latest-started or custom priorities; music you paused yourself stays paused. Phones and tablets paired with each other arbitrate among themselves too |
| See lyrics | Synced lyrics, word highlighting and translations in both apps; desktop and taskbar lyrics on Windows |
| Continue on another device | Handoff through NetEase Cloud Music, QQ Music or Spotify, with an attempt to resume at the current position |
| Use just one device | “This PC only” or “This phone only” for local controls and lyrics, without pairing |

**Remote control keeps audio on the original device's output.** Handoff opens the song in the other device's player. Audio forwarding sends sound to the other device; it is a separate, optional feature.

## Preview

<table>
  <tr><th>Windows</th><th>Android</th></tr>
  <tr>
    <td><img src="website/assets/win-player-zh.webp" width="680" alt="Windows player with artwork, controls and synced lyrics (Chinese UI)"></td>
    <td><img src="website/assets/android-player.webp" width="210" alt="Android player with track information and playback controls"></td>
  </tr>
</table>

## Download

Choose your files from the [latest release](https://github.com/TIANLI0/MediaDuet/releases/latest). Use matching versions on both devices where possible.

| Device | Requirements | File |
| --- | --- | --- |
| Windows PC | Windows 10 2004+ / Windows 11, x64 | `MediaDuet-<version>-x64-setup.exe`, or portable `MediaDuet-<version>-win-x64.zip` |
| Android phone | Android 10+, 64-bit ARM | `MediaDuet-<version>-android.apk` |

Extract the entire portable ZIP before running `MediaDuet.exe`. Install and open the APK on Android. Windows 10 and some Android versions still need physical-device verification; see [compatibility](docs/compatibility.md).

## Quick start

1. **Open both apps** and choose to use them with another device. For local controls and lyrics only, choose “This PC only” or “This phone only” and skip pairing.
2. **Grant phone permissions** in **Settings → Permissions & background**: notification access for media sessions, the permissions needed for your connection, and background activity. Notification contents are never read, stored or sent. If notification access is blocked, enable **App info → ⋮ → Allow restricted settings** in Android settings first.
3. **Pair over your LAN.** Connect both devices to the same network; Ethernet on the PC is fine. On the PC, open **Settings → Devices & pairing**: it shows a QR code (or click **Show QR code**). On the phone, tap **Scan to link a PC** at the top of the player (or **Settings → Connection → Scan to pair**) and scan it; there is nothing to compare. Alternatively, enable **Settings → Connection → Pairing mode** on the phone (valid for five minutes), select the phone under **Pair a new phone** on the PC, compare the six-digit code and confirm on both devices. If discovery fails, use **Connect by IP address** with the address and port shown in the phone app (default `47321`).
4. **Play a song** in your phone's usual music app, then try pausing or skipping from the PC. To control PC playback from the phone, start playback on the PC first.

For Bluetooth, pair the devices in **Windows Settings → Bluetooth & devices** first, then complete MediaDuet pairing. Auto connection mode supports switching between LAN and Bluetooth. Physical-device Bluetooth verification is still pending; see [compatibility](docs/compatibility.md).

If phone tracks are missing, check notification access and make sure the player is playing. If the connection drops when the screen locks, allow background activity and remove battery restrictions. Closing PC windows leaves MediaDuet running in the system tray; use the tray menu to exit.

## Optional features

- **Automatic pause and resume:** configure **Settings → Playback arbitration** on the PC. You can disable automatic resume, adjust its delay or restrict it to selected audio outputs. Games can trigger phone pauses but usually cannot be paused remotely.
- **Lyrics notifications:** enable the Android option to show the current line in the notification shade and on the lock screen. Android 16+ Live Updates and ColorOS 16 Fluid Cloud presentation depend on system support, permissions and settings.
- **Handoff:** use “Continue on phone / Continue on PC” on the PC, the PC taskbar's continue button while the phone plays, or the phone's playback-source panel. The target needs the player installed and access to the song. Some targets only open a song or search results; background Android launches may need a notification tap. Several adapters, including Spotify, still need physical-device verification. See the [handoff compatibility table](docs/compatibility.md#handoff-targets-continue-on-the-other-device-protocol-87).
- **Audio forwarding:** phone → PC uses Bluetooth and still needs physical-device compatibility verification. Experimental PC → phone forwarding uses the LAN and has noticeable latency, making it unsuitable for watching video. Phone → PC over LAN is not implemented. See [audio routing](docs/audio-routing.md).

The [detailed usage guide](docs/usage.md) is currently in Chinese.

## Privacy and network access

No MediaDuet account or cloud relay is required. Devices communicate directly over encrypted LAN or Bluetooth connections.

Online lyrics, artwork and handoff searches contact external providers using song information such as title, artist, album and duration. Providers include NetEase Cloud Music, QQ Music, LRCLIB and iTunes. Update checks and downloads use GitHub. Online lyrics providers, HD artwork and automatic update checks can be disabled in settings. Your music player's account, subscription and catalogue restrictions still apply.

## Development

Windows uses C++20 / WinUI 3; Android uses Kotlin / Jetpack Compose.

- [Build and test commands](README.md#开发) in the Chinese README
- [Windows UI development](windows/ui/README.md) and [Android development](android/README.md)
- [Architecture](docs/architecture.md), [protocol](protocol/specification.md) and [Bluetooth / LAN connections](docs/bluetooth.md)
- [Compatibility](docs/compatibility.md) and [test records](docs/testing.md)

## License

[MIT](LICENSE) © 2026 tianli. Third-party components retain their own licenses; see [third-party notices](docs/THIRD_PARTY_NOTICES.md). The bundled HarmonyOS Sans font is covered by its own font license, outside the MIT license.
