<p align="center"><img src="windows/app/resources/app_512.png" width="96" alt="MediaDuet"></p>

<h1 align="center">MediaDuet</h1>
<p align="center">Lightweight cross-device media control for Windows and Android<br>
Windows 与 Android 之间的轻量双向媒体控制与智能播放协调</p>

**[官网 / Website](https://tianli0.github.io/MediaDuet/)** · **[下载 / Download](https://github.com/TIANLI0/MediaDuet/releases/latest)**

[中文](#中文) · [English](#english) · [Protocol](protocol/specification.md) · [Architecture](docs/architecture.md) · [Testing & status](docs/testing.md) · [Compatibility](docs/compatibility.md)

---

## 中文

### 它能做什么

- **双向控制**：在电脑上看到并控制手机正在播放的音乐（网易云、QQ 音乐、Spotify…），也能在手机上控制电脑上的播放器。
- **局域网优先，蓝牙兜底**：同一 Wi-Fi 下自动发现并作为首选控制链路（延迟更低）；蓝牙 RFCOMM 作为热备份。断线无缝切换，局域网稳定后再切回，同一指令绝不执行两次。
- **智能播放仲裁**：耳机同时连着两台设备时，电脑开始放视频/游戏声音会自动暂停手机（也可设为手机优先、最新优先或按应用自定义优先级）。区分“你暂停的”和“程序暂停的”，不会擅自恢复你暂停的音乐，也不会双向打架。通话时总是优先。
- **原生集成**：手机正在播放的歌曲出现在 Windows 系统媒体浮窗/键盘媒体键里；Windows 媒体出现在手机 App 中。
- **接力播放**：一键把正在听的歌换到另一台设备上，用网易云音乐、QQ 音乐或 Spotify 从当前进度接着播放，原设备随后暂停（电脑端在播放卡片的「在手机上继续 / 在电脑上继续」按钮里，手机端在「播放来源」面板里）。歌曲按歌名/歌手/时长在目标 App 的曲库里精确定位，可以跨 App（例如手机上的 QQ 音乐接到电脑上的网易云）。手机在后台时，Android 不允许打开其他 App，此时会弹出一条通知，轻触即可继续。
- **精美界面**：Windows 端 WinUI 3 媒体卡片、悬停才显示控制的迷你播放器、可置顶、可点击穿透的桌面歌词（描边字、双行交替/居中、长句横向滚动、渐进式逐字/逐行填充）；Android 端 Kyant0 液态玻璃、无底部导航栏的全高“正在播放”主页（设置从右上角进入）、类 Apple Music 的播放器与歌词一体界面。
- **歌词**：本地 LRC、手机推送、网易云（含逐字 yrc）、QQ 音乐、LRCLIB 依次查找（只发送歌名/歌手/专辑/时长，可调整顺序或关闭）；30 首测试曲目命中 29 首。不会伪造歌词。歌词时间轴经过平滑，并自动补偿蓝牙耳机延迟，每个播放器可单独微调偏移（±0.1 秒）。
- **手机自己找歌词**：手机不连电脑时也会用同样的规则在网易云、QQ 音乐、LRCLIB 中查找并缓存歌词；同一首歌的元数据反复变化（如 QQ 音乐）时歌词不会闪烁。
- **任务栏歌词**：在 Windows 11（居中或靠左）和 Windows 10 任务栏的空白处显示渐进式歌词或播放控件（悬停显示按钮）；换行时像列表一样向上滚动，过长的歌词和译文会跟着演唱进度横向滚动。
- **高清封面**：播放器提供的封面太小时，自动查找同一首歌的高清封面（可关闭）。
- **单设备也能用**：首次启动选择「搭配使用」或「仅此手机 / 仅此电脑」；配对后两端互相显示版本号，版本不一致时提醒。
- **音量**：双向调节对方设备音量；Windows 端可单独调节每个应用的音量。
- **多点耳机辅助**：耳机因另一台设备占用而自动暂停你刚播放的音乐时，自动接管；可设置仅在指定音频输出（如耳机）时才进行播放干预。
- **网易云 Windows 兼容**：网易云未公开系统媒体会话时，通过窗口标题、本地播放队列和音频活动识别，并按歌曲 ID 精确获取歌词。
- **音频（可选）**：Windows 作为手机的蓝牙音频接收端（A2DP Sink）；实验性的“电脑声音推到手机”（局域网 PCM）。
- **检查更新与软件内更新**：两端每天在 GitHub 上检查一次新版本（可关闭，也可手动检查）。在「设置 › 关于」里一键下载，用发布附带的 SHA256SUMS.txt 校验后安装：电脑上安装版静默升级、便携版自动替换文件，完成后都会自动重启；手机上由系统确认安装。
- **安全**：首次配对需在两端核对 6 位数字；每次连接 ECDHE + 配对密钥双向认证 + AES-256-GCM；密钥用 DPAPI / Android Keystore 保存；不需要任何云账号。

### 安装

**Windows 10 2004+ / Windows 11**

1. 下载并解压 `MediaDuet-<版本>-win-x64.zip`（或运行安装包），运行 `MediaDuet.exe`。它会常驻托盘，界面按需打开；关闭窗口不影响后台同步。
2. 如需蓝牙：先在 *Windows 设置 → 蓝牙* 中与手机完成系统配对。

**Android 10+（目标 API 37 / Android 17）**

1. 安装 APK，打开 App。
2. *设置 → 权限与后台*：授予“通知使用权”（只用于读取媒体会话，**不会读取或上传通知内容**）、蓝牙、通知、局域网访问（Android 17）；建议关闭电池优化并按厂商指引允许后台运行。
3. *设置 → 连接 → 配对模式* 打开（5 分钟有效）。
4. 在电脑 *设置 → 设备 → 配对新手机* 中选择手机（蓝牙）或输入 IP（局域网，默认端口 47321），两端核对 6 位数字后确认。

### 构建

| 组件 | 命令 |
|------|------|
| Windows 主程序 + 测试 | `cmake -S windows -B windows/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build windows/build && windows\build\md_tests.exe`（需 VS 2022/2026 C++、Windows SDK 10.0.26100） |
| Windows 界面 (WinUI 3) | `msbuild windows\ui\MediaDuetUI.vcxproj -restore -p:Configuration=Release -p:Platform=x64` |
| Windows 打包 | `powershell -ExecutionPolicy Bypass -File windows\packaging\package.ps1 -Arch x64` → `dist\MediaDuet-<版本>-win-x64.zip` 与 `dist\MediaDuet-<版本>-x64-setup.exe`（需 NSIS 3；也可用 `installer.iss` + Inno Setup） |
| Android | `cd android && ./gradlew :app:assembleDebug :app:assembleRelease :app:testDebugUnitTest :app:lintDebug`（JDK 21，Android SDK 37）。发布签名：在 `android/keystore.properties` 写入 `storeFile/storePassword/keyAlias/keyPassword`（已被 git 忽略），否则 release 包不签名 |

### 当前状态

真实完成度见 [docs/testing.md](docs/testing.md) 与 [docs/compatibility.md](docs/compatibility.md)。简要：

- ✅ 单元测试：Windows 146/146，Android 157/157，WinUI 逻辑 16/16；协议向量由独立 Python 实现交叉验证。
- ✅ 真机外的端到端：Windows 主程序 ↔ Android App（API 36/37 模拟器，局域网）配对、会话、双向控制、封面、SMTC 桥接、断线恢复、撤销、与真实游戏音频的仲裁。
- 🔧 需要真实硬件验证：电脑与实体手机之间的蓝牙 RFCOMM、蓝牙↔局域网切换、A2DP 接收、网易云/QQ 音乐真机兼容、各厂商 ROM 的后台存活。

### 隐私

不使用云服务，不上传通知、联系人或聊天内容。可选的外部请求只有三类：歌词和高清封面查询（网易云音乐、QQ 音乐、LRCLIB、iTunes，只发送歌名、歌手、专辑和时长）、接力播放时的曲库查找（同样只发送歌曲信息），以及每天一次的更新检查（GitHub，只请求最新版本号）。这些都可以在设置中关闭。

---

## English

### What it does

- **Two-way control**: see and control your phone's music (NetEase Cloud Music, QQ Music, Spotify, …) from Windows, and Windows players from the phone.
- **LAN first, Bluetooth fallback**: on the same Wi-Fi the phone is discovered automatically and used as the control link (lower latency); Bluetooth RFCOMM stays connected as a hot standby. Seamless failover, stable switch-back, and at-most-once command execution.
- **Playback arbitration**: with one headset on both devices, PC video/game audio pauses the phone (or phone-first, latest-wins, custom per-app priorities). User pauses are never undone, policy pauses are attributed so the devices never fight, calls always win.
- **Native integration**: phone media appears in the Windows media flyout and media keys; Windows media appears on the phone.
- **Handoff**: continue the song you are listening to on the other device — in NetEase Cloud Music, QQ Music or Spotify, from the same position — and this side pauses ("Continue on your phone / on this PC" on the PC's player card, the source sheet on the phone). The song is found in the target app's catalogue by title / artist / duration, also across apps (QQ Music on the phone → NetEase on the PC). While the phone app is in the background, Android does not let it open another app; a notification then continues with one tap.
- **Polished UI**: WinUI 3 media card, hover-only mini player, always-on-top, click-through desktop lyrics (outlined text, two-line alternate or centred layout, scrolling long lines, progressive word/line fill) on Windows; Kyant0 liquid glass, a full-height Now Playing home without a tab bar (settings from the top-right) and an Apple-Music-style integrated player/lyrics screen on Android.
- **Lyrics**: local LRC, phone-provided, NetEase (word-timed yrc), QQ Music, LRCLIB in a configurable order (title/artist/album/duration only; can be disabled); 30/30 hit rate on a mixed test set. **Translations** (NetEase / QQ Music) shown under each line on every surface, with a toggle. Never invented. Lyric timing is smoothed, compensates Bluetooth output latency automatically and has a per-player offset (±0.1 s steps).
- **Lyrics on the phone alone**: without a PC the phone looks lyrics up itself (same providers and matching rules, cached); lyrics no longer flicker when a player (QQ Music) keeps re-reporting the same song.
- **Taskbar lyrics** on the Windows 11 (centred or left-aligned) and Windows 10 taskbar, with hover controls; lines scroll up like a list, long lines and their translations scroll sideways with the singing.
- **HD covers**: when a player's own cover is small, the song's high-resolution cover is looked up (optional).
- **One device is enough**: choose "with my phone/PC" or "this device only" on first launch; once paired, each side shows the other's version and warns on a mismatch.
- **Volume**: control the other device's volume; per-app volume on Windows.
- **Multipoint headset assist** and optional *only intervene when this output is in use*.
- **NetEase Cloud Music for Windows** compatibility when it publishes no media session.
- **Audio (optional)**: Windows as the phone's Bluetooth A2DP sink; experimental PC→phone LAN PCM streaming.
- **Updates**: both apps look for a new version on GitHub once a day (can be turned off, or checked by hand) and install it from Settings › About after verifying it against the release's SHA256SUMS.txt — the PC updates its installed or portable copy and restarts, the phone hands the APK to Android to confirm.
- **Security**: 6-digit comparison on first pairing; per-connection ECDHE with mutual pairing-key authentication and AES-256-GCM; keys in DPAPI / Android Keystore; no accounts, no cloud.

### Install

**Windows 10 2004+ / 11**: unzip the release (or run the setup) and start `MediaDuet.exe` (tray app; the UI opens on demand). For Bluetooth, bond the phone in *Windows Settings → Bluetooth* first.

**Android 10+ (targets API 37)**: install the APK, then *Settings → Permissions & background*: grant notification access (used only to read media sessions — notification contents are never read or uploaded), Bluetooth, notifications and local network (Android 17); allow background activity. Open *Settings → Connection → Pairing mode*, then pair from *Windows Settings → Devices → Pair new phone* (Bluetooth device, or IP on port 47321) and confirm the matching 6-digit code on both screens.

### Build

See the table in the Chinese section; commands are identical. CI: `.github/workflows/windows.yml` (host, tests, vector reproducibility, WinUI) and `android.yml` (build, unit tests, lint).

### Status

Verified results, measurements (idle host: 0.13 % of one core, 7.5 MB private) and the list of hardware-only checks are in [docs/testing.md](docs/testing.md) and [docs/compatibility.md](docs/compatibility.md).

### Repository layout

```
protocol/   specification, JSON test vectors, reference generator, Python peer client
windows/    core/ (protocol, crypto, link, policy, lyrics, settings) · platform/ (Bluetooth, LAN,
            GSMTC/SMTC, Core Audio, A2DP, loopback, lyrics providers, D2D overlay) · app/ (tray host)
            · ui/ (WinUI 3) · tests/ · tools/ · packaging/
android/    app/ (Kotlin, Compose, liquid glass) · testplayer/ (test fixture)
docs/       architecture, ipc, bluetooth, media sessions, audio routing, testing, compatibility
```

### License

MIT © 2026 tianli — see [LICENSE](LICENSE). Bundled third-party components keep their own licences, see
[third-party notices](docs/THIRD_PARTY_NOTICES.md); in particular the HarmonyOS Sans fonts in
`windows/ui/Assets/Fonts` are © Huawei Device Co., Ltd. under the HarmonyOS Sans Fonts License
Agreement (shipped unmodified) and are not covered by the MIT licence.
