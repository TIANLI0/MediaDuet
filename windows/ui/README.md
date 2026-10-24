# MediaDuetUI — WinUI 3 user interface

`MediaDuetUI.exe` is the thin, on-demand UI process of MediaDuet on Windows. All logic (links,
media sessions, pairing, arbitration, lyrics fetching, the desktop-lyrics overlay) lives in the
host `MediaDuet.exe`. The UI renders the host's `state` / `lyrics` / `diagnostics` messages and
sends commands over the named-pipe JSON-lines IPC described in [`docs/ipc.md`](../../docs/ipc.md).

C++20 · C++/WinRT · WinUI 3 (Windows App SDK) · unpackaged, self-contained · x64 and ARM64.

## Build

Requirements: Visual Studio 2022 17.x or Visual Studio 2026 with the *WinUI application development*
(C++) workload and a Windows 10/11 SDK. NuGet packages are restored automatically
(PackageReference, pinned versions):

| Package | Version |
|---------|---------|
| Microsoft.WindowsAppSDK.Foundation | 2.3.12 |
| Microsoft.WindowsAppSDK.InteractiveExperiences | 2.1.9 |
| Microsoft.WindowsAppSDK.WinUI | 2.3.9 |
| Microsoft.Windows.CppWinRT | 3.0.260818.1 |
| Microsoft.Windows.SDK.BuildTools | 10.0.28000.2705 |
| Microsoft.Windows.ImplementationLibrary | 1.0.260126.7 |

The three `Microsoft.WindowsAppSDK.*` packages are the components of the Windows App SDK 2.5.1
metapackage that the UI needs. Referencing the metapackage would also deploy the AI, ML, Search
and Widgets components (~175 MB instead of ~58 MB in the self-contained output).

```powershell
# from the repository root (Developer PowerShell, or call MSBuild.exe by full path)
msbuild windows\ui\MediaDuetUI.vcxproj -restore -p:Configuration=Release -p:Platform=x64
msbuild windows\ui\MediaDuetUI.vcxproj -restore -p:Configuration=Release -p:Platform=ARM64
# or the solution
msbuild windows\ui\MediaDuetUI.slnx -restore -p:Configuration=Release -p:Platform=x64
```

Output: `windows\ui\bin\<Platform>\Release\` — `MediaDuetUI.exe` plus the self-contained Windows App
SDK runtime and `MediaDuetUI.pri`. Copy the whole folder next to `MediaDuet.exe` (the UI launches the
host from its own directory). Intermediate files go to `obj\` and `Generated Files\` (git-ignored).

## Command line

```
MediaDuetUI.exe [--show main|compact|settings|pair]... [--pipe <name>] [--language auto|zh-CN|en-US]
```

* `--show` may be repeated. A second launch activates the running instance
  (`AppInstance::FindOrRegisterForKey` + activation redirection) and opens the requested window.
* `--show settings` opens the main window on its settings page, `--show pair` on *Devices & pairing*
  with the pairing flow started; `--show main` returns to the player page.
* `--pipe` overrides the default pipe `\\.\pipe\MediaDuet.UI.<user SID>` (used for testing). The
  single-instance key includes the pipe name, so a test instance never redirects to (or is redirected
  by) a production instance.
* The process exits when its last window closes (or after `host.exit`).

## Project structure

| File | Purpose |
|------|---------|
| `Main.cpp` | `wWinMain`: single instance + activation redirection, initial language, XAML start |
| `App.xaml(.h/.cpp)` | Application resources (styles, theme brushes) and launch |
| `Controller.*` | Process-wide controller: pipe, latest host state, events, typed commands, window lifetime, theme, runtime language switching |
| `PipeClient.*` | Overlapped named-pipe client (JSON lines, auto-reconnect with back-off, 4 MiB line limit) |
| `Model.*` | Tolerant parsing of `state` / `lyrics` (unknown fields ignored), position extrapolation (`GetTickCount64` + QPC anchor) |
| `Karaoke.*` | Progressive lyric line: text segments in a `LyricWrapPanel`, each with its own brush; one feathered fill edge placed in line x-space like the overlay; interlude rows (breathing dots) |
| `LyricWrapPanel.*`, `LyricSegments.h` | Wrapping panel for the segments; plain-C++ segmentation (words, CJK / emoji clusters, whitespace), wrapping and the line x-space fill mapping |
| `LyricsView.*` | Apple-Music-style synced lyrics view (per-line composition translations, emphasis, wheel / touch scrolling, follow pause, click to seek) |
| `LyricTiming.h` | Glue to the host's own lyrics code (`windows/core` `md::lyrics`: `LyricsClock`, `buildKaraoke`, `fillAt`, `lineAt`, compiled into the UI): clock reports, lyric position, display rows, clusters |
| `PlaybackClock.h` | Plain C++: optimistic seek hold, optimistic play/pause, offset formatting |
| `RenderTest.*` | Invisible off-screen render test (`--render-test`, see below) |
| `Ambient.*`, `AmbientRender.h` | Flowing artwork background (the Android `FlowingArtwork`): float scene + blur + TPDF dither rendered off the UI thread into a `WriteableBitmap`, crossfaded |
| `MainWindow.*` | Custom title bar with back button; player page (media card, progress/seek, source switcher, connection pill, arbitration line, volume flyout + inline volume, Apple-Music-style synced lyrics) and the settings page as a sub-page |
| `CompactWindow.*` | 340×96 mini player: artwork, song info, current lyric line; controls appear on hover / keyboard focus; edge snapping, position memory, per-monitor DPI |
| `SettingsPage.*` | Settings sub-page of the main window (left-rail NavigationView): general, devices & pairing (SAS dialog), connection, playback arbitration (headset assist, audio-output gate), lyrics (provider order), taskbar lyrics, Windows media integration, compatibility, audio, diagnostics, about |
| `Artwork.*` | Artwork decoded once per path at the displayed size; up to three dominant colours for the background tint |
| `WindowUtil.*` | Backdrop (Mica → Desktop Acrylic), caption colours, icon, min size, visibility/minimise tracking, work-area clamping |
| `Strings.*`, `Strings\<lang>\Resources.resw` | Localisation (x:Uid in XAML, `Str()`/`StrF()` in code) |
| `tools\gen_resw.py` | Single source of all UI strings (en-US + zh-CN). Generates both `.resw` files and fails if an `x:Uid` or `Str()` key is missing or unused |
| `tools\fake_host.py` | Fake host for development and tests (see below) |
| `tools\capture_window.ps1` | Saves a PNG of one window of a given process via `PrintWindow` (never a screen copy, so nothing else on the desktop is captured); used for `docs\screenshots\windows-ui` |
| `tools\make_icon.py` | Regenerates `Assets\MediaDuet.ico/.png` (Pillow) |
| `tests\ui_logic_tests.cpp` | Headless doctest tests of the plain-C++ headers (no XAML, no window), see *Tests* |

## Behaviour notes

* **Language** follows `settings.language`; `auto` uses the language the host reports in `hello`
  (falling back to the Windows display language). A change is applied at runtime and in-process:
  `PrimaryLanguageOverride` is updated and the open windows are recreated (same kind, page and
  placement), because x:Uid strings are only resolved when XAML loads. Every visible string comes
  from the `.resw` files; WinUI's own ToggleSwitch On/Off labels are overridden as they follow the
  OS language. Edit strings in `tools\gen_resw.py`, then run `python tools\gen_resw.py`.
* **Theme** follows `settings.theme` (`RequestedTheme` on each window root + caption button colours
  + DWM dark mode). Before the first `state` arrives, the persisted `%LOCALAPPDATA%\MediaDuet\settings.json`
  provides theme, language and window positions.
* **No polling.** Positions are extrapolated locally: `positionMs + (now - atTick) * rate`, with a
  QueryPerformanceCounter anchor (`qpcNow - (GetTickCount64() - atTick)`) for sub-frame precision.
* **Synced lyrics** (main window, `LyricsView`): Apple Music / AMLL style. One row per lyric
  line, so the current row is always the overlay's `lineAt()`; an intro row with breathing dots
  when the first line starts >= 4 s in, blank lines >= 4 s are interlude rows (dots that light up
  over the gap, breathe and shrink away), shorter blank lines are empty rows (nothing highlighted,
  like the overlay). The current line is fully emphasised: scale 1.0 (others 0.93), sung part in
  full colour, unsung part at ~50 % - always brighter than every other line, which fall off with
  distance (0.36 / 0.28 / 0.22 / 0.18); the emphasis animates in ~220 ms so it never lags the fill.
  The lines sit in a clipped Canvas (no ScrollViewer); each has its own `Translation` with an
  implicit transition (composition thread), lines below the current one arrive slightly later
  (wave). The current line sits at 38 % height; font size and spacing grow with the viewport height
  (26 -> 34 epx). New lyrics fade in line by line. Mouse wheel / touch scrolling pauses following
  for 4 s and shows "Back to current line"; clicking a line seeks (when the session can seek).
  Plain lyrics are a static, scrollable list. The compact window shows the current line with the
  same fill (long lines scroll like the desktop lyrics) and a 240 ms slide/cross-fade.
* **Lyrics typeface**: HarmonyOS Sans SC, shipped unmodified in `Assets\Fonts` with its licence
  (notice in Settings > About). Two weights: Bold (700) for the main window's lines, Medium (500)
  for the compact window's line and all translation rows; each TextBlock points at the file of its
  weight (`ms-appx:///Assets/Fonts/<file>#HarmonyOS Sans SC`, resolves in the unpackaged app) so no
  bold is synthesised. Missing files fall back to the system UI font (SemiBold / Normal).
* **Work mode** (settings `mode`: `linked` | `standalone`): the first run without a paired phone
  asks once in the main window (`modeChosen`); "This PC only" hides the connection pill, the
  arbitration line and the phone-only settings pages. **Versions**: Settings > Devices / About show
  the phone's app version; a major/minor mismatch (host: `md::version`) shows a warning InfoBar on
  the Devices page and a dismissible one (per version pair, `ui.versionNoticeDismissed`) in the player.
* **Karaoke fill** uses the host's own code (`md::lyrics::buildKaraoke` / `fillAt`), so it is the
  desktop overlay's fill: the line is laid out as segments (words, single CJK characters / emoji
  clusters, whitespace runs; segments cut out of one word stay glued when wrapping), every segment
  has its own brush, and the edge is placed in *line x-space* (the measured segment widths
  concatenated on one row) as `x(from) + (x(to) - x(from)) * frac`, exactly like the overlay's
  caret-based `fillX`. Segments before the edge are sung, the ones under the feathered edge
  (`max(6, 0.5 * font size)`) get a gradient, the rest are unsung - one edge, also across wrapped
  rows. (1.2.0 derived the fill from TextPointer caret rectangles and two clips; carets at spaces
  and soft wraps report other rows, which showed two fills.)
* **Lyrics timing and latency**: the clock is the host's `md::lyrics::LyricsClock` (QPC
  extrapolation, slew <= ±5 %, snap > 300 ms / play-pause / rate / track / seek), fed with each
  session report (`atTick` converted to the QPC time base on receipt); the lyric position is
  `clock - session.lyricsDelayMs`, applied once (0 with older hosts). The progress bar stays exact.
  `tests\ui_logic_tests.cpp` runs the host overlay pipeline and the UI pipeline over the same
  jittery timeline (delay 0 / 180 / -300 ms) and asserts the same line and fill. A clock button
  under the lyrics (only when the host reports `lyricsDelayMs`) opens the per-app offset (−0.1 s /
  +0.1 s / reset, `lyrics.offset`); `[` / `]` do the same in the player. Settings → Lyrics → Timing
  edits `lyrics.latency.auto`, `lyrics.latency.bluetoothMs` (shows `audioOutputLatency`) and lists
  `lyrics.offsets` with reset.
* **Lyrics layout transitions**: while the next track's lyrics are loading the layout holds (<=
  1.5 s) and the old lines fade; then the lyrics pane fades / slides out and the card glides to the
  centre (or the reverse), 420 ms decelerating composition animations (FLIP: the layout changes at
  once, the card animates from its old place). Off with `ui.animations=false`, low power or the
  Windows "Animation effects" setting (instant, no layout flash). The compact window cross-fades
  between the lyric row and the source row.
* **Background**: the same flowing artwork as on Android — four copies of a 48 px artwork
  thumbnail turning slowly around drifting centres, saturated (1.45), Gaussian-blurred (radius 0.16
  × the shorter side of a 1/8-size canvas) and under the Android scrim (dark: black 50 %, light:
  `#F5F5F7` 55 %); Mica shows while there is no artwork. Each frame is rendered on a thread-pool
  thread in float and upsampled with ±1 LSB triangular dither into a `WriteableBitmap` (at most
  ~0.4 MP, stretched), so it has no 8-bit colour bands. It flows with the display refresh (`CompositionTarget::Rendering`, at most ~60 fps) while music plays and
  the window is visible (player page only; off with `ui.animations=false`, low power or the Windows
  "Animation effects" setting) and otherwise renders only on artwork (900 ms crossfade), theme, DPI
  and size changes (debounced 120 ms).
* **Transport**: play/pause is optimistic (rolled back after 2.5 s without confirmation or on a
  failed `result`); seeks keep the target until the host reports it (no jump back from a state that
  was already in flight); keyboard seeks are coalesced (250 ms). Shortcuts in the player: Space
  play/pause, ←/→ ±5 s (when no slider / button / text box uses the key), `[` / `]` lyrics offset.
  Media keys are left to Windows (they reach the active media session system-wide).
* **Translations**: lines may carry `tr` (lyrics message `hasTranslation`). The main view shows the
  translation under each line (0.8× size, 0.6 opacity, 0.8 on the current line; static, the fill
  applies to the original only); it is part of the line element, so line spacing and the 38 % follow
  centring include it. The compact window shows it as the second lyric row (the artist line gives
  way while it is shown). A "Translation" toggle under the lyrics (only when translations exist) and
  Settings → Lyrics flip `lyrics.translation`, which the host also applies to desktop lyrics and the
  taskbar. `fake_host.py --no-translation` sends lyrics without translations.
* **Performance.** All timers and the frame clock run only while the window is visible, not
  minimised and playback is running. The lyric frame clock is a ~60 fps (main) / ~30 fps (compact)
  `DispatcherQueueTimer`, not `CompositionTarget::Rendering`: on the 300 Hz test display a bare
  `Rendering` subscription alone cost ~60 ms CPU/s. `ui.lowPower` switches to a ~10 fps timer and
  disables the fill and transitions; `ui.animations=false` gives instant jumps and a plain
  highlight. WinUI's `ProgressBar` was replaced by a scaled rectangle in the compact window (its
  value animation cost ~50 ms CPU/s). Measured on the development machine (Release x64): ~0 ms/s
  CPU when paused, minimised, idle or on a settings page; ~40–80 ms/s (main) and ~25 ms/s (compact)
  while synced lyrics animate. The indeterminate discovery spinner on *Devices & pairing* also
  redraws at the display refresh rate (~120 ms/s), so it only spins for the first 15 s of each
  discovery session; discovery itself continues and "Searching…" stays visible.
* **Window placement**: the main window restores `ui.main.{x, y, width, height}` (physical pixels)
  and `ui.main.maximized`; the compact window `ui.compact.{x, y}`. The mini player's controls show
  on hover or keyboard focus and hide 350 ms after the pointer has left (no flicker between child
  elements or at the border).
* **Settings** is a sub-page of the main window: the title bar shows a back button, the player and
  settings pages slide/fade (280 ms, off with `ui.animations=false`). The player's timers and frame
  clock stop while settings is shown; the settings page is created on first use. The compact
  window's settings/expand buttons navigate the main window.
* **Volume**: the volume button opens a flyout with both devices (`state.volume.local/remote`) and,
  when the focused (or active) Windows session reports `appVolume`, a row for that app. An inline
  slider below the progress bar controls the focused session's device (hidden in short windows; it
  is always in the flyout). Dragging sends `volume.set` at most ~10/s and the final value is always
  sent on release; host echoes are ignored briefly (phone: until its `result`, up to ~3 s) so the
  thumb does not jump back. A refused phone volume change rolls the slider back to the real level;
  `background_restricted` (Android 17+ blocks volume changes of background apps) shows a short
  inline hint under the inline slider and in the flyout until the next successful change. Per-app
  volume is not offered in the 340×96 compact window (no room).
* **Badges**: `session.source == "compat"` adds "Compatibility mode" to the status line (the tooltip
  explains the limits); `positionEstimated` prefixes the elapsed time with "≈". When
  `policy.gated` is set the arbitration line says so (arbitration only intervenes while one of the
  chosen audio outputs, `policy.onlyWithOutputs`, is in use).
* **Settings pages**: lyrics providers are an ordered list (enable + move up/down → `lyrics.providers`)
  with a privacy note; the taskbar page edits `taskbar.*` and shows the host's live
  `supported/shown/reason`; the compatibility page edits `compat.neteaseDesktop`; the arbitration
  page lists `state.audioOutputs` (current default marked) as a multi-select stored as
  `policy.onlyWithOutputs=[{id,name}]`.
* **Large windows** (Apple Music desktop / full-screen player): the composition is capped (1760 epx
  with lyrics, 1240 without) and centred, the card column (38 %, 392-720 epx) and the card /
  artwork (360-680 epx) grow with the window, card text steps up (22 / 26 / 30 epx titles), and the
  lyrics column gets a 740 epx reading width centred in its column, text left-aligned.
* **Layout**: nothing is clipped at the minimum window size or with very long names: the media
  card sits in a vertical ScrollViewer with an explicit width, short windows first drop the album
  line and the inline volume and shrink the artwork, single-line texts use `CharacterEllipsis`, and
  rows that hold trimmed text are Grids (a horizontal StackPanel never constrains its children).
* Pairing: result dialogs (connected / failed) are shown only after the UI saw that pairing in
  progress, and only once; opening settings while the host still reports an old result shows
  nothing. The SAS dialog has no default button, so a stray Enter can never confirm a code.
  `pairing.error` codes are localised; unknown codes show a generic message.
  `connection.lastError` (English diagnostic text) is shown only as secondary detail.

## Running with the fake host

`tools\fake_host.py` (Python 3, standard library + `ctypes`; Pillow optional for test artwork)
serves the documented pipe, sends `hello` + a realistic `state` + word-timed `lyrics` (all clearly
marked test data), prints every command it receives and reacts to it (play/pause/seek/next, focus,
settings deep-merge, a full pairing flow, discovery, device connect/forget, overlay, audio,
diagnostics every 2 s, `volume.set` including the per-app `key`, `host.exit`). The state includes
volume for both devices, `appVolume`, taskbar status, audio outputs and `policy.gated`.

```powershell
cd windows\ui
# custom pipe, so it never collides with a real host
python tools\fake_host.py --pipe MediaDuetTest --lang en-US --theme dark --log commands.jsonl
bin\x64\Release\MediaDuetUI.exe --show main --pipe MediaDuetTest
# or let the fake host start the UI
python tools\fake_host.py --pipe MediaDuetTest --launch compact
```

Options: `--volume-restricted` (phone volume changes fail with `background_restricted`),
`--lyrics-loading-ms N` (answer `loading` first, the lyrics N ms later),
`--scenario playing|idle|nopeer|firstrun|standalone|pairing|stale-pairing|compat` (`stale-pairing`: the host
still reports an old pairing result; `compat`: a NetEase desktop session in compatibility mode;
`firstrun`: no phone and `modeChosen=false`, the first-run choice; `standalone`: settings `mode`
"standalone"), `--phone-version 1.1.0` (the phone's app version: version warning),
`--no-lyrics`, `--long-names` (layout stress test), `--diagnostics`,
`--switch-lang-after 5 zh-CN` (runtime language switch: re-sends `hello` + `state`),
`--show-on-connect settings`. Interactive console commands: `pair`, `unpair`, `next`,
`lyrics on|off`, `bgvolume on|off`, `lang zh-CN`, `show compact`, `output 0|1` (switch the default audio output),
`disconnect`, `connect`, `quit`.

The fake host also reports `lyricsDelayMs` / `outputLatencyMs` per session (the test earbuds are
treated as Bluetooth), `audioOutputLatency`, and handles `lyrics.offset`.

Always use a test pipe (`--pipe`) when a real MediaDuet is installed and running, so the test UI
and fake host never talk to, or replace, the real ones.

Screenshot helper (captures only the given process's windows; `-Pick popup` = an open flyout):

```powershell
powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 -ProcessId <pid> -Pick largest -Out shot.png
```

## Tests

The timing, segmentation, display-row and background-dither logic lives in plain C++ headers
(`LyricTiming.h`, `LyricSegments.h`, `PlaybackClock.h`, `AmbientRender.h`) and is tested headless
(no XAML, no window) against the host's reference code in `windows/core`:

```powershell
# from the repository root, in a VS developer prompt
cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 /wd5285 /wd4100 /DNOMINMAX /I windows\ui /I windows\core\include ^
   /I windows\third_party windows\ui\tests\ui_logic_tests.cpp windows\core\src\lyrics\Karaoke.cpp ^
   windows\core\src\lyrics\LyricsClock.cpp windows\core\src\lyrics\Lrc.cpp windows\core\src\util\Text.cpp ^
   /Fe:ui_logic_tests.exe
.\ui_logic_tests.exe
```

## Render test (invisible)

`MediaDuetUI.exe --render-test <outdir> --pipe <private test pipe> [--render-only karaoke|sizes|light|transitions|hold|settings|compact]`
renders the real windows against `tools\fake_host.py` and saves PNGs (`RenderTargetBitmap`,
composited over a Mica-like colour) plus `render-test.log`. Nothing is ever shown on a monitor:

* windows are tool windows (`WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, not in Alt+Tab or the taskbar),
  placed entirely beyond the union of all monitors and shown with `SW_SHOWNOACTIVATE` - never
  `Activate()` / `SetForegroundWindow`; the test aborts at once if a window rectangle intersects a
  monitor or this process ever becomes the foreground window (the user switching between other
  apps meanwhile is logged and followed, not an error);
* it refuses the default pipe, skips the single-instance redirection, persisted settings, window
  placement and every settings write;
* the lyric clock is frozen and stepped (deterministic frames); composition transitions can run in
  slow motion to capture them mid-way.

```powershell
python tools\fake_host.py --pipe MediaDuetRenderTest --theme dark --lang en-US --lyrics-loading-ms 900
bin\x64\Release\MediaDuetUI.exe --render-test D:\temp\render --pipe MediaDuetRenderTest
```

Sizes are DIPs (4K at 100 / 150 / 200 % = 3840x2160 / 2560x1440 / 1920x1080 DIP); the window takes
the DPI of the monitor nearest to its off-screen position.
