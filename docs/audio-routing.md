# Audio routing

Audio is an optional module. Media control never depends on it.

## Android → Windows: Bluetooth A2DP sink (supported)

Windows 10 2004+ can act as an A2DP **sink** through
`Windows.Media.Audio.AudioPlaybackConnection`:

1. `DeviceWatcher` on `AudioPlaybackConnection.GetDeviceSelector()` lists paired
   phones that are A2DP sources (Phase 0: *OPPO Find X8 Ultra* was eligible).
2. On "Receive phone audio", the host calls `TryCreateFromId` → `StartAsync` →
   `OpenAsync`; the phone then sees the PC as a Bluetooth audio output and its
   audio plays on the PC's current output device.
3. The connection object is kept alive while enabled; `Close()` on disable.
   Closure by the phone is reported (`StateChanged`).

RFCOMM (control) and A2DP (audio) are separate profiles over the same bond.
While A2DP is active the arbiter is suspended (both streams already share one
output) and PC→phone streaming is disabled to avoid a loop.

Status: implemented; **requires real-device verification** (needs the phone to
accept the audio connection — not possible in this environment without
interacting with the phone).

## Windows → Android: LAN PCM stream (experimental)

* Capture: WASAPI **loopback** of the default render endpoint, shared mode,
  event driven, `AUTOCONVERTPCM` to 48 kHz stereo float, MMCSS "Pro Audio".
* Transport: separate TCP connection to a port the phone opens after
  `audio.offer`; first 16 bytes = stream id; then AES-256-GCM records (spec §9)
  of 10 ms PCM s16le frames with a sender timestamp.
* Back-pressure: frames are dropped *before* sealing when the socket is not
  writable (keeps latency bounded, keeps the record sequence contiguous).
* Resources (thread, audio client, socket, buffers) exist only while streaming.
* Protected content: Windows renders DRM-protected streams silent in loopback;
  nothing is bypassed.
* Bandwidth ≈ 1.5 Mbit/s (no codec yet). Opus would reduce it ~10× but needs
  libopus on Windows and MediaCodec Opus on Android; not implemented.
* Only over LAN; never over RFCOMM.

## Android → Windows over LAN (not implemented)

`AudioPlaybackCapture` needs a MediaProjection consent each session and only
captures apps that allow it (`allowAudioPlaybackCapture`, usage media/game);
NetEase and QQ Music commonly opt out, and DRM content is excluded. Given that
the A2DP sink path covers this direction natively, LAN capture from Android is
**not implemented**; the protocol (`audio.offer` with
`direction:"android_to_windows"`) is reserved for it.

## Ordinary Android phones as A2DP sinks

Most phones do not expose an A2DP sink role to apps; MediaDuet does not rely on
it.
