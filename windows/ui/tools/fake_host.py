"""Fake MediaDuet host for developing and testing MediaDuetUI.exe without the real host.

Serves the documented UI pipe (docs/ipc.md) with ctypes only (no pywin32 needed), sends `hello`
and a realistic `state` + `lyrics`, prints every command the UI sends, and reacts to them
(play/pause, seek, focus, settings, pairing flow, discovery, devices, overlay, audio, diagnostics).

EVERYTHING IN HERE IS TEST DATA. Track titles, lyrics, device names and artwork are invented.

Usage (from windows/ui):
    python tools/fake_host.py                       # default pipe for the current user
    python tools/fake_host.py --pipe MediaDuetTest  # custom pipe; start the UI with --pipe MediaDuetTest
    python tools/fake_host.py --lang en-US --theme dark --launch main
    python tools/fake_host.py --log commands.jsonl  # also append received commands to a file

Options: --scenario playing|idle|nopeer|pairing|stale-pairing|compat   --no-lyrics   --no-translation   --diagnostics   --long-names
         --volume-restricted (phone volume changes fail with "background_restricted", like Android 17 in the background)
Console (stdin) commands while running: pair, unpair, next, lyrics on|off, show main|compact|settings|pair,
bgvolume on|off (phone volume background restriction),
    lang auto|zh-CN|en-US, output 0|1 (default audio output), disconnect, connect, quit
    --switch-lang-after 5 zh-CN   changes the language after 5 s (hello + state), to test runtime switching
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

PIPE_ACCESS_DUPLEX = 0x00000003
FILE_FLAG_OVERLAPPED = 0x40000000
FILE_FLAG_FIRST_PIPE_INSTANCE = 0x00080000
PIPE_TYPE_BYTE = 0x0
PIPE_READMODE_BYTE = 0x0
PIPE_WAIT = 0x0
PIPE_REJECT_REMOTE_CLIENTS = 0x8
PIPE_UNLIMITED_INSTANCES = 255
ERROR_IO_PENDING = 997
ERROR_PIPE_CONNECTED = 535
INVALID_HANDLE_VALUE = wt.HANDLE(-1).value


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p),
                ("Offset", wt.DWORD), ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


kernel32.CreateNamedPipeW.restype = wt.HANDLE
kernel32.CreateNamedPipeW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_void_p]
kernel32.CreateEventW.restype = wt.HANDLE
kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
kernel32.ConnectNamedPipe.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED)]
kernel32.ReadFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
kernel32.WriteFile.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)]
kernel32.GetOverlappedResult.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.BOOL]
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.DisconnectNamedPipe.argtypes = [wt.HANDLE]
kernel32.CancelIoEx.argtypes = [wt.HANDLE, ctypes.POINTER(OVERLAPPED)]
kernel32.ResetEvent.argtypes = [wt.HANDLE]
kernel32.LocalFree.argtypes = [ctypes.c_void_p]
kernel32.GetTickCount64.restype = ctypes.c_ulonglong
kernel32.GetUserDefaultUILanguage.restype = wt.WORD
kernel32.GetCurrentProcess.restype = wt.HANDLE
advapi32.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
advapi32.GetTokenInformation.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
advapi32.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.LPWSTR)]


def tick() -> int:
    return kernel32.GetTickCount64()


def current_user_sid() -> str:
    token = wt.HANDLE()
    if not advapi32.OpenProcessToken(kernel32.GetCurrentProcess(), 0x0008, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    size = wt.DWORD()
    advapi32.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))  # TokenUser
    buf = ctypes.create_string_buffer(size.value)
    if not advapi32.GetTokenInformation(token, 1, buf, size, ctypes.byref(size)):
        raise ctypes.WinError(ctypes.get_last_error())
    sid_ptr = ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0]
    out = wt.LPWSTR()
    if not advapi32.ConvertSidToStringSidW(sid_ptr, ctypes.byref(out)):
        raise ctypes.WinError(ctypes.get_last_error())
    sid = out.value
    kernel32.LocalFree(ctypes.cast(out, ctypes.c_void_p))
    kernel32.CloseHandle(token)
    return sid


# --------------------------------------------------------------------------------------------
# Test data
# --------------------------------------------------------------------------------------------

def make_showcase_artwork(folder: Path):
    """An invented, text-free cover (dusk sky, two overlapping suns) for showcase screenshots."""
    try:
        from PIL import Image, ImageDraw, ImageFilter
    except ImportError:
        return None
    folder.mkdir(parents=True, exist_ok=True)
    size = 512
    img = Image.new("RGB", (size, size))
    px = img.load()
    top, bottom = (58, 44, 120), (236, 140, 120)
    for y in range(size):
        t = y / size
        row = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(size):
            px[x, y] = row
    glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    d.ellipse((96, 170, 296, 370), fill=(255, 214, 170, 200))
    d.ellipse((216, 150, 416, 350), fill=(255, 240, 220, 170))
    glow = glow.filter(ImageFilter.GaussianBlur(2))
    img.paste(glow, (0, 0), glow)
    hills = ImageDraw.Draw(img)
    hills.polygon([(0, 400), (140, 340), (300, 390), (420, 350), (512, 380), (512, 512), (0, 512)], fill=(44, 34, 84))
    path = folder / "art_showcase.png"
    img.save(path)
    return str(path)


def make_artwork(folder: Path):
    """Creates two invented album covers (PNG) if Pillow is available; returns paths or Nones."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        return None, None
    folder.mkdir(parents=True, exist_ok=True)
    out = []
    for name, c1, c2, label in [("art_a.png", (255, 120, 80), (120, 40, 200), "TEST A"),
                                ("art_b.png", (30, 170, 160), (20, 50, 120), "TEST B")]:
        size = 512
        img = Image.new("RGB", (size, size))
        px = img.load()
        for y in range(size):
            for x in range(size):
                t = (x * 0.4 + y * 0.6) / size
                px[x, y] = tuple(int(c1[i] + (c2[i] - c1[i]) * t) for i in range(3))
        d = ImageDraw.Draw(img)
        for r in range(40, 260, 36):
            d.ellipse((size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r), outline=(255, 255, 255), width=3)
        try:
            font = ImageFont.truetype("segoeuib.ttf", 44)
        except OSError:
            font = ImageFont.load_default()
        d.text((32, size - 80), label, fill=(255, 255, 255), font=font)
        path = folder / name
        img.save(path)
        out.append(str(path))
    return out[0], out[1]


def track_id(title, artist, album, duration_ms):
    secs = "" if not duration_ms else str((duration_ms + 500) // 1000)
    raw = "\x1f".join([title, artist, album, secs]).encode("utf-8")
    return hashlib.sha256(raw).hexdigest()[:16]


def version_match(pc, phone):
    """md::version::compare: equal | patch | mismatch | unknown."""
    def parse(v):
        m = re.match(r"\s*[vV]?(\d{1,6})\.(\d{1,6})(?:\.(\d{1,6}))?", v or "")
        return (int(m.group(1)), int(m.group(2)), int(m.group(3) or 0)) if m else None
    a, b = parse(pc), parse(phone)
    if not a or not b:
        return "unknown"
    if a[:2] != b[:2]:
        return "mismatch"
    return "patch" if a[2] != b[2] else "equal"


DEFAULT_SETTINGS = {
    "language": "auto", "theme": "system", "autostart": True, "mode": "linked", "modeChosen": True,
    "artwork": {"online": True},
    "connection": {"mode": "auto", "switchBackDelaySec": 10},
    "policy": {"mode": "windows_first", "autoResume": True, "resumeDelaySec": 5, "activationMs": 1500,
               "sensitivity": 0.02, "pauseForCalls": True, "headsetAssist": True, "onlyWithOutputs": [],
               "blacklist": ["discord.exe"], "whitelist": [],
               "priorities": {"spotify.exe": 20, "com.example.testplayer": 10}},
    "smtcBridge": True,
    "compat": {"neteaseDesktop": True},
    "taskbar": {"enabled": True, "content": "auto", "position": "left", "offset": 0, "width": 360, "fontSize": 13},
    "lyrics": {"online": True, "translation": True, "providers": ["netease", "qqmusic", "lrclib"], "localFolder": "",
               "latency": {"auto": True, "bluetoothMs": 180},
               "offsets": {},
               "overlay": {"visible": False, "locked": False, "clickThrough": False, "autoHide": True,
                           "fontFamily": "Microsoft YaHei UI", "fontSize": 32, "opacity": 0.92,
                           "karaoke": True, "twoLines": True, "colorActive": "#FFFFFFFF",
                           "colorInactive": "#B3FFFFFF", "x": None, "y": None, "width": 900,
                           "topmost": True, "hideInFullscreen": False, "layout": "alternate"}},
    "ui": {"alwaysOnTop": False, "animations": True, "lowPower": False,
           "main": {"x": None, "y": None, "width": None, "height": None, "maximized": False},
           "compact": {"x": None, "y": None, "alwaysOnTop": True, "snap": True}},
    "diagnostics": False,
}

# Invented test lyrics (word-timed for the first track) and invented translations ("tr"). Two lines
# deliberately have no translation, and one translation is long enough to wrap in the main window.
TEST_TRANSLATIONS = {
    8000: "[测试数据] 模拟主机歌词第一行",
    12500: "晨光洒过窗台",
    17000: "两台设备唱着同一首老歌，这一行的译文故意写得很长，用来检查换行后的行距和自动滚动居中",
    21500: "Test lyric: this line checks Chinese text rendering",
    26000: "每个字都在唱到时亮起",
    35000: "手机暂停，电脑继续播放",
    39500: "Test lyric: word highlighting should be smooth",
    44000: "桥梁、管道与 JSON 行",
    48500: "副歌之前还有一行",
    53000: "（副歌）二重唱，两块屏幕一支曲",
    57500: "（副歌）左右合拍，分毫不差",
    70500: "最后一行测试歌词——感谢测试",
}

TEST_LYRICS_WORDS = [
    (8000, "[Test data] Fake host lyrics line one"),
    (12500, "Morning light across the window"),
    (17000, "Two devices keep the same old song"),
    (21500, "测试歌词：这一行用于检查中文显示"),
    (26000, "Every word lights up when it is sung"),
    (30500, "Seek here to check the scrolling"),
    (35000, "Pause the phone, the PC plays on"),
    (39500, "测试歌词：逐字高亮应当平滑"),
    (44000, "Bridges, pipes and JSON lines"),
    (48500, "One more line before the chorus"),
    (53000, "(chorus) Duet, duet, two screens one tune"),
    (57500, "(chorus) Left and right, in perfect time"),
    (62000, ""),
    (66000, "Instrumental gap above is shown as a note"),
    (70500, "Last test line — thanks for testing"),
]


# --showcase: an invented song with original lyrics, for website / README screenshots.
SHOWCASE_LYRICS = [
    (8000, "窗外的晚风还没有停", "The evening wind is still outside"),
    (12500, "你把这首歌放进口袋", "You put this song into your pocket"),
    (17000, "走进另一个房间", "Walked into another room"),
    (21500, "旋律也跟着你进来", "And the melody followed you in"),
    (26000, "两块屏幕亮起同一句", "Two screens light up the same line"),
    (30500, "不早一秒，也不晚一秒", "Not a second early, not a second late"),
    (35000, "手机放下，电脑接着唱", "Put the phone down, the PC sings on"),
    (39500, "这首歌一直没有断", "This song never stopped"),
    (44000, "晚风写给你的信", "A letter the evening wind wrote you"),
    (48500, "每个字都被点亮", "Every word is lit"),
    (53000, "我们唱同一首歌", "We sing the same song"),
    (57500, "在两个地方，同一个节拍", "In two places, on the same beat"),
    (62000, "", ""),
    (66000, "直到最后一句", "Until the very last line"),
]


def lyric_rows(showcase):
    if showcase:
        return [(t, text) for t, text, _ in SHOWCASE_LYRICS], {t: tr for t, _, tr in SHOWCASE_LYRICS if tr}
    return TEST_LYRICS_WORDS, TEST_TRANSLATIONS


def build_lyrics(session, words=True, translation=True, showcase=False):
    lines = []
    rows, translations = lyric_rows(showcase)
    for i, (t, text) in enumerate(rows):
        nxt = rows[i + 1][0] if i + 1 < len(rows) else t + 4500
        line = {"t": t, "d": nxt - t, "text": text}
        if translation and t in translations:
            line["tr"] = translations[t]
        if words and text:
            parts = text.split(" ") if " " in text else list(text)
            step = max(1, int((nxt - t) * 0.8) // max(1, len(parts)))
            line["words"] = [{"t": t + j * step, "d": step, "text": p + (" " if " " in text and j < len(parts) - 1 else "")}
                             for j, p in enumerate(parts)]
        lines.append(line)
    return {"type": "lyrics", "trackId": session["trackId"], "side": session["_side"], "key": session["key"],
            "status": "found", "source": "netease", "synced": True,
            "hasTranslation": any("tr" in l for l in lines), "lines": lines}


# --------------------------------------------------------------------------------------------
# Fake host
# --------------------------------------------------------------------------------------------

class FakeHost:
    def __init__(self, args):
        self.args = args
        self.lock = threading.RLock()
        self.pipe = None
        self.connected = False
        self.log_file = open(args.log, "a", encoding="utf-8") if args.log else None
        art_dir = Path(tempfile.gettempdir()) / "MediaDuetFakeHost"
        self.art_a, self.art_b = make_artwork(art_dir)
        self.lyrics_enabled = not args.no_lyrics
        self.volume_restricted = args.volume_restricted
        self.diag_enabled = args.diagnostics
        self.settings = json.loads(json.dumps(DEFAULT_SETTINGS))
        if args.lang:
            self.settings["language"] = args.lang
        if args.theme:
            self.settings["theme"] = args.theme
        self.settings["diagnostics"] = self.diag_enabled
        self.overlay_visible = False
        self.discovering = False
        self.bytes_in = 18234
        self.bytes_out = 9120
        self.since = tick() - 754_000
        self.peer = {"id": "a1b2c3d4e5f60718293a4b5c6d7e8f90", "name": "Test Phone (fake host)", "platform": "android"}
        self.devices = [{"id": self.peer["id"], "name": self.peer["name"], "platform": "android",
                         "btAddress": "00:11:22:33:44:55", "lanHost": "192.168.1.20:47321",
                         "connected": True, "transport": "bt", "lastSeen": int(time.time() * 1000)}]
        self.connection_status = "connected"
        # App versions (DeviceInfo.app): this PC and the phone (--phone-version, e.g. 1.1.0 -> mismatch warning).
        self.self_app = "1.5.0"
        for d in self.devices:
            d["app"] = args.phone_version
            d["versionMatch"] = version_match(self.self_app, args.phone_version)
        self.peer["app"] = args.phone_version
        self.peer["versionMatch"] = version_match(self.self_app, args.phone_version)
        self.pairing = {"state": "idle", "sas": None, "peerName": None, "error": None}
        self.a2dp = {"available": True, "state": "off", "device": self.peer["name"], "error": None}
        self.lansend = {"available": True, "state": "off", "error": None}
        self.focus = {"side": "remote", "key": "com.example.testplayer#1", "auto": True}
        self.volume = {"local": {"available": True, "level": 0.42, "muted": False},
                       "remote": {"available": True, "level": 0.6, "muted": False, "steps": 15}}
        self.taskbar_state = {"supported": True, "shown": True, "reason": None}
        self.outputs = [{"id": "{0.0.0.00000000}.{11111111-test}", "name": "Headphones (Test Earbuds)", "default": True},
                        {"id": "{0.0.0.00000000}.{22222222-test}", "name": "Speakers (Test Audio)", "default": False}]
        # Which outputs are Bluetooth (the real host asks Windows); output 0 = the test earbuds.
        self.bluetooth_outputs = {"{0.0.0.00000000}.{11111111-test}"}
        now = tick()
        remote = {"_side": "remote", "key": "com.example.testplayer#1",
                  "app": {"id": "com.example.testplayer", "name": "测试播放器 Test Player"},
                  "title": "测试曲目 · Test Track One", "artist": "Fake Host Ensemble", "album": "Test Album (fake data)",
                  "durationMs": 245000, "positionMs": 9000, "atTick": now, "rate": 1.0, "status": "playing",
                  "actions": ["play", "pause", "next", "previous", "seek"], "artworkPath": self.art_a,
                  "kind": "music", "policyState": "playing", "source": "remote",
                  # The phone app's own buttons and a play queue (media.queue, sent after hello).
                  "customActions": [{"id": "like", "title": "喜欢 Like", "iconPath": None},
                                    {"id": "mode", "title": "随机 Shuffle", "iconPath": None}],
                  "queue": {"count": 24, "rev": "fake1", "activeId": 3}}
        local1 = {"_side": "local", "key": "Spotify.exe", "app": {"id": "Spotify.exe", "name": "Spotify"},
                  "title": "Second Test Song", "artist": "Example Artist", "album": "Example Album",
                  "durationMs": 198000, "positionMs": 61000, "atTick": now, "rate": 1.0, "status": "paused",
                  "actions": ["play", "pause", "next", "previous", "seek"], "artworkPath": self.art_b,
                  "kind": "music", "policyState": "policy_paused", "source": "smtc",
                  "appVolume": {"available": True, "level": 0.8, "muted": False}}
        local2 = {"_side": "local", "key": "chrome.exe", "app": {"id": "chrome.exe", "name": "Google Chrome"},
                  "title": "A test video", "artist": "Test channel", "album": "",
                  "durationMs": 600000, "positionMs": 120000, "atTick": now, "rate": 1.0, "status": "paused",
                  "actions": ["play", "pause", "seek"], "artworkPath": None, "kind": "video", "policyState": "user_paused",
                  "source": "smtc", "appVolume": {"available": True, "level": 1.0, "muted": False}}
        for s in (remote, local1, local2):
            s["trackId"] = track_id(s["title"], s["artist"], s["album"], s["durationMs"])
        self.remote = [remote]
        self.local = [local1, local2]
        self.local_gen = 3
        self.remote_gen = 9
        self.last_action = {"what": "pause_local", "reason": "remote_active", "app": "Spotify.exe", "atTick": now - 30000}
        if args.showcase:
            remote["app"] = {"id": "com.netease.cloudmusic", "name": "网易云音乐"}
            remote["key"] = "com.netease.cloudmusic#1"
            remote.update(title="晚风来信", artist="林岸", album="夜车", durationMs=212000)
            remote["artworkPath"] = make_showcase_artwork(Path(os.environ.get("TEMP", ".")) / "mediaduet-fakehost") or self.art_a
            local1.update(title="慢慢走", artist="林岸", album="夜车")
            local2.update(title="周末的猫", artist="生活频道")
            for s in (remote, local1, local2):
                s["trackId"] = track_id(s["title"], s["artist"], s["album"], s["durationMs"])
            self.peer["name"] = self.devices[0]["name"] = self.a2dp["device"] = "我的手机"
            self.focus["key"] = remote["key"]
        if args.long_names:
            # Layout stress test: every user-visible name is far longer than the space for it.
            remote["title"] = "测试曲目 · An Extremely Long Test Track Title That Keeps Going (Extended Live Version) [Remastered 2026]"
            remote["artist"] = "Fake Host Ensemble feat. A Very Long List Of Guest Artists And Orchestra Members"
            remote["album"] = "A Test Album With A Ridiculously Long Name To Check Ellipsis Handling (Deluxe Edition)"
            remote["app"]["name"] = "测试播放器 Test Player With A Very Long Application Name"
            remote["trackId"] = track_id(remote["title"], remote["artist"], remote["album"], remote["durationMs"])
            self.peer["name"] = "Test Phone (fake host) With An Unusually Long Device Name 1234567890"
            self.devices[0]["name"] = self.peer["name"]
            self.last_action = {"what": "pause_local", "reason": "remote_active",
                                "app": "SomeVeryLongProcessNameForTesting_ThatNeverEnds.exe", "atTick": now - 30000}
        if args.scenario == "idle":
            self.remote, self.local = [], []
        elif args.scenario in ("nopeer", "firstrun", "standalone"):
            self.remote, self.devices = [], []
            if args.scenario == "firstrun":
                self.settings["modeChosen"] = False
            if args.scenario == "standalone":
                self.settings["mode"] = "standalone"
            self.connection_status = "no_peer"
            self.peer = {"id": "", "name": "", "platform": ""}
            self.local = [local1]
            self.local[0]["status"] = "playing"
            self.focus = {"side": "local", "key": "Spotify.exe", "auto": True}
            self.last_action = None
        elif args.scenario == "pairing":
            self.pairing = {"state": "awaiting_user", "sas": "482915", "peerName": "Test Phone (fake host)", "error": None}
        elif args.scenario == "stale-pairing":
            # A host that keeps reporting the last pairing result: the UI must not pop a dialog.
            self.pairing = {"state": "succeeded", "sas": None, "peerName": "Test Phone (fake host)", "error": None}
        elif args.scenario == "compat":
            netease = {"_side": "local", "key": "cloudmusic.exe", "app": {"id": "cloudmusic.exe", "name": "网易云音乐"},
                       "title": "兼容模式测试曲 (test)", "artist": "Fake Host Ensemble", "album": "",
                       "durationMs": 212000, "positionMs": 40000, "atTick": now, "rate": 1.0, "status": "playing",
                       "actions": ["play", "pause", "next", "previous"], "artworkPath": None, "kind": "music",
                       "policyState": "playing", "source": "compat", "positionEstimated": True,
                       "appVolume": {"available": True, "level": 0.7, "muted": False}}
            netease["trackId"] = track_id(netease["title"], netease["artist"], netease["album"], netease["durationMs"])
            self.local.insert(0, netease)
            self.remote[0]["status"], self.remote[0]["policyState"] = "paused", "policy_paused"
            self.focus = {"side": "local", "key": "cloudmusic.exe", "auto": True}

    # ---- state ---------------------------------------------------------------------------
    def output_latency(self):
        """audioOutputLatency: delay the host compensates for Windows' current output."""
        default = next((o["id"] for o in self.outputs if o["default"]), None)
        bt = default in self.bluetooth_outputs
        lat = self.settings["lyrics"].get("latency", {})
        ms = int(lat.get("bluetoothMs", 180)) if bt and lat.get("auto", True) else 0
        return {"bluetooth": bt, "ms": ms}

    def session_out(self, s):
        out = {k: v for k, v in s.items() if not k.startswith("_")}
        # Lyrics timing (host is the single source of truth): output latency applies to what plays
        # on this PC; the phone's own output is not ours to compensate.
        latency = self.output_latency()["ms"] if s["_side"] == "local" else 0
        offset = int((self.settings["lyrics"].get("offsets") or {}).get(s["app"]["id"], 0))
        out["outputLatencyMs"] = latency
        out["lyricsDelayMs"] = latency + offset
        return out

    def gated(self):
        chosen = [o.get("id") for o in self.settings["policy"].get("onlyWithOutputs") or []]
        if not chosen:
            return False
        default = next((o["id"] for o in self.outputs if o["default"]), None)
        return default not in chosen

    def state(self):
        with self.lock:
            peer = self.peer if self.connection_status != "no_peer" else None
            discovered = []
            if self.discovering:
                discovered = [
                    {"kind": "bt", "id": "Bluetooth#Bluetooth00:aa:bb:cc:dd:ee-66:77:88:99:aa:bb", "name": "Test Tablet (fake)",
                     "detail": "66:77:88:99:AA:BB", "paired": False},
                    {"kind": "lan", "id": "192.168.1.31:47321", "name": "Another Test Phone", "detail": "192.168.1.31:47321", "paired": False, "ready": True},
                    {"kind": "bt", "id": "Bluetooth#Bluetooth00:aa:bb:cc:dd:ee-12:34:56:78:9a:bc", "name": "Sleeping Test Phone",
                     "detail": "12:34:56:78:9A:BC", "paired": False, "ready": False},
                    {"kind": "lan", "id": "192.168.1.20:47321", "name": self.peer["name"] or "Test Phone",
                     "detail": "192.168.1.20:47321", "paired": bool(self.devices)},
                ]
            return {
                "type": "state", "tick": tick(),
                "self": {"id": "f00dfeedf00dfeedf00dfeedf00dfeed", "name": "我的电脑" if self.args.showcase else os.environ.get("COMPUTERNAME", "TEST-PC"),
                         "app": self.self_app},
                "standalone": self.settings.get("mode") == "standalone",
                "connection": {"status": self.connection_status, "transport": "bt" if self.connection_status == "connected" else None,
                               "secondary": "lan" if self.connection_status == "connected" else None,
                               "peer": peer, "sinceTick": self.since, "rttMs": 41 if self.connection_status == "connected" else None,
                               "mode": self.settings["connection"]["mode"], "lastError": None,
                               "bytesIn": self.bytes_in, "bytesOut": self.bytes_out,
                               "bluetooth": {"adapter": True, "on": True}},
                "devices": self.devices,
                "local": {"gen": self.local_gen, "active": self.local[0]["key"] if self.local else None,
                          "sessions": [self.session_out(s) for s in self.local]},
                "remote": {"gen": self.remote_gen, "active": self.remote[0]["key"] if self.remote else None,
                           "sessions": [self.session_out(s) for s in self.remote]},
                "focus": self.focus,
                "policy": {"mode": self.settings["policy"]["mode"], "enabled": self.settings["policy"]["mode"] != "off",
                           "remote": self.remote[0]["policyState"] if self.remote else "unknown",
                           "local": self.local[0]["policyState"] if self.local else "unknown",
                           "windowsAudioActive": False, "activeApps": [], "suspended": False, "gated": self.gated(),
                           "lastAction": self.last_action},
                "audio": {"a2dpSink": self.a2dp, "lanSend": self.lansend},
                "volume": self.volume,
                "taskbar": self.taskbar_state if self.settings["taskbar"].get("enabled", True) else
                           {"supported": True, "shown": False, "reason": "disabled"},
                "audioOutputs": self.outputs,
                "audioOutputLatency": self.output_latency(),
                "pairing": self.pairing,
                "discovered": discovered,
                "lyricsOverlay": {"visible": self.overlay_visible},
                "settings": self.settings,
                "diagnostics": {"enabled": self.diag_enabled},
                # Handoff: music apps on this "PC" and the "phone" (the player card's menu).
                "handoff": {"apps": {"local": ["netease", "spotify"], "remote": ["qqmusic", "netease"]}, "active": None},
                "update": {"phase": self.args.update, "current": self.self_app,
                           "latest": "1.5.1" if self.args.update in ("available", "downloading") else self.self_app,
                           "page": "https://github.com/TIANLI0/MediaDuet/releases/latest", "notes": "",
                           "progress": 0.42 if self.args.update == "downloading" else 0.0, "error": None,
                           "auto": True, "portable": False, "checkedAt": 0},
            }

    def find(self, side, key):
        lst = self.local if side == "local" else self.remote
        if key is None:
            return lst[0] if lst else None
        for s in lst:
            if s["key"] == key:
                return s
        return None

    def focused(self):
        s = self.find(self.focus.get("side") or "remote", self.focus.get("key"))
        if s:
            return s
        for s in self.remote + self.local:
            if s["status"] == "playing":
                return s
        return (self.remote + self.local or [None])[0]

    # ---- pipe I/O ------------------------------------------------------------------------
    def send(self, obj):
        data = (json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8")
        with self.lock:
            if not self.pipe or not self.connected:
                return
            ev = kernel32.CreateEventW(None, True, False, None)
            ov = OVERLAPPED()
            ov.hEvent = ev
            n = wt.DWORD()
            buf = ctypes.create_string_buffer(data, len(data))
            ok = kernel32.WriteFile(self.pipe, buf, len(data), None, ctypes.byref(ov))
            if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
                self.connected = False
            elif not kernel32.GetOverlappedResult(self.pipe, ctypes.byref(ov), ctypes.byref(n), True):
                self.connected = False
            kernel32.CloseHandle(ev)

    def resolved_language(self):
        lang = self.settings.get("language", "auto")
        if lang in ("zh-CN", "en-US"):
            return lang
        return "zh-CN" if (kernel32.GetUserDefaultUILanguage() & 0x3FF) == 0x04 else "en-US"

    def send_hello(self):
        self.send({"type": "hello", "version": "0.0.0-fakehost", "pid": os.getpid(), "language": self.resolved_language()})
        items = [{"id": i, "title": f"测试曲目 {i + 1} · Queue Track {i + 1}", "subtitle": "Fake Host Ensemble"} for i in range(24)]
        self.send({"type": "queue", "side": "remote", "key": "com.example.testplayer#1",
                   "queue": {"key": "com.example.testplayer#1", "rev": "fake1", "title": "我喜欢的音乐 · Liked", "activeId": 3,
                             "total": 24, "offset": 0, "items": items}})

    def push_state(self):
        self.send(self.state())

    def push_lyrics(self):
        s = self.focused()
        if not s:
            return
        if not self.lyrics_enabled or not self.settings["lyrics"]["online"]:
            self.send({"type": "lyrics", "trackId": s["trackId"], "side": s["_side"], "key": s["key"],
                       "status": "notfound" if self.settings["lyrics"]["online"] else "disabled", "source": None,
                       "synced": False, "lines": []})
            return
        if s["kind"] != "music":
            self.send({"type": "lyrics", "trackId": s["trackId"], "side": s["_side"], "key": s["key"],
                       "status": "notfound", "source": None, "synced": False, "lines": []})
            return
        lyrics = build_lyrics(s, words=s is (self.remote[0] if self.remote else None),
                              translation=not self.args.no_translation, showcase=self.args.showcase)
        delay = getattr(self.args, "lyrics_loading_ms", 0) or 0
        if delay > 0:
            # Like the real host: "loading" first, the lyrics once the (simulated) lookup is done.
            self.send({"type": "lyrics", "trackId": s["trackId"], "side": s["_side"], "key": s["key"],
                       "status": "loading", "source": None, "synced": False, "lines": []})
            track = s["trackId"]

            def later():
                if self.focused() is s and s["trackId"] == track:
                    self.send(lyrics)
            threading.Timer(delay / 1000.0, later).start()
            return
        self.send(lyrics)

    def serve(self):
        name = self.args.pipe_name
        print(f"[fake-host] serving {name}")
        while True:
            pipe = kernel32.CreateNamedPipeW(
                name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, None)
            if pipe == INVALID_HANDLE_VALUE or pipe is None:
                raise ctypes.WinError(ctypes.get_last_error())
            ev = kernel32.CreateEventW(None, True, False, None)
            ov = OVERLAPPED()
            ov.hEvent = ev
            ok = kernel32.ConnectNamedPipe(pipe, ctypes.byref(ov))
            err = ctypes.get_last_error()
            if not ok and err == ERROR_IO_PENDING:
                n = wt.DWORD()
                kernel32.GetOverlappedResult(pipe, ctypes.byref(ov), ctypes.byref(n), True)
            elif not ok and err != ERROR_PIPE_CONNECTED:
                kernel32.CloseHandle(pipe)
                continue
            kernel32.CloseHandle(ev)
            with self.lock:
                old = self.pipe
                self.pipe = pipe
                self.connected = True
            if old:
                kernel32.DisconnectNamedPipe(old)
            print("[fake-host] UI connected")
            self.send_hello()
            self.push_state()
            self.push_lyrics()
            if self.args.show_on_connect:
                self.send({"type": "show", "window": self.args.show_on_connect})
                self.args.show_on_connect = None
            threading.Thread(target=self.reader, args=(pipe,), daemon=True).start()

    def reader(self, pipe):
        ev = kernel32.CreateEventW(None, True, False, None)
        pending = b""
        buf = ctypes.create_string_buffer(65536)
        while True:
            ov = OVERLAPPED()
            ov.hEvent = ev
            kernel32.ResetEvent(ev)
            n = wt.DWORD()
            ok = kernel32.ReadFile(pipe, buf, 65536, None, ctypes.byref(ov))
            if not ok and ctypes.get_last_error() != ERROR_IO_PENDING:
                break
            if not kernel32.GetOverlappedResult(pipe, ctypes.byref(ov), ctypes.byref(n), True) or n.value == 0:
                break
            pending += buf.raw[:n.value]
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                if line.strip():
                    try:
                        self.handle(json.loads(line.decode("utf-8")))
                    except Exception as e:  # keep serving on malformed input
                        print("[fake-host] bad message:", e, line[:200])
        kernel32.CloseHandle(ev)
        with self.lock:
            if self.pipe == pipe:
                self.connected = False
                self.pipe = None
        kernel32.CloseHandle(pipe)
        print("[fake-host] UI disconnected")

    # ---- commands ------------------------------------------------------------------------
    def handle(self, msg):
        t = msg.get("type")
        print("[fake-host] <-", json.dumps(msg, ensure_ascii=False))
        if self.log_file:
            self.log_file.write(json.dumps(msg, ensure_ascii=False) + "\n")
            self.log_file.flush()
        ok, error = True, None
        lyrics_changed = False
        resend_hello = False
        with self.lock:
            if t == "media.command":
                s = self.find(msg.get("side"), msg.get("key"))
                action = msg.get("action")
                if not s:
                    ok, error = False, "no such session"
                elif action not in s["actions"] and action != "toggle":
                    ok, error = False, "action_unsupported"
                else:
                    now = tick()
                    pos = s["positionMs"] + ((now - s["atTick"]) if s["status"] == "playing" else 0)
                    s["positionMs"], s["atTick"] = min(pos, s["durationMs"]), now
                    if action == "play" or (action == "toggle" and s["status"] != "playing"):
                        s["status"], s["policyState"] = "playing", "playing"
                    elif action in ("pause", "toggle"):
                        s["status"], s["policyState"] = "paused", "user_paused"
                    elif action == "seek":
                        s["positionMs"] = int(msg.get("positionMs", 0))
                    elif action in ("next", "previous"):
                        s["positionMs"] = 0
                        s["title"] = ("Next" if action == "next" else "Previous") + " test track " + time.strftime("%H:%M:%S")
                        s["trackId"] = track_id(s["title"], s["artist"], s["album"], s["durationMs"])
                        lyrics_changed = True
                    if s["_side"] == "local":
                        self.local_gen += 1
                    else:
                        self.remote_gen += 1
            elif t == "focus.set":
                if msg.get("auto"):
                    self.focus = {"side": "remote" if self.remote else "local",
                                  "key": (self.remote or self.local)[0]["key"] if (self.remote or self.local) else None, "auto": True}
                else:
                    self.focus = {"side": msg.get("side"), "key": msg.get("key"), "auto": False}
                lyrics_changed = True
            elif t == "settings.set":
                before = self.settings.get("language", "auto")
                merge(self.settings, msg.get("patch", {}))
                if "mode" in msg.get("patch", {}):
                    self.settings["modeChosen"] = True
                lyrics_changed = "lyrics" in msg.get("patch", {})
                if self.settings.get("language", "auto") != before:
                    resend_hello = True
            elif t == "pair.start":
                self.pairing = {"state": "connecting", "sas": None, "peerName": "Another Test Phone", "error": None}
                threading.Timer(1.5, self.pair_step, args=("awaiting_user",)).start()
            elif t == "pair.respond":
                if msg.get("accept"):
                    self.pairing = dict(self.pairing, state="awaiting_peer")
                    threading.Timer(2.0, self.pair_step, args=("succeeded",)).start()
                else:
                    self.pairing = dict(self.pairing, state="failed", error="rejected")
            elif t == "pair.cancel":
                self.pairing = {"state": "idle", "sas": None, "peerName": None, "error": None}
            elif t == "device.forget":
                self.devices = [d for d in self.devices if d["id"] != msg.get("id")]
                if not self.devices:
                    self.connection_status = "no_peer"
            elif t == "device.connect":
                for d in self.devices:
                    if d["id"] == msg.get("id"):
                        d["connected"] = True
                self.connection_status = "connected"
            elif t == "device.disconnect":
                for d in self.devices:
                    if d["id"] == msg.get("id"):
                        d["connected"] = False
                        d["lastSeen"] = int(time.time() * 1000)
                self.connection_status = "disconnected"
            elif t == "connect.manual":
                pass
            elif t == "discover":
                self.discovering = bool(msg.get("enable"))
            elif t == "audio.a2dpSink":
                self.a2dp["state"] = "connected" if msg.get("enable") else "off"
            elif t == "audio.lanSend":
                self.lansend["state"] = "streaming" if msg.get("enable") else "off"
            elif t == "volume.set":
                side = msg.get("side")
                if msg.get("key"):
                    sess = self.find("local", msg["key"])
                    target = sess.get("appVolume") if sess else None
                else:
                    target = self.volume.get(side)
                if not target or not target.get("available"):
                    ok, error = False, "volume_unavailable"
                elif side == "remote" and not msg.get("key") and self.volume_restricted:
                    # Android 17: the phone cannot change its volume while MediaDuet is in the background.
                    ok, error = False, "background_restricted"
                    target["error"] = "background_restricted"
                else:
                    target.pop("error", None)
                    if "level" in msg:
                        target["level"] = max(0.0, min(1.0, float(msg["level"])))
                    if "muted" in msg:
                        target["muted"] = bool(msg["muted"])
            elif t == "lyrics.offset":
                app = msg.get("appId")
                offsets = self.settings["lyrics"].setdefault("offsets", {})
                if not isinstance(app, str) or not app:
                    ok, error = False, "bad_app"
                elif msg.get("reset"):
                    offsets.pop(app, None)
                elif isinstance(msg.get("deltaMs"), (int, float)):
                    offsets[app] = max(-5000, min(5000, int(offsets.get(app, 0)) + int(msg["deltaMs"])))
                    if offsets[app] == 0:
                        offsets.pop(app)
                else:
                    ok, error = False, "bad_delta"
            elif t == "lyrics.overlay":
                self.overlay_visible = bool(msg.get("visible"))
                self.settings["lyrics"]["overlay"]["visible"] = self.overlay_visible
            elif t == "diagnostics":
                self.diag_enabled = bool(msg.get("enable"))
                self.settings["diagnostics"] = self.diag_enabled
            elif t == "host.exit":
                print("[fake-host] host.exit received - exiting")
                os._exit(0)
            else:
                ok, error = False, f"unknown command {t}"
        if "rid" in msg:
            self.send({"type": "result", "rid": msg["rid"], "ok": ok, "error": error})
        if resend_hello:
            self.send_hello()  # like the real host: hello (resolved language) after a language change
        self.push_state()
        if lyrics_changed:
            self.push_lyrics()

    def pair_step(self, state):
        with self.lock:
            if self.pairing["state"] in ("idle", "failed"):
                return
            if state == "awaiting_user":
                self.pairing = dict(self.pairing, state="awaiting_user", sas="482915")
            elif state == "succeeded":
                self.pairing = dict(self.pairing, state="succeeded")
                self.devices.append({"id": "0f1e2d3c4b5a69788796a5b4c3d2e1f0", "name": "Another Test Phone", "platform": "android",
                                     "btAddress": "", "lanHost": "192.168.1.31:47321", "connected": False,
                                     "transport": None, "lastSeen": int(time.time() * 1000)})
        self.push_state()

    def diagnostics_loop(self):
        while True:
            time.sleep(2)
            if self.diag_enabled and self.connected:
                self.bytes_in += 512
                self.bytes_out += 256
                self.send({"type": "diagnostics", "cpuPercent": 0.12, "workingSetMB": 21.4, "privateMB": 9.8,
                           "transport": "bt", "connectedForSec": (tick() - self.since) // 1000,
                           "bytesIn": self.bytes_in, "bytesOut": self.bytes_out, "msgsIn": 311, "msgsOut": 207,
                           "rttMs": 41, "lastCommandLatencyMs": 120, "localSessions": len(self.local),
                           "remoteSessions": len(self.remote), "audioSessionsActive": 1, "threads": 9})

    def console(self):
        for line in sys.stdin:
            cmd = line.strip().split()
            if not cmd:
                continue
            with self.lock:
                if cmd[0] == "pair":
                    self.pairing = {"state": "awaiting_user", "sas": "482915", "peerName": "Test Phone (fake host)", "error": None}
                elif cmd[0] == "unpair":
                    self.pairing = {"state": "idle", "sas": None, "peerName": None, "error": None}
                elif cmd[0] == "bgvolume" and len(cmd) > 1:
                    self.volume_restricted = cmd[1] == "on"
                    if not self.volume_restricted:
                        self.volume["remote"].pop("error", None)
                elif cmd[0] == "lyrics" and len(cmd) > 1:
                    self.lyrics_enabled = cmd[1] == "on"
                elif cmd[0] == "disconnect":
                    self.connection_status = "disconnected"
                elif cmd[0] == "connect":
                    self.connection_status = "connected"
                elif cmd[0] == "output" and len(cmd) > 1:
                    # switch the default output: output 0 | output 1
                    for k, o in enumerate(self.outputs):
                        o["default"] = (k == int(cmd[1]))
                elif cmd[0] == "lang" and len(cmd) > 1:
                    self.settings["language"] = cmd[1]
                elif cmd[0] == "quit":
                    os._exit(0)
            if cmd[0] == "lang":
                self.send_hello()
            if cmd[0] == "show" and len(cmd) > 1:
                self.send({"type": "show", "window": cmd[1]})
            elif cmd[0] == "next":
                self.handle({"type": "media.command", "side": "remote", "key": None, "action": "next"})
            self.push_state()
            self.push_lyrics()


def merge(target, patch):
    for k, v in patch.items():
        if v is None:
            target.pop(k, None)  # RFC 7396: null removes (the UI then uses the documented default)
        elif isinstance(v, dict) and isinstance(target.get(k), dict):
            merge(target[k], v)
        else:
            target[k] = v


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pipe", help="pipe name (default: MediaDuet.UI.<your SID>)")
    p.add_argument("--lang", choices=["auto", "zh-CN", "en-US"])
    p.add_argument("--theme", choices=["system", "light", "dark"])
    p.add_argument("--scenario", default="playing",
                   choices=["playing", "idle", "nopeer", "firstrun", "standalone", "pairing", "stale-pairing", "compat"])
    p.add_argument("--phone-version", default="1.5.0", help="the phone's app version (e.g. 1.1.0: version warning)")
    p.add_argument("--no-lyrics", action="store_true")
    p.add_argument("--lyrics-loading-ms", type=int, default=0,
                   help="answer 'loading' first and send the lyrics after this many ms (layout hold test)")
    p.add_argument("--volume-restricted", action="store_true",
                   help='phone volume changes fail with "background_restricted" (Android 17 background restriction)')
    p.add_argument("--no-translation", action="store_true", help="lyrics without translations (hasTranslation=false)")
    p.add_argument("--long-names", action="store_true", help="very long titles/names (layout clipping test)")
    p.add_argument("--showcase", action="store_true",
                   help="clean demo data for screenshots: invented song, original lyrics, neutral device names")
    p.add_argument("--diagnostics", action="store_true")
    p.add_argument("--update", default="current", choices=["idle", "current", "available", "downloading", "error"],
                   help="state.update phase (Settings > About, the main window's update bar)")
    p.add_argument("--log", help="append received commands (JSON lines) to this file")
    p.add_argument("--launch", choices=["main", "compact", "settings", "pair"],
                   help="start ../bin/x64/Release/MediaDuetUI.exe with --show <window>")
    p.add_argument("--switch-lang-after", nargs=2, metavar=("SECONDS", "LANG"),
                   help="change settings.language after SECONDS and re-send hello + state (runtime switch test)")
    p.add_argument("--show-on-connect", choices=["main", "compact", "settings", "pair"],
                   help="send a 'show' message right after the UI connects")
    args = p.parse_args()
    name = args.pipe or ("MediaDuet.UI." + current_user_sid())
    args.pipe_name = name if name.startswith("\\\\.\\pipe\\") else "\\\\.\\pipe\\" + name

    host = FakeHost(args)
    threading.Thread(target=host.diagnostics_loop, daemon=True).start()
    threading.Thread(target=host.serve, daemon=True).start()
    if args.launch:
        exe = Path(__file__).resolve().parent.parent / "bin" / "x64" / "Release" / "MediaDuetUI.exe"
        cmd = [str(exe), "--show", args.launch]
        if args.pipe:
            cmd += ["--pipe", args.pipe]
        subprocess.Popen(cmd)
    if args.switch_lang_after:
        def switch():
            time.sleep(float(args.switch_lang_after[0]))
            with host.lock:
                host.settings["language"] = args.switch_lang_after[1]
            print("[fake-host] switching language to", args.switch_lang_after[1])
            host.send_hello()
            host.push_state()
        threading.Thread(target=switch, daemon=True).start()
    if sys.stdin and sys.stdin.isatty():
        threading.Thread(target=host.console, daemon=True).start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
