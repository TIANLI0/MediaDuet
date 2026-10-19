<p align="center"><img src="windows/app/resources/app_512.png" width="96" alt="MediaDuet 图标"></p>

<h1 align="center">MediaDuet</h1>
<p align="center"><strong>用电脑控制手机音乐，用手机控制电脑播放。</strong><br>
电脑出声时自动暂停手机音乐，在电脑和手机上同步看歌词。</p>

<p align="center">
  <a href="https://github.com/TIANLI0/MediaDuet/releases/latest"><strong>下载 Windows / Android 版</strong></a> ·
  <a href="https://tianli0.github.io/MediaDuet/">官网</a> ·
  <a href="#english">English</a>
</p>

## 中文

MediaDuet 连接你的 **Windows 电脑和 Android 手机**，让你在任意一端查看、控制另一端正在播放的内容。音乐和视频由原来的播放器负责播放，MediaDuet 帮你切歌、调音量、看歌词，以及协调两台设备的播放。

## 你可以用它做什么

- **手机听歌，电脑切歌。** 手机放在一旁，在电脑上查看歌名、封面和进度，暂停、切歌或调节音量；也可以反过来，用手机控制电脑上的音乐和视频。
- **电脑有声音时，让手机音乐先停一停。** 例如耳机同时连接电脑和手机：电脑开始播放视频或游戏声音时，自动暂停手机音乐；电脑安静后再恢复。你手动暂停的音乐会保持暂停。
- **把歌词放到方便看的地方。** 在电脑播放器、桌面悬浮窗、任务栏或手机上看同步歌词，支持逐字高亮和译文，也可以手动调整歌词时间。
- **换台设备，接着听。** 把当前歌曲交给另一台设备上的网易云音乐、QQ 音乐或 Spotify，尝试从当前进度继续播放。效果取决于目标播放器和曲库，详见下方说明。
- **只用一台设备也有用。** 选择“仅此电脑”或“仅此手机”，就能控制本机播放、查看歌词；电脑端还能使用桌面歌词和任务栏歌词。

此外，还支持 Windows 键盘媒体键控制手机播放、迷你播放器、电脑应用单独调音量，以及在软件内检查和安装更新。

## 界面预览

<table>
  <tr>
    <th>Windows：播放控制与同步歌词</th>
    <th>Android：查看和控制播放</th>
  </tr>
  <tr>
    <td><img src="website/assets/win-player-zh.webp" width="680" alt="Windows 播放器：歌曲封面、播放控制、音量和同步歌词"></td>
    <td><img src="website/assets/android-player.webp" width="210" alt="Android 播放器：歌曲封面、进度、切歌和音量控制"></td>
  </tr>
</table>

## 下载与开始使用

### 1. 安装电脑端和手机端

打开 [下载页面](https://github.com/TIANLI0/MediaDuet/releases/latest)，两端尽量安装同一版本。

| 设备 | 系统要求 | 下载哪个文件 |
| --- | --- | --- |
| Windows 电脑 | Windows 10 2004 或更高版本 / Windows 11，x64 | 安装版 `MediaDuet-<版本>-x64-setup.exe`，或免安装版 `MediaDuet-<版本>-win-x64.zip` |
| Android 手机 | Android 10 或更高版本 | `MediaDuet-<版本>-android.apk` |

电脑端运行安装程序，或将免安装版**完整解压**后运行 `MediaDuet.exe`。手机端安装 APK 并打开。

首次启动时，选择搭配另一台设备使用。如果只需要本机播放器和歌词，选择“仅此电脑 / 仅此手机”即可，无需进行下面的配对。

### 2. 在手机上开启权限

进入手机 MediaDuet 的 **设置 → 权限与后台**，按页面提示开启：

- **通知使用权**：用于获取、控制音乐和视频的播放状态。MediaDuet 不读取、保存或发送通知正文。
- **附近的设备 / 蓝牙、通知、局域网访问**：按系统版本和连接方式授权。
- **后台运行**：建议解除电池优化限制，并按照页面中的厂商指引允许后台活动，避免锁屏后断开。

如果系统不允许开启“通知使用权”，可先到系统的 **应用信息 → 右上角菜单 → 允许受限设置**，再返回授权。具体名称可能因手机而异。

### 3. 连接并配对

可以通过**同一局域网或蓝牙**连接。第一次使用时，可以先让电脑和手机连接同一个 Wi-Fi；电脑通过网线连接同一路由器也可以。

1. 手机 MediaDuet：进入 **设置 → 连接 → 配对模式**，开启配对（5 分钟内有效）。
2. 电脑 MediaDuet：进入 **设置 → 设备与配对 → 配对新手机**，选择发现的手机。
3. 检查两端显示的 **6 位配对码**是否一致，并在两端确认。
4. 在手机或电脑上播放一首歌，另一端应显示歌曲信息；试试暂停或切歌。

没有发现手机时，可在电脑端选择“通过 IP 地址连接”，填写手机 App 中显示的地址（默认端口 `47321`）。

要通过蓝牙连接，请先在 **Windows 系统设置 → 蓝牙和设备**中完成电脑与手机的系统配对，再进行上面的 MediaDuet 配对。自动连接模式支持在局域网和蓝牙之间切换。

## 几个常用设置

### 自动暂停和恢复

在电脑端 **设置 → 播放仲裁**中选择行为。这里的“播放仲裁”就是决定两台设备同时播放时，哪一端继续播放。

| 模式 | 会发生什么 |
| --- | --- |
| 电脑优先（默认） | 电脑播放声音时暂停手机音乐，电脑安静后按设置恢复 |
| 手机优先 | 手机播放时，暂停电脑上可控制的媒体 |
| 后播放者优先 | 最近开始播放的一端继续，另一端暂停 |
| 自定义优先级 | 按你设置的应用优先级决定 |
| 关闭 | 两台设备各自播放，不自动干预 |

你可以关闭自动恢复、调整恢复前的等待时间，或设置成仅在使用指定耳机等音频输出设备时介入。**自动恢复只针对 MediaDuet 自动暂停的内容。**

### 在另一台设备上继续听

电脑端点击播放卡片中的 **“在手机上继续 / 在电脑上继续”**；手机端打开 **“播放来源”**面板，选择接力到另一端和目标播放器。

目标设备需要安装对应播放器，并能播放这首歌。MediaDuet 会查找歌曲、尝试从当前进度播放，成功后暂停原设备。部分播放器或跨曲库匹配只能打开歌曲或搜索结果，需要手动开始播放；Spotify 等目标仍有待真机验证。手机端在后台时，可能需要点一下通知才能继续。

### 让手机声音从电脑播放

如果你希望手机的声音从电脑音箱或电脑连接的耳机传出，可以开启 **“接收手机音频”**。这需要先完成系统蓝牙配对；必要时还需在手机上选择电脑作为声音输出。

另有实验性的“电脑声音传到手机”功能，需要局域网连接，存在明显延迟，不适合边看视频边听。音频传输的兼容情况见 [音频说明](docs/audio-routing.md)。

## 常见问题

**控制了手机音乐，声音会自动转到电脑吗？**

默认仍从手机当前的耳机或扬声器播放。远程控制、接力播放和音频传输是三个独立功能；需要转移声音时，再开启上面的音频功能。

**哪些播放器可以用？**

主要通过播放器提供的系统媒体控制接口工作，控制按钮和进度信息取决于播放器开放的能力。项目包含网易云音乐、QQ 音乐、Spotify 等适配，但不同版本、设备的支持程度有差异。游戏声音可以触发自动暂停手机音乐，游戏本身通常不能被远程暂停。已验证范围见 [兼容性列表](docs/compatibility.md) 和 [测试记录](docs/testing.md)。

**关掉电脑窗口后，还能用吗？**

可以。Windows 端关闭窗口后会留在系统托盘，继续同步和响应控制；要完全退出，请使用托盘菜单。手机端需要允许后台运行。

**连接成功，但看不到手机歌曲怎么办？**

先确认手机上的播放器正在播放，再到“权限与后台”检查通知使用权。锁屏后容易掉线时，检查电池优化和厂商后台限制。

**需要账号或互联网吗？**

MediaDuet 无需注册账号。设备之间的控制和同步通过局域网或蓝牙进行；在线查歌词、封面、跨曲库接力查歌和检查更新需要互联网。原播放器的登录、会员和歌曲访问要求仍然适用。

## 隐私

设备之间直接连接，并对通信加密，不经过云端转发。MediaDuet 不上传通知正文、联系人或聊天内容。

会访问外网的功能包括：从网易云音乐、QQ 音乐、LRCLIB、iTunes 查询歌词或高清封面，接力时查找歌曲，以及通过 GitHub 检查和下载更新。歌曲查询使用歌名、歌手、专辑、时长等歌曲信息。你可以在设置中关闭在线歌词源、高清封面查询和自动更新检查，并自行决定是否使用接力播放。

## 开发与项目文档

普通用户直接下载发布版即可。构建命令放在下面，协议、架构和详细验证记录见对应文档。

<details>
<summary>从源码构建与测试</summary>

### Windows

需要 Visual Studio 2022 / 2026 的 C++ 和 WinUI 开发工具、Windows SDK、CMake 3.24+、Ninja。生成安装包还需 NSIS 3。

在仓库根目录运行以下命令，可构建主程序和 WinUI 界面、运行核心测试，并生成 `dist/` 下的免安装压缩包；已安装 NSIS 时还会生成安装包：

```powershell
powershell -ExecutionPolicy Bypass -File windows\packaging\package.ps1 -Arch x64
```

仅构建主程序并运行核心测试（在 Visual Studio Developer PowerShell 中）：

```powershell
cmake -S windows -B windows/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build windows/build
ctest --test-dir windows/build --output-on-failure
```

单独构建界面：

```powershell
msbuild windows\ui\MediaDuetUI.vcxproj -restore -p:Configuration=Release -p:Platform=x64
```

更多界面构建、调试与测试方法见 [Windows 界面文档](windows/ui/README.md)。

### Android

需要 JDK 21、Android SDK 37。在 Windows PowerShell 中运行：

```powershell
cd android
.\gradlew.bat :app:assembleDebug :app:testDebugUnitTest :app:lintDebug
```

macOS / Linux 使用 `./gradlew`。Debug APK 位于 `android/app/build/outputs/apk/debug/`。发布构建和签名配置见 [Android 文档](android/README.md)；未配置签名时，Release APK 不会签名。

</details>

- [架构与目录说明](docs/architecture.md)
- [通信协议](protocol/specification.md) · [蓝牙与局域网连接](docs/bluetooth.md)
- [兼容性列表](docs/compatibility.md) · [测试记录与待验证项目](docs/testing.md)
- [Windows 界面开发](windows/ui/README.md) · [Android 开发](android/README.md)

## English

**Control your phone's music from Windows, or control your PC's playback from Android.** MediaDuet also coordinates playback between the two devices and displays synced lyrics. Your existing music and video apps handle playback.

- **Remote controls:** view track information, pause, skip, seek and adjust volume on the other device, where supported by the player.
- **Automatic pause and resume:** PC video or game audio can pause phone music and let it resume when the PC goes quiet. Music you paused yourself stays paused. Choose PC-first, phone-first, latest-started, custom priorities, or turn this off.
- **Lyrics:** synced lyrics and translations in both apps, plus desktop and taskbar lyrics on Windows.
- **Handoff:** try to continue a song on the other device using NetEase Cloud Music, QQ Music or Spotify. Results depend on the target app and catalogue; some targets only open the song or search results. See [compatibility](docs/compatibility.md).
- **One device is enough:** choose “This PC only” or “This phone only” for local controls and lyrics.

### Get started

1. [Download the Windows installer or portable ZIP and the Android APK](https://github.com/TIANLI0/MediaDuet/releases/latest). Requires Windows 10 2004+ / Windows 11 (x64) and Android 10+. Use matching app versions.
2. On Android, open **Settings → Permissions & background**. Grant notification access for media controls, the permissions needed for your connection, and allow background activity. Notification contents are never read, stored or sent.
3. Put both devices on the same LAN. On the phone, enable **Settings → Connection → Pairing mode**. In the PC app, open **Settings → Devices & pairing → Pair a new phone**, select the phone, compare the six-digit code and confirm on both devices. If discovery fails, enter the address shown in the phone app (default port `47321`).
4. Play something in your usual player and try controlling it from the other device. For Bluetooth, pair the devices in Windows system settings first, then pair in MediaDuet.

Closing the PC window leaves MediaDuet running in the tray. Remote control leaves audio on its original output by default. Optional phone → PC audio uses Bluetooth; experimental PC → phone audio uses the LAN and has noticeable latency. See [audio routing](docs/audio-routing.md).

No MediaDuet account or cloud relay is required. Online lyrics, artwork and handoff searches contact external providers using song information; update checks and downloads use GitHub. Online lyrics providers, HD artwork and automatic update checks can be disabled in settings.

For build instructions and verification details, see the [Android](android/README.md), [Windows UI](windows/ui/README.md), [architecture](docs/architecture.md) and [testing](docs/testing.md) docs.

## 开源许可 / License

[MIT](LICENSE) © 2026 tianli。第三方组件使用各自的许可，详见 [第三方许可说明](docs/THIRD_PARTY_NOTICES.md)。随附的 HarmonyOS Sans 字体遵循其字体许可，不属于 MIT 许可范围。
