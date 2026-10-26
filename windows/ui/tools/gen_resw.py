"""Generates Strings/en-US/Resources.resw and Strings/zh-CN/Resources.resw from the table below
and checks that every x:Uid used in XAML and every Str()/StrF() key used in C++ exists.

Run after changing UI text:  python tools/gen_resw.py
"""
import re
import sys
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parent.parent
TT = "[using:Microsoft.UI.Xaml.Controls]ToolTipService.ToolTip"
AN = "[using:Microsoft.UI.Xaml.Automation]AutomationProperties.Name"


def icon_button(uid, en, zh):
    return [(f"{uid}.{TT}", en, zh), (f"{uid}.{AN}", en, zh)]


S = []  # (key, en-US, zh-CN)

# ---- Main window -------------------------------------------------------------------------
S += icon_button("PrevButton", "Previous", "上一首")
S += [(f"PlayPauseButton.{AN}", "Play or pause", "播放/暂停")]
S += icon_button("NextButton", "Next", "下一首")
S += icon_button("SourceButton", "Choose which player to show and control", "选择要显示和控制的播放器")
S += icon_button("OverlayToggle", "Desktop lyrics", "桌面歌词")
S += [
    ("TranslationButton.Content", "Translation", "译"),
    (f"TranslationButton.{TT}", "Show or hide the lyric translation", "显示或隐藏歌词翻译"),
    (f"TranslationButton.{AN}", "Lyric translation", "歌词翻译"),
]
S += icon_button("CompactButton", "Mini player", "迷你播放器")
S += icon_button("SettingsButton", "Settings", "设置")
S += [
    ("EmptyTitle.Text", "Nothing is playing", "当前没有播放"),
    ("EmptyPairButton.Content", "Pair a phone", "配对手机"),
    ("StartHostButton.Content", "Start MediaDuet", "启动 MediaDuet"),
    ("StartHostButtonShort.Content", "Start", "启动"),
]
S += icon_button("ExpandButton", "Open the full player", "打开完整播放器")
S += icon_button("CloseButton", "Close", "关闭")

# ---- Settings window: navigation ------------------------------------------------------------
NAV = [
    ("General", "General", "常规"),
    ("Devices", "Devices & pairing", "设备与配对"),
    ("Connection", "Connection", "连接"),
    ("Arbitration", "Playback arbitration", "播放仲裁"),
    ("Lyrics", "Lyrics", "歌词"),
    ("Integration", "Windows media integration", "Windows 媒体集成"),
    ("Audio", "Audio", "音频"),
    ("Diagnostics", "Diagnostics", "诊断"),
    ("About", "About", "关于"),
]
for name, en, zh in NAV:
    S.append((f"Nav{name}.Content", en, zh))
    if name != "About":
        S.append((f"Nav{name}Title.Text", en, zh))

S += [
    # General
    ("HdrAppearance.Text", "Appearance", "外观"),
    ("SetLanguageTitle.Text", "Language", "语言"),
    ("SetLanguageDesc.Text", "MediaDuet's windows reopen to apply a new language.", "更改语言后，MediaDuet 的窗口会重新打开以应用。"),
    ("LangAuto.Content", "Windows display language", "跟随 Windows 显示语言"),
    ("SetThemeTitle.Text", "Theme", "主题"),
    ("ThemeSystem.Content", "Use system setting", "跟随系统"),
    ("ThemeLight.Content", "Light", "浅色"),
    ("ThemeDark.Content", "Dark", "深色"),
    ("SetAnimationsTitle.Text", "Animations", "动画效果"),
    ("SetAnimationsDesc.Text", "Subtle transitions for artwork and lyrics.", "为封面和歌词使用轻微的过渡动画。"),
    ("SetLowPowerTitle.Text", "Low power mode", "低功耗模式"),
    ("SetLowPowerDesc.Text", "Turns off animations and updates the progress less often.", "关闭动画并降低进度刷新频率。"),
    ("HdrWindows.Text", "Windows", "窗口"),
    ("SetMainOnTopTitle.Text", "Keep the main window on top", "主窗口始终置顶"),
    ("SetCompactOnTopTitle.Text", "Keep the mini player on top", "迷你播放器始终置顶"),
    ("SetCompactSnapTitle.Text", "Snap the mini player to screen edges", "迷你播放器吸附到屏幕边缘"),
    ("HdrStartup.Text", "Startup", "启动"),
    ("SetAutostartTitle.Text", "Start MediaDuet when I sign in", "登录 Windows 时启动 MediaDuet"),
    ("SetAutostartDesc.Text", "Runs in the notification area and keeps your phone in sync. This window only opens when you need it.",
     "在通知区域运行并与手机保持同步。此窗口只在需要时打开。"),
    ("SetQuitTitle.Text", "Quit MediaDuet", "退出 MediaDuet"),
    ("SetQuitDesc.Text", "Stops syncing with your phone until MediaDuet is started again.", "在再次启动 MediaDuet 之前停止与手机同步。"),
    ("QuitHostButton.Content", "Quit", "退出"),
    # Devices
    ("HdrPaired.Text", "Paired phones", "已配对的手机"),
    ("PairedEmptyText.Text", "No phone is paired yet. Pair one below.", "还没有配对的手机，可以在下方配对。"),
    ("HdrPairNew.Text", "Pair a new phone", "配对新手机"),
    ("PairHowTitle.Text", "Nearby phones", "附近的手机"),
    ("PairHowDesc.Text", "Open MediaDuet on your Android phone and tap \"Pair with PC\". Phones paired over Bluetooth or on the same network appear here.",
     "在 Android 手机上打开 MediaDuet 并点按“与电脑配对”。已通过蓝牙配对或位于同一网络的手机会显示在这里。"),
    ("QrPairTitle.Text", "Pair by scanning a QR code", "扫码配对"),
    ("QrHideButton.Content", "Hide", "收起"),
    ("QrShowButton", "Show QR code", "显示二维码"),
    ("QrShowAgain", "New code", "重新生成"),
    ("QrPairIdle", "Open MediaDuet on the phone, tap \"Scan to link a PC\" and scan the code. Nothing to compare.", "在手机上打开 MediaDuet，点“扫码连接电脑”扫描二维码，无需核对配对码。"),
    ("QrPairWaiting", "Scan this code with MediaDuet on the phone (valid for 5 minutes). The phone must be on the same network as this PC or paired with it over Bluetooth.", "用手机上的 MediaDuet 扫描这个二维码（5 分钟内有效）。手机需要和这台电脑在同一网络，或已通过蓝牙配对。"),
    ("QrPairSucceeded", "Paired with {0}.", "已与 {0} 配对。"),
    ("QrPairExpired", "The code expired. Show a new one to try again.", "二维码已失效，请重新生成。"),
    ("Searching.Text", "Searching…", "正在搜索…"),
    ("DiscoveredEmptyText.Text", "No phones found yet. Make sure Bluetooth is on and the phone is paired with this PC in Windows Settings, or that both devices are on the same network.",
     "尚未发现手机。请确认蓝牙已开启且手机已在 Windows 设置中与此电脑配对，或者两台设备位于同一网络。"),
    ("ManualTitle.Text", "Connect by IP address", "通过 IP 地址连接"),
    ("ManualDesc.Text", "If the phone does not show up, enter the address shown in the Android app.", "如果没有发现手机，请输入 Android 应用中显示的地址。"),
    ("ManualHost.PlaceholderText", "IP address, e.g. 192.168.1.20", "IP 地址，例如 192.168.1.20"),
    ("ManualPairButton.Content", "Pair", "配对"),
    ("ManualConnectButton.Content", "Connect", "连接"),
    # Connection
    ("HdrConnStatus.Text", "Status", "状态"),
    ("LblStatus.Text", "Status", "状态"),
    ("LblPeer.Text", "Phone", "手机"),
    ("LblTransport.Text", "Transport", "传输方式"),
    ("LblLatency.Text", "Round-trip time", "往返延迟"),
    ("LblConnectedFor.Text", "Connected for", "已连接时长"),
    ("LblTraffic.Text", "Traffic", "流量"),
    ("LblLastError.Text", "Last error", "最近的错误"),
    ("HdrConnMode.Text", "Transport", "传输方式"),
    ("SetConnModeTitle.Text", "Preferred connection", "首选连接方式"),
    ("SetConnModeDesc.Text", "Automatic uses Bluetooth and the local network together and switches between them without interrupting control.",
     "自动模式会同时使用蓝牙和局域网，并在两者之间无缝切换，不会中断控制。"),
    ("ConnModeAuto.Content", "Automatic (recommended)", "自动（推荐）"),
    ("ConnModeBt.Content", "Bluetooth only", "仅蓝牙"),
    ("ConnModeLan.Content", "Local network only", "仅局域网"),
    ("SetSwitchBackTitle.Text", "Switch-back delay (seconds)", "切回延迟（秒）"),
    ("SetSwitchBackDesc.Text", "How long the preferred transport must be stable before switching back to it.", "首选传输方式需要稳定多久才会切换回去。"),
    # Arbitration
    ("ArbIntro.Text", "When both the PC and the phone play audio, MediaDuet can pause one of them automatically and resume it later.",
     "当电脑和手机同时播放时，MediaDuet 可以自动暂停其中一方，并在之后恢复。"),
    ("HdrArbLive.Text", "Right now", "当前状态"),
    ("HdrArbMode.Text", "Mode", "模式"),
    ("ModeOffTitle.Text", "Off", "关闭"),
    ("ModeOffDesc.Text", "Both devices play independently.", "两台设备各自独立播放。"),
    ("ModeWinTitle.Text", "This PC first", "电脑优先"),
    ("ModeWinDesc.Text", "When the PC plays audio, the phone pauses; it resumes when the PC is quiet.", "电脑播放声音时暂停手机，电脑安静后再恢复。"),
    ("ModeAndroidTitle.Text", "Phone first", "手机优先"),
    ("ModeAndroidDesc.Text", "When the phone plays, media on the PC pauses.", "手机播放时暂停电脑上的媒体。"),
    ("ModeLatestTitle.Text", "Most recent wins", "后播放者优先"),
    ("ModeLatestDesc.Text", "Whatever started playing last keeps playing; the other device pauses.", "最后开始播放的一方继续播放，另一方暂停。"),
    ("ModeCustomTitle.Text", "Custom priorities", "自定义优先级"),
    ("ModeCustomDesc.Text", "Apps with a higher priority win. Set priorities below.", "优先级更高的应用胜出，可在下方设置。"),
    ("HdrArbBehaviour.Text", "Behaviour", "行为"),
    ("SetAutoResumeTitle.Text", "Resume automatically", "自动恢复播放"),
    ("SetAutoResumeDesc.Text", "Only media that MediaDuet paused is resumed — never something you paused yourself.",
     "只会恢复由 MediaDuet 暂停的媒体，绝不会恢复你自己暂停的内容。"),
    ("SetResumeDelayTitle.Text", "Resume delay (seconds)", "恢复延迟（秒）"),
    ("SetResumeDelayDesc.Text", "How long the other device must be quiet before resuming.", "另一台设备需要安静多久才会恢复播放。"),
    ("SetActivationTitle.Text", "Activation time (milliseconds)", "触发时间（毫秒）"),
    ("SetActivationDesc.Text", "Audio must play this long before it counts. Ignores short notification sounds.", "声音需要持续这么久才会被计入，从而忽略短促的提示音。"),
    ("SetSensitivityTitle.Text", "Audio detection threshold", "声音检测阈值"),
    ("SetSensitivityDesc.Text", "Peak level above which a Windows app counts as playing. Lower is more sensitive.",
     "Windows 应用的峰值音量超过此值才算正在播放。数值越低越灵敏。"),
    ("SetCallsTitle.Text", "Pause during calls", "通话时暂停"),
    ("SetCallsDesc.Text", "Pause media on both devices while the phone is in a call.", "手机通话期间暂停两台设备上的媒体。"),
    ("HdrArbApps.Text", "Windows apps", "Windows 应用"),
    ("BlacklistTitle.Text", "Ignored apps", "忽略的应用"),
    ("BlacklistDesc.Text", "Audio from these processes never pauses the phone (for example games or voice chat).",
     "这些进程发出的声音不会导致手机暂停（例如游戏或语音聊天）。"),
    ("ProcessInput.PlaceholderText", "Process name, e.g. chrome.exe", "进程名，例如 chrome.exe"),
    ("AddButton.Content", "Add", "添加"),
    ("WhitelistTitle.Text", "Only these apps", "仅限这些应用"),
    ("WhitelistDesc.Text", "If this list is not empty, only audio from these processes counts as Windows playback.",
     "如果此列表不为空，只有这些进程的声音才算作 Windows 正在播放。"),
    ("PrioritiesTitle.Text", "Custom priorities", "自定义优先级"),
    ("PrioritiesDesc.Text", "Used in \"Custom priorities\" mode. Enter a Windows process name or an Android package name; higher numbers win.",
     "用于“自定义优先级”模式。输入 Windows 进程名或 Android 包名，数值越大越优先。"),
    ("PriorityAppInput.PlaceholderText", "App id, e.g. spotify.exe or com.netease.cloudmusic", "应用标识，例如 spotify.exe 或 com.netease.cloudmusic"),
    # Lyrics
    ("HdrLyricsSources.Text", "Sources", "来源"),
    ("SetOnlineLyricsTitle.Text", "Find lyrics online", "在线查找歌词"),
    ("SetOnlineLyricsDesc.Text", "Searches the sources below for synced lyrics of the current track.", "从下方的来源搜索当前曲目的同步歌词。"),
    ("PrivacyNoteProviders.Title", "Privacy", "隐私"),
    ("PrivacyNoteProviders.Message", "Only the title, artist, album and duration of the current track are sent to the enabled sources (NetEase Cloud Music, QQ Music, LRCLIB). Nothing else leaves your PC, and no account is used.",
     "只会向已启用的来源（网易云音乐、QQ 音乐、LRCLIB）发送当前曲目的标题、艺术家、专辑和时长，不会发送任何其他信息，也不需要账户。"),
    ("SetLocalFolderTitle.Text", "Local LRC folder", "本地 LRC 文件夹"),
    ("SetTranslationTitle.Text", "Show lyric translation", "显示歌词翻译"),
    ("SetTranslationDesc.Text", "When the lyrics include a translation, show it under each line. Also applies to desktop lyrics and the taskbar.",
     "歌词带有翻译时，在每行原文下方显示。同样作用于桌面歌词和任务栏歌词。"),
    ("BrowseButton.Content", "Browse…", "浏览…"),
    ("ClearButton.Content", "Clear", "清除"),
    ("HdrOverlay.Text", "Desktop lyrics", "桌面歌词"),
    ("SetOverlayShowTitle.Text", "Show desktop lyrics", "显示桌面歌词"),
    ("SetOverlayShowDesc.Text", "A floating, transparent lyrics line above other windows.", "在其他窗口上方显示悬浮的透明歌词。"),
    ("SetOverlayLockTitle.Text", "Lock position", "锁定位置"),
    ("SetOverlayClickTitle.Text", "Click-through", "鼠标穿透"),
    ("SetOverlayClickDesc.Text", "Mouse clicks pass through the lyrics to the windows below.", "鼠标点击会穿过歌词，作用于下方的窗口。"),
    ("SetOverlayAutoHideTitle.Text", "Hide when nothing is playing", "没有播放时自动隐藏"),
    ("SetOverlayFontTitle.Text", "Font", "字体"),
    ("SetOverlayOpacityTitle.Text", "Opacity", "不透明度"),
    ("SetOverlayKaraokeTitle.Text", "Karaoke highlighting", "卡拉 OK 高亮"),
    ("SetOverlayKaraokeDesc.Text", "Fills the current line as it is sung (word by word when the lyrics have word timing).",
     "随演唱进度填充当前行（歌词带逐字时间时逐字高亮）。"),
    ("SetOverlayTwoLinesTitle.Text", "Show two lines", "双行显示"),
    ("SetOverlayTwoLinesDesc.Text", "Also shows the next line.", "同时显示下一行。"),
    ("SetOverlayColorsTitle.Text", "Colours", "颜色"),
    ("ColorActive.Text", "Sung", "已唱"),
    ("ColorInactive.Text", "Not yet sung", "未唱"),
    ("ApplyButton.Content", "Apply", "应用"),
    ("SetOverlayResetTitle.Text", "Position", "位置"),
    ("SetOverlayResetDesc.Text", "Drag the desktop lyrics to move them (when not locked).", "未锁定时可以直接拖动桌面歌词。"),
    ("OverlayResetButton.Content", "Reset position", "重置位置"),
    # Integration
    ("SetSmtcTitle.Text", "Show phone media in Windows", "在 Windows 中显示手机媒体"),
    ("SetSmtcDesc.Text", "Publishes what your phone is playing to the Windows media controls (the volume flyout, Quick Settings and the lock screen), so the keyboard media keys and Windows controls work for the phone too.",
     "将手机正在播放的内容发布到 Windows 媒体控件（音量浮窗、快速设置和锁屏），这样键盘媒体键和 Windows 控件也能控制手机。"),
    ("SmtcNote.Text", "MediaDuet never mirrors this bridged session back to the phone, so there is no echo.", "MediaDuet 不会把这个桥接会话再同步回手机，因此不会产生回环。"),
    # Audio
    ("HdrA2dp.Text", "Phone audio on this PC", "在电脑上播放手机音频"),
    ("SetA2dpTitle.Text", "Play phone audio through this PC (Bluetooth)", "通过此电脑播放手机音频（蓝牙）"),
    ("SetA2dpDesc.Text", "This PC acts as a Bluetooth speaker for your phone.", "让此电脑作为手机的蓝牙音箱。"),
    ("HdrLanSend.Text", "PC audio on the phone", "在手机上播放电脑音频"),
    ("SetLanSendTitle.Text", "Stream PC audio to the phone (experimental)", "将电脑音频串流到手机（实验性）"),
    ("LanSendWarning.Title", "Experimental", "实验性功能"),
    ("LanSendWarning.Message", "Uncompressed audio over your local network (about 1.5 Mbit/s) with noticeable delay. Not suitable for video. Works only on the local network, never over Bluetooth.",
     "通过局域网传输未压缩音频（约 1.5 Mbit/s），延迟明显，不适合视频。仅支持局域网，不会通过蓝牙传输。"),
    # Diagnostics
    ("SetDiagTitle.Text", "Collect live diagnostics", "收集实时诊断信息"),
    ("SetDiagDesc.Text", "Shows resource usage and link statistics, updated every 2 seconds. Nothing is sent anywhere.",
     "显示资源占用和连接统计，每 2 秒更新一次，不会发送到任何地方。"),
    ("DiagWaiting.Text", "Waiting for data…", "正在等待数据…"),
    # About
    ("AboutTagline.Text", "Two-way media control between Windows and Android.", "Windows 与 Android 之间的双向媒体控制。"),
    ("LblUiVersion.Text", "App version", "应用版本"),
    ("LblHostVersion.Text", "This PC (background service)", "此电脑（后台服务）"),
    ("LblLicense.Text", "License", "许可证"),
    ("HdrThirdParty.Text", "Third-party software", "第三方软件"),
    ("AboutLrclib.Text", "Free synced-lyrics database used for online lyrics (lrclib.net).", "用于在线歌词的免费同步歌词数据库（lrclib.net）。"),
    ("LblPhoneVersion.Text", "Phone app", "手机端"),
    # Work mode (General) and first run (main window)
    ("HdrWorkMode.Text", "Work mode", "使用方式"),
    ("SetWorkModeTitle.Text", "How MediaDuet is used", "MediaDuet 的使用方式"),
    ("SetWorkModeDesc.Text", "With a phone, MediaDuet links the two devices. \"This PC only\" turns every phone feature off (no Bluetooth or network connections); the player, lyrics, desktop lyrics and taskbar lyrics keep working.",
     "与手机配合时，MediaDuet 会连接两台设备。“仅此电脑”会关闭所有与手机相关的功能（不进行蓝牙或网络连接），播放器、歌词、桌面歌词和任务栏歌词照常可用。"),
    ("WorkModeLinked.Content", "Use with my phone", "与手机配合使用"),
    ("WorkModeStandalone.Content", "This PC only", "仅此电脑"),
    ("EmptyStandaloneButton.Content", "Use MediaDuet on this PC only", "仅在此电脑上使用"),
    ("WelcomeTitle.Text", "Welcome to MediaDuet", "欢迎使用 MediaDuet"),
    ("WelcomeSubtitle.Text", "How would you like to use it?", "你打算如何使用？"),
    ("WelcomeLinkedTitle.Text", "Use with my phone", "与手机配合使用"),
    ("WelcomeLinkedDesc.Text", "Control your phone's music from this PC and the PC's from your phone, with automatic pausing and shared lyrics. Pair your phone next.",
     "在电脑上控制手机的音乐，也可在手机上控制电脑，支持自动暂停和歌词共享。下一步将配对手机。"),
    ("WelcomeStandaloneTitle.Text", "Use on this PC only", "仅在此电脑上使用"),
    ("WelcomeStandaloneDesc.Text", "A player with lyrics, desktop lyrics and taskbar lyrics for what plays on this PC. No phone, no Bluetooth or network connections.",
     "为此电脑上播放的内容提供播放器、歌词、桌面歌词和任务栏歌词。无需手机，不进行蓝牙或网络连接。"),
    ("WelcomeFootnote.Text", "You can change this at any time in Settings > General.", "之后可随时在“设置 > 常规”中更改。"),
    # Online cover art (Windows media integration)
    ("HdrArtwork.Text", "Cover art", "封面"),
    ("SetArtworkOnlineTitle.Text", "Look up high-resolution covers online", "在线查找高清封面"),
    ("SetArtworkOnlineDesc.Text", "When a player only provides a small cover, MediaDuet searches NetEase Cloud Music, QQ Music or iTunes for the song (sends its title, artist and album). Covers found before are still used when this is off.",
     "当播放器只提供小尺寸封面时，MediaDuet 会在网易云音乐、QQ 音乐或 iTunes 中查找这首歌（会发送歌名、歌手和专辑）。关闭后，之前找到的封面仍会使用。"),
    ("HdrDeveloper.Text", "Developer", "开发者"),
    ("AboutDeveloperName.Text", "tianli", "tianli"),
    ("AboutDeveloperRole.Text", "Developer", "开发者"),
    ("AboutDeveloperEmail.Content", "wutianli@tianli0.top", "wutianli@tianli0.top"),
    (f"AboutDeveloperEmail.{TT}", "Send an email", "发送邮件"),
    ("ThirdHarmonyTitle.Text", "HarmonyOS Sans", "HarmonyOS Sans"),
    ("ThirdHarmonyDesc.Text", "© Huawei Device Co., Ltd. · HarmonyOS Sans Fonts License Agreement · the lyrics typeface, shipped unmodified",
     "© 华为终端有限公司（Huawei Device Co., Ltd.）· HarmonyOS Sans 字体许可协议 · 用于歌词显示，随附未经修改的字体文件"),
    ("ThirdHarmonyLicense.Content", "View the license agreement", "查看许可协议"),
]

# ---- Strings used from C++ (Str / StrF) ---------------------------------------------------
S += [
    ("TransportBluetooth", "Bluetooth", "蓝牙"),
    ("TransportLan", "LAN", "局域网"),
    ("YourPhone", "Phone", "手机"),
    ("ThisPC", "This PC", "此电脑"),
    ("ConnConnectingTo", "Connecting to {0}…", "正在连接 {0}…"),
    ("ConnDisconnectedFrom", "{0} · Disconnected", "{0} · 未连接"),
    ("ConnNoPeer", "No phone paired", "未配对手机"),
    ("HostConnecting", "Connecting to MediaDuet…", "正在连接 MediaDuet…"),
    ("HostNotRunningTitle", "MediaDuet host is not running", "MediaDuet 后台服务未运行"),
    ("HostNotRunningText", "The MediaDuet background service connects to your phone and controls media. Start it to continue.",
     "MediaDuet 后台服务负责连接手机并控制媒体。请启动它以继续。"),
    ("HostNotRunningShort", "Start it to view and change settings.", "启动后即可查看和更改设置。"),
    ("HostMissingText", "MediaDuet.exe was not found next to MediaDuetUI.exe. Please reinstall MediaDuet.",
     "在 MediaDuetUI.exe 所在的文件夹中找不到 MediaDuet.exe，请重新安装 MediaDuet。"),
    ("EmptyHintNoPeer", "Pair your Android phone to control its media here, or start playing something on this PC.",
     "配对 Android 手机即可在这里控制它的媒体，也可以在此电脑上开始播放。"),
    ("EmptyHint", "Start playing music on this PC or on {0}.", "在此电脑或 {0} 上开始播放音乐。"),
    ("NothingPlaying", "Nothing is playing", "当前没有播放"),
    ("UnknownTitle", "Unknown title", "未知标题"),
    ("SourceLabel", "{0} · {1}", "{0} · {1}"),
    ("SourceAutomatic", "Automatic (follow what is playing)", "自动（跟随正在播放的内容）"),
    ("SourceNone", "No media sessions", "没有媒体会话"),
    ("StatusPolicyPaused", "Paused automatically by MediaDuet", "已由 MediaDuet 自动暂停"),
    ("StatusRemotePaused", "Paused from the other device", "已从另一台设备暂停"),
    ("StatusBuffering", "Buffering…", "正在缓冲…"),
    ("Play", "Play", "播放"),
    ("Pause", "Pause", "暂停"),
    ("PolicyOff", "Playback arbitration is off", "播放仲裁已关闭"),
    ("PolicyPausedRemote", "Paused {0} because {1} is playing", "{1} 正在播放，已暂停 {0}"),
    ("PolicyPausedRemoteGeneric", "Paused {0} because this PC is playing audio", "此电脑正在播放声音，已暂停 {0}"),
    ("PolicyPausedLocal", "Paused {0} because {1} is playing", "{1} 正在播放，已暂停 {0}"),
    ("PolicyPausedLocalGeneric", "Paused media on this PC because {0} is playing", "{0} 正在播放，已暂停此电脑上的媒体"),
    ("PolicyResumedRemote", "Resumed playback on {0}", "已恢复 {0} 上的播放"),
    ("PolicyResumedLocal", "Resumed {0}", "已恢复 {0}"),
    ("PolicyResumedLocalGeneric", "Resumed media on this PC", "已恢复此电脑上的媒体"),
    ("PolicyModeActive", "Arbitration: {0}", "播放仲裁：{0}"),
    ("PolicyMode_off", "Off", "关闭"),
    ("PolicyMode_windows_first", "This PC first", "电脑优先"),
    ("PolicyMode_android_first", "Phone first", "手机优先"),
    ("PolicyMode_latest", "Most recent wins", "后播放者优先"),
    ("PolicyMode_custom", "Custom priorities", "自定义优先级"),
    ("PolicyState_playing", "Playing", "正在播放"),
    ("PolicyState_user_paused", "Paused by you", "已由你暂停"),
    ("PolicyState_policy_paused", "Paused by MediaDuet", "已由 MediaDuet 暂停"),
    ("PolicyState_remote_paused", "Paused from the other device", "已从另一台设备暂停"),
    ("PolicyState_stopped", "Stopped", "已停止"),
    ("PolicyLiveWinActive", "Windows audio: playing ({0})", "Windows 声音：正在播放（{0}）"),
    ("PolicyLiveWinActiveGeneric", "Windows audio: playing", "Windows 声音：正在播放"),
    ("PolicyLiveWinIdle", "Windows audio: quiet", "Windows 声音：安静"),
    ("PolicyLiveStates", "This PC: {0}   ·   {1}: {2}", "此电脑：{0}   ·   {1}：{2}"),
    ("LyricsSource", "Lyrics: {0}", "歌词来源：{0}"),
    ("LyricsSourceLocal", "local file", "本地文件"),
    ("LyricsSourceCache", "saved on this PC", "本机缓存"),
    ("LyricsSourcePhone", "phone", "手机"),
    ("LyricsLarger", "Larger text", "放大歌词"),
    ("LyricsSmaller", "Smaller text", "缩小歌词"),
    ("LyricsSizeReset", "Default size (now {0} %)", "默认大小（当前 {0}%）"),
    ("LyricsMenuTranslation", "Show translation", "显示翻译"),
    ("CommandFailed", "The command failed", "命令执行失败"),
    ("MenuOpenMain", "Open the full player", "打开完整播放器"),
    ("MenuSettings", "Settings", "设置"),
    ("MenuAlwaysOnTop", "Always on top", "始终置顶"),
    ("MenuClose", "Close", "关闭"),
    ("SettingsTitle", "Settings", "设置"),
    ("DurationHM", "{0} h {1} min", "{0} 小时 {1} 分钟"),
    ("DurationMS", "{0} min {1} s", "{0} 分 {1} 秒"),
    ("DurationS", "{0} s", "{0} 秒"),
    ("PairErr_rejected", "The pairing was rejected.", "配对已被拒绝。"),
    ("PairErr_timeout", "The pairing timed out. Please try again.", "配对超时，请重试。"),
    ("PairErr_mac_mismatch", "The codes did not match, so the connection may not be secure. Please try again.", "配对码不一致，连接可能不安全，请重试。"),
    ("PairErr_pairing_disabled", "The phone is not in pairing mode. Open \"Pair with PC\" in the Android app.", "手机未处于配对模式。请在 Android 应用中打开“与电脑配对”。"),
    ("PairErr_busy", "The phone is busy with another pairing. Try again in a moment.", "手机正在进行另一个配对，请稍后重试。"),
    ("PairErrUnknown", "Pairing failed.", "配对失败。"),
    ("ManualInvalid", "Enter an IP address or host name and a port between 1 and 65535.", "请输入 IP 地址或主机名，以及 1 到 65535 之间的端口。"),
    ("Pair", "Pair", "配对"),
    ("DeviceConnectedVia", "Connected via {0}", "已通过{0}连接"),
    ("DeviceNotConnected", "Not connected", "未连接"),
    ("DeviceLastSeen", "last seen {0}", "上次在线 {0}"),
    ("DeviceLan", "LAN {0}", "局域网 {0}"),
    ("Connect", "Connect", "连接"),
    ("Disconnect", "Disconnect", "断开连接"),
    ("Forget", "Forget", "忘记"),
    ("AlreadyPaired", "Paired", "已配对"),
    ("ConnState_no_peer", "No phone paired", "未配对手机"),
    ("ConnState_disconnected", "Disconnected", "未连接"),
    ("ConnState_connecting", "Connecting…", "正在连接…"),
    ("ConnState_connected", "Connected", "已连接"),
    ("TransportWithBackup", "{0} (with {1} as backup)", "{0}（{1} 备用）"),
    ("TrafficFormat", "↓ {0}   ↑ {1}", "↓ {0}   ↑ {1}"),
    ("ListEmpty", "None", "无"),
    ("Remove", "Remove", "移除"),
    ("NoFolderSelected", "No folder selected", "未选择文件夹"),
    ("PickFolderTitle", "Choose a folder with .lrc files", "选择包含 .lrc 文件的文件夹"),
    ("AudioStatus", "Status: {0}", "状态：{0}"),
    ("AudioState_off", "Off", "关闭"),
    ("AudioState_connecting", "Connecting…", "正在连接…"),
    ("AudioState_connected", "Connected", "已连接"),
    ("AudioState_on", "On", "已开启"),
    ("AudioState_streaming", "Streaming", "正在传输"),
    ("AudioState_error", "Error", "错误"),
    ("A2dpUnavailable", "Not available on this PC (requires Windows 10 version 2004 or later and a Bluetooth adapter).",
     "此电脑不支持（需要 Windows 10 2004 或更高版本以及蓝牙适配器）。"),
    ("LanSendUnavailable", "Not available: the phone must be connected over the local network.", "不可用：手机需要通过局域网连接。"),
    ("DiagCpu", "CPU (background service)", "CPU（后台服务）"),
    ("DiagWorkingSet", "Working set", "工作集内存"),
    ("DiagPrivate", "Private memory", "专用内存"),
    ("DiagThreads", "Threads", "线程数"),
    ("DiagTransport", "Transport", "传输方式"),
    ("DiagConnectedFor", "Connected for", "已连接时长"),
    ("DiagRtt", "Round-trip time", "往返延迟"),
    ("DiagLatency", "Last command latency", "最近命令延迟"),
    ("DiagBytes", "Data received / sent", "接收 / 发送数据"),
    ("DiagMessages", "Messages received / sent", "接收 / 发送消息"),
    ("DiagSessions", "Media sessions", "媒体会话"),
    ("DiagAudioSessions", "Active Windows audio sessions", "活动的 Windows 音频会话"),
    ("DiagUiMemory", "This window: working set · private", "本窗口：工作集 · 专用内存"),
    ("DiagSessionsFormat", "This PC {0} · Phone {1}", "此电脑 {0} · 手机 {1}"),
    ("NotConnected", "Not connected", "未连接"),
    ("PairTitleConnecting", "Pairing with {0}", "正在与 {0} 配对"),
    ("PairBodyConnecting", "Connecting… Keep the pairing screen open on your phone.", "正在连接…请保持手机上的配对界面打开。"),
    ("PairTitleConfirm", "Confirm the pairing code", "确认配对码"),
    ("PairBodyConfirm", "Make sure {0} shows the same 6-digit code, then confirm on both devices.", "请确认 {0} 显示相同的 6 位配对码，然后在两台设备上确认。"),
    ("PairCodesMatch", "Codes match", "配对码一致"),
    ("PairCodesDiffer", "Codes don't match", "配对码不一致"),
    ("PairBodyWaitingPeer", "Waiting for you to confirm on {0}…", "正在等待你在 {0} 上确认…"),
    ("PairTitleSucceeded", "Paired", "配对成功"),
    ("PairBodySucceeded", "{0} is now paired with this PC.", "{0} 已与此电脑配对。"),
    ("PairTitleFailed", "Pairing failed", "配对失败"),
    ("Cancel", "Cancel", "取消"),
    ("On", "On", "开"),
    ("Off", "Off", "关"),
    ("Done", "Done", "完成"),
    ("Close", "Close", "关闭"),
    ("ForgetTitle", "Forget {0}?", "忘记 {0}？"),
    ("ForgetBody", "The pairing key is deleted. The phone has to be paired again before it can connect.", "配对密钥将被删除，需要重新配对后手机才能再次连接。"),
]

# ---- Added for full localisation / accessibility --------------------------------------------
S += [
    (f"ProgressSlider.{AN}", "Playback position", "播放进度"),
    (f"LyricsScroller.{AN}", "Lyrics", "歌词"),
    (f"CompactLyricLine.{AN}", "Current lyric line", "当前歌词"),
    (f"CompactProgress.{AN}", "Playback progress", "播放进度"),
    (f"PolicyModeRadios.{AN}", "Arbitration mode", "仲裁模式"),
    ("LangZh.Content", "简体中文", "简体中文"),
    ("LangEn.Content", "English", "English"),
    ("AboutAppName.Text", "MediaDuet", "MediaDuet"),
    ("AboutLicenseValue.Text", "MIT License · © 2026 tianli", "MIT 许可证 · © 2026 tianli"),
    ("ThirdWasdkTitle.Text", "Windows App SDK / WinUI 3", "Windows App SDK / WinUI 3"),
    ("ThirdWasdkDesc.Text", "© Microsoft Corporation · MIT License · github.com/microsoft/WindowsAppSDK", "© Microsoft Corporation · MIT 许可证 · github.com/microsoft/WindowsAppSDK"),
    ("ThirdCppWinrtTitle.Text", "C++/WinRT", "C++/WinRT"),
    ("ThirdCppWinrtDesc.Text", "© Microsoft Corporation · MIT License · github.com/microsoft/cppwinrt", "© Microsoft Corporation · MIT 许可证 · github.com/microsoft/cppwinrt"),
    ("ThirdWilTitle.Text", "Windows Implementation Libraries (WIL)", "Windows 实现库（WIL）"),
    ("ThirdWilDesc.Text", "© Microsoft Corporation · MIT License · github.com/microsoft/wil", "© Microsoft Corporation · MIT 许可证 · github.com/microsoft/wil"),
    ("ThirdJsonTitle.Text", "JSON for Modern C++ (nlohmann/json)", "JSON for Modern C++（nlohmann/json）"),
    ("ThirdJsonDesc.Text", "© 2013–2025 Niels Lohmann · MIT License · github.com/nlohmann/json", "© 2013–2025 Niels Lohmann · MIT 许可证 · github.com/nlohmann/json"),
    ("ThirdLrclibTitle.Text", "LRCLIB", "LRCLIB"),
    ("LblBluetooth.Text", "Bluetooth", "蓝牙"),
    ("AppName", "MediaDuet", "MediaDuet"),
    ("EmptyHintStandalone", "Play music or a video on this PC and it shows up here.", "在此电脑上播放音乐或视频，就会显示在这里。"),
    ("WorkModeStandaloneShort", "This PC only", "仅此电脑"),
    ("NoPhonePaired", "No phone paired", "尚未配对手机"),
    ("VersionMismatchTitle", "Versions differ", "版本不一致"),
    ("VersionMismatchMessage", "The phone runs MediaDuet {0}, this PC {1}. Some features may not work; please update both to the same version.",
     "手机端版本 {0} 与电脑端 {1} 不一致，部分功能可能无法使用，请将两端更新到同一版本。"),
    ("DeviceVersion", "MediaDuet {0} {1}", "MediaDuet {0} {1}"),
    ("DeviceVersionUnknown", "MediaDuet version unknown", "MediaDuet 版本未知"),
    ("VersionPatchNote", "patch version differs", "补丁版本不同"),
    ("VersionMismatchShort", "differs from this PC", "与电脑端版本不一致"),
    ("PhoneVersionLastSeen", "{0} (last connected)", "{0}（上次连接时）"),
    ("CompactTitle", "MediaDuet mini player", "MediaDuet 迷你播放器"),
    ("Milliseconds", "{0} ms", "{0} 毫秒"),
    ("DeviceBt", "Bluetooth {0}", "蓝牙 {0}"),
    ("BtNoAdapter", "No Bluetooth adapter", "没有蓝牙适配器"),
    ("BtOn", "On", "已开启"),
    ("BtOff", "Off (turn Bluetooth on in Windows Settings)", "已关闭（请在 Windows 设置中打开蓝牙）"),
    ("PolicySuspended", "Playback arbitration is temporarily suspended", "播放仲裁已暂时暂停"),
    ("DiscoveredNotReady", "Not ready: open MediaDuet on this phone", "未就绪：请在这台手机上打开 MediaDuet"),
    ("PairErr_connect_failed", "Could not connect to the phone. Make sure it is nearby and MediaDuet is open on it.", "无法连接到手机。请确认手机在附近并已打开 MediaDuet。"),
    ("PairErr_unknown_device", "That phone could not be found any more. Refresh the list and try again.", "找不到这台手机了，请刷新列表后重试。"),
    ("PairErr_invalid_address", "The address is not valid. Check the IP address and port.", "地址无效，请检查 IP 地址和端口。"),
    ("PairErr_connection_closed", "The connection was closed during pairing. Please try again.", "配对过程中连接已断开，请重试。"),
    ("PairErr_protocol", "The phone runs an incompatible version of MediaDuet. Update both apps and try again.", "手机上的 MediaDuet 版本不兼容，请更新两端应用后重试。"),
    ("PairErr_bad_message", "The phone sent an unexpected message. Please try again.", "手机发送了无法识别的消息，请重试。"),
]


# ---- Round 2: settings as a page, volume, taskbar, providers, compatibility -----------------
S += icon_button("BackButton", "Back", "返回")
S += icon_button("MuteButton", "Mute or unmute", "静音或取消静音")
S += icon_button("VolumeButton", "Volume on both devices", "两台设备的音量")
S += [
    ("CompatTooltip", "Read through MediaDuet's compatibility support; the position is approximate.", "通过 MediaDuet 的兼容支持读取，播放进度为估计值。"),
    ("CompatBadge", "Compatibility mode", "兼容模式"),
    (f"InlineVolumeSlider.{AN}", "Volume", "音量"),
    (f"LocalVolumeSlider.{AN}", "Volume of this PC", "此电脑的音量"),
    (f"RemoteVolumeSlider.{AN}", "Phone volume", "手机音量"),
    ("VolumeFlyoutTitle.Text", "Volume", "音量"),
    ("VolumeThisPc.Text", "This PC", "此电脑"),
    ("VolumeUnavailable.Text", "Volume control is not available right now.", "当前无法调节音量。"),
    ("NavTaskbar.Content", "Taskbar", "任务栏"),
    ("NavTaskbarTitle.Text", "Taskbar", "任务栏"),
    ("NavCompat.Content", "Compatibility", "兼容性"),
    ("NavCompatTitle.Text", "Compatibility", "兼容性"),
    ("SetHeadsetAssistTitle.Text", "Multipoint headset assist", "多点连接耳机辅助"),
    ("SetHeadsetAssistDesc.Text", "If a Bluetooth headset connected to both devices pauses a player right after you start it (because the other device still holds the headset), MediaDuet pauses the other device and resumes the one you just started.",
     "如果同时连接两台设备的蓝牙耳机在你刚开始播放后立即暂停了播放器（因为另一台设备仍占用耳机），MediaDuet 会暂停另一台设备，并恢复你刚开始播放的那一台。"),
    ("ProvidersTitle.Text", "Online sources (in order)", "在线歌词来源（按顺序）"),
    ("ProvidersDesc.Text", "The first source that has lyrics for the track is used. Turn a source off and it is never contacted.", "使用第一个找到该曲目歌词的来源。关闭某个来源后将不会再访问它。"),
    ("TaskbarIntro.Text", "Shows lyrics or the current track directly in the Windows taskbar.", "在 Windows 任务栏中直接显示歌词或当前曲目。"),
    ("HdrTaskbarStatus.Text", "Status", "状态"),
    ("HdrTaskbarWidget.Text", "Taskbar widget", "任务栏组件"),
    ("SetTaskbarEnabledTitle.Text", "Show in the taskbar", "在任务栏中显示"),
    ("SetTaskbarEnabledDesc.Text", "Drawn by MediaDuet inside the taskbar; no extra window on the desktop.", "由 MediaDuet 绘制在任务栏内，不会在桌面上占用额外的窗口。"),
    ("SetTaskbarContentTitle.Text", "Content", "内容"),
    ("SetTaskbarContentDesc.Text", "Automatic shows lyrics when available, otherwise the title and artist.", "自动：有歌词时显示歌词，否则显示标题和艺术家。"),
    ("TaskbarContentAuto.Content", "Automatic", "自动"),
    ("TaskbarContentLyrics.Content", "Lyrics", "歌词"),
    ("TaskbarContentPlayer.Content", "Player (title and controls)", "播放器（标题和控制按钮）"),
    ("SetTaskbarPositionTitle.Text", "Position", "位置"),
    ("TaskbarPositionLeft.Content", "Left side of the taskbar", "任务栏左侧"),
    ("TaskbarPositionRight.Content", "Right, next to the notification area", "右侧，靠近通知区域"),
    ("SetTaskbarOffsetTitle.Text", "Offset", "偏移"),
    ("SetTaskbarOffsetDesc.Text", "Moves the widget along the taskbar.", "沿任务栏移动组件。"),
    ("SetTaskbarWidthTitle.Text", "Width", "宽度"),
    ("SetTaskbarFontSizeTitle.Text", "Font size", "字号"),
    ("CompatIntro.Text", "Support for players that do not publish their playback to Windows.", "支持不向 Windows 发布播放信息的播放器。"),
    ("SetNeteaseDesktopTitle.Text", "NetEase Cloud Music for Windows", "网易云音乐 Windows 版"),
    ("SetNeteaseDesktopDesc.Text", "Recognises NetEase Cloud Music although it does not publish a media session: the title comes from its window, the playback state from its audio, and controls are sent to its window.",
     "即使网易云音乐没有发布媒体会话也能识别它：标题来自其窗口，播放状态来自其音频，控制命令发送到其窗口。"),
    ("CompatLimits.Title", "Limitations", "限制"),
    ("CompatLimits.Message", "The playback position of such players is estimated, and artwork and seeking may be unavailable.", "此类播放器的播放进度为估计值，封面和跳转可能不可用。"),
    ("VolumeMuted", "Muted", "静音"),
    ("Pixels", "{0} px", "{0} 像素"),
    ("Provider_netease", "NetEase Cloud Music", "网易云音乐"),
    ("Provider_qqmusic", "QQ Music", "QQ 音乐"),
    ("Provider_lrclib", "LRCLIB", "LRCLIB"),
    ("MoveUp", "Move up", "上移"),
    ("MoveDown", "Move down", "下移"),
    ("TaskbarStatusUnsupported", "Not supported by this version of Windows or its taskbar", "当前 Windows 版本或任务栏不支持"),
    ("TaskbarStatusDisabled", "Turned off", "已关闭"),
    ("TaskbarStatusShown", "Shown in the taskbar", "已在任务栏中显示"),
    ("TaskbarStatusHidden", "Not shown right now", "当前未显示"),
]

S += [
    (f"AppVolumeSlider.{AN}", "App volume", "应用音量"),
    ("AppVolumeName", "{0} volume", "{0} 的音量"),
    ("OnlyWithOutputsTitle.Text", "Only intervene when this audio output is in use", "仅在使用以下音频输出时介入"),
    ("OnlyWithOutputsDesc.Text", "Automatic pausing, resuming and headset assist only happen while Windows plays through one of the selected outputs, for example your multipoint earbuds. Select nothing to always intervene.",
     "只有当 Windows 通过所选的某个输出设备（例如多点连接耳机）播放时，才会自动暂停、恢复或启用耳机辅助。不选择任何设备则始终介入。"),
    ("OutputsNone", "No audio outputs reported yet.", "尚未报告任何音频输出设备。"),
    ("OutputCurrent", "current output", "当前输出"),
    ("OutputNotConnected", "not connected", "未连接"),
    ("OutputUnknown", "another output", "其他输出设备"),
    ("PolicyGated", "Arbitration paused: the current output is {0}", "播放仲裁已暂停：当前输出为 {0}"),
]

# ---- Lyrics timing, follow button, volume restriction ----------------------------------------
S += [
    ("LyricsFollowButton.Content", "Back to current line", "回到当前歌词"),
    (f"LyricsFollowButton.{AN}", "Back to the current lyric line", "回到当前歌词行"),
]
S += icon_button("LyricsOffsetButton", "Lyrics timing", "歌词偏移")
S += [
    ("LyricsOffsetTitle.Text", "Lyrics timing", "歌词偏移"),
    ("LyricsEarlierButton.Content", "\u22120.1 s", "\u22120.1 秒"),
    (f"LyricsEarlierButton.{TT}", "Show the lyrics earlier (shortcut: [ )", "歌词提前显示 (快捷键 [ )"),
    (f"LyricsEarlierButton.{AN}", "Show the lyrics 0.1 seconds earlier", "歌词提前 0.1 秒"),
    ("LyricsLaterButton.Content", "+0.1 s", "+0.1 秒"),
    (f"LyricsLaterButton.{TT}", "Show the lyrics later (shortcut: ] )", "歌词延后显示 (快捷键 ] )"),
    (f"LyricsLaterButton.{AN}", "Show the lyrics 0.1 seconds later", "歌词延后 0.1 秒"),
    ("LyricsOffsetResetButton.Content", "Reset", "重置"),
    ("OffsetSeconds", "{0} s", "{0} 秒"),
    ("LyricsOffsetDetail", "For {0}. Positive values show the lyrics later. Shortcuts: [ and ].",
     "适用于 {0}。正值表示歌词延后显示。快捷键：英文方括号 [ 和 ]。"),
    ("LyricsOffsetLatency", "Audio output delay compensated automatically: {0} ms.", "已自动补偿音频输出延迟：{0} 毫秒。"),
    ("LyricsOffsetReset", "Reset", "重置"),
    ("LyricsOffsetResetFor", "Reset the lyrics offset of {0}", "重置 {0} 的歌词偏移"),
    ("HdrLyricsTiming.Text", "Timing", "时间同步"),
    ("SetLatencyAutoTitle.Text", "Compensate Bluetooth headphone delay automatically", "自动补偿蓝牙耳机延迟"),
    ("SetLatencyAutoDesc.Text", "Bluetooth audio reaches your ears a little later; the lyrics wait for it.",
     "蓝牙音频传到耳朵会稍有延迟，歌词会相应延后显示。"),
    ("SetLatencyBtTitle.Text", "Bluetooth headphone delay", "蓝牙耳机延迟"),
    ("SetLatencyBtDesc.Text", "Used while Windows plays through Bluetooth headphones.", "Windows 通过蓝牙耳机播放时使用。"),
    ("LatencyStatusBt", "Current output: Bluetooth, {0} ms compensated", "当前输出：蓝牙，已补偿 {0} 毫秒"),
    ("LatencyStatusBtOff", "Current output: Bluetooth (not compensated)", "当前输出：蓝牙（未补偿）"),
    ("LatencyStatusWired", "Current output: not Bluetooth, no delay added", "当前输出：非蓝牙，不增加延迟"),
    ("LyricsOffsetsTitle.Text", "Lyrics offset per app", "按应用的歌词偏移"),
    ("LyricsOffsetsDesc.Text", "Adjust while a song plays: the clock button under the lyrics, or [ and ] in the player.",
     "播放时调整：点按歌词下方的时钟按钮，或在播放器中按 [ 和 ]。"),
    ("SetOverlayTopmostTitle.Text", "Always on top", "始终置顶"),
    ("SetOverlayHideFullscreenTitle.Text", "Hide when a full-screen app is in front", "全屏应用在前台时隐藏"),
    ("SetOverlayHideFullscreenDesc.Text", "For example games and full-screen videos.", "例如游戏和全屏视频。"),
    ("SetOverlayLayoutTitle.Text", "Two-line layout", "双行布局"),
    ("SetOverlayLayoutDesc.Text", "Alternating shows each next line on the other side, like karaoke screens.",
     "左右交错：下一行显示在另一侧，类似 KTV 字幕。"),
    ("OverlayLayoutAlternate.Content", "Alternating left and right", "左右交错"),
    ("OverlayLayoutCenter.Content", "Centred", "居中"),
    ("VolumeBackgroundRestricted",
     "The phone blocks volume changes while MediaDuet is in the background. Tap \u201cAllow volume control\u201d in the phone's notification, or open MediaDuet once.",
     "手机系统限制了后台调节音量：请在手机通知中点按\u201c允许电脑调节音量\u201d，或打开一次 MediaDuet"),
]

# taskbar.reason codes (docs/ipc.md), shown after the taskbar status.
S += [
    ("TaskbarReason_disabled", "turned off", "已关闭"),
    ("TaskbarReason_nothing_playing", "nothing is playing", "没有正在播放的内容"),
    ("TaskbarReason_fullscreen", "a full-screen app is in front", "全屏应用位于前台"),
    ("TaskbarReason_autohide", "the taskbar hides automatically", "任务栏处于自动隐藏状态"),
    ("TaskbarReason_vertical_taskbar", "the taskbar is vertical", "任务栏为纵向"),
    ("TaskbarReason_no_space", "not enough space on the taskbar", "任务栏空间不足"),
    ("TaskbarReason_taskbar_not_ready", "waiting for the taskbar (tries again automatically)", "正在等待任务栏（会自动重试）"),
    ("TaskbarReason_no_taskbar", "no taskbar found yet (tries again automatically)", "暂未找到任务栏（会自动重试）"),
    ("TaskbarReason_unavailable", "temporarily unavailable (tries again automatically)", "暂时不可用（会自动重试）"),
]

# Keys built at runtime from a prefix + protocol value (checked by prefix).
DYNAMIC_PREFIXES = ["ConnState_", "PolicyMode_", "PolicyState_", "AudioState_", "PairErr_", "Provider_", "TaskbarReason_"]


def write_resw(path: Path, lang_index: int):
    lines = [
        '<?xml version="1.0" encoding="utf-8"?>',
        "<!-- Generated by tools/gen_resw.py - edit the table there, then re-run the script. -->",
        "<root>",
        '  <resheader name="resmimetype"><value>text/microsoft-resx</value></resheader>',
        '  <resheader name="version"><value>2.0</value></resheader>',
        '  <resheader name="reader"><value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>',
        '  <resheader name="writer"><value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>',
    ]
    for entry in S:
        key, value = entry[0], entry[1 + lang_index]
        lines.append(f'  <data name="{escape(key, {chr(34): "&quot;"})}" xml:space="preserve"><value>{escape(value)}</value></data>')
    lines.append("</root>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check() -> int:
    keys = [k for k, _, _ in S]
    errors = 0
    dupes = {k for k in keys if keys.count(k) > 1}
    for d in sorted(dupes):
        print("duplicate key:", d)
        errors += 1
    uid_prefixes = {k.split(".", 1)[0] for k in keys if "." in k}
    plain = {k for k in keys if "." not in k}

    used_uids = set()
    for xaml in ROOT.glob("*.xaml"):
        used_uids |= set(re.findall(r'x:Uid="([^"]+)"', xaml.read_text(encoding="utf-8")))
    for uid in sorted(used_uids - uid_prefixes):
        print("x:Uid without strings:", uid)
        errors += 1
    for uid in sorted(uid_prefixes - used_uids):
        print("strings for unused x:Uid:", uid)
        errors += 1

    used_code = set()
    for cpp in ROOT.glob("*.cpp"):
        text = cpp.read_text(encoding="utf-8")
        used_code |= set(re.findall(r'StrF?\(L"([A-Za-z0-9_]+)"', text))
        # Keys kept in tables, e.g. { L"DiagCpu", "cpuPercent" }
        used_code |= {k for k in re.findall(r'L"([A-Za-z0-9_]+)"', text) if k in plain}
    used_code -= set(DYNAMIC_PREFIXES)
    for key in sorted(used_code - plain):
        print("code string missing:", key)
        errors += 1
    for key in sorted(plain - used_code):
        if not any(key.startswith(p) for p in DYNAMIC_PREFIXES):
            print("unused code string:", key)
            errors += 1
    return errors


if __name__ == "__main__":
    write_resw(ROOT / "Strings" / "en-US" / "Resources.resw", 0)
    write_resw(ROOT / "Strings" / "zh-CN" / "Resources.resw", 1)
    n = check()
    print(f"{len(S)} strings written; {n} problem(s)")
    sys.exit(1 if n else 0)
