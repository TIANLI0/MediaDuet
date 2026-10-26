#!/usr/bin/env python3
"""Generates the MingCute icons (Apache-2.0) as vector drawables (android/app/src/main/res/drawable/ic_mc_*.xml)
and android/.../ui/components/Icons.kt, which exposes them to Compose as AppIcons.*.

    python tools/gen_android_icons.py [path/to/mingcute_icon/package]

Without an argument the pinned npm tarball is downloaded. Every icon is a filled outline on a
24 x 24 grid; the invisible bounding path of each SVG is dropped and even-odd fill is kept.
"""
import io
import os
import re
import sys
import tarfile
import tempfile
import urllib.request

VERSION = "2.9.72"
TARBALL = f"https://registry.npmjs.org/mingcute_icon/-/mingcute_icon-{VERSION}.tgz"
OUT = os.path.join(os.path.dirname(__file__), "..", "android", "app", "src", "main", "kotlin",
                   "app", "mediaduet", "ui", "components", "Icons.kt")

# Kotlin name -> (svg path inside package/svg, doc comment or None)
ICONS = [
    ("Play", "media/play_fill", None),
    ("Pause", "media/pause_fill", None),
    ("Next", "media/skip_forward_fill", None),
    ("Previous", "media/skip_previous_fill", None),
    ("Bluetooth", "device/bluetooth_line", None),
    ("Wifi", "device/wifi_line", None),
    ("Music", "media/music_2_line", None),
    ("Phone", "device/cellphone_line", None),
    ("Computer", "device/laptop_line", None),
    ("Devices", "device/device_line", None),
    ("Link", "file/link_line", None),
    ("Shield", "system/safety_certificate_line", None),
    ("Tune", "arrow/transfer_3_line", "Arbitration (who plays)."),
    ("Gear", "system/settings_3_line", "Settings gear."),
    ("Speaker", "device/speaker_line", None),
    ("Globe", "map/world_2_line", None),
    ("Pulse", "system/heartbeat_line", "Diagnostics."),
    ("Info", "system/information_line", None),
    ("Mail", "contact/mail_line", None),
    ("Check", "system/check_line", None),
    ("CheckCircle", "system/check_circle_line", None),
    ("Warning", "system/warning_line", None),
    ("ExpandMore", "arrow/down_line", None),
    ("ChevronRight", "arrow/right_line", None),
    ("ChevronLeft", "arrow/left_line", None),
    ("Plus", "system/add_line", None),
    ("Close", "system/close_line", None),
    ("More", "system/more_1_line", None),
    ("Auto", "design/magic_2_line", "Automatic source."),
    ("Translate", "editor/translate_2_line", None),
    ("Timer", "device/stopwatch_line", "Stopwatch (lyrics timing)."),
    ("TextSize", "editor/font_size_line", "Lyrics text size."),
    ("Playlist", "media/playlist_line", "Up next (play queue)."),
    ("Restore", "system/refresh_anticlockwise_1_line", "Reset."),
    ("VolumeUp", "media/volume_line", None),
    ("VolumeOff", "media/volume_off_line", None),
    ("QrCode", "device/qrcode_2_line", "Scan a PC's pairing code."),
    ("Flashlight", "device/flashlight_line", "Torch (QR scanner)."),
]

MINUS = "M4 11h16a1 1 0 1 1 0 2H4a1 1 0 1 1 0-2Z"

PATH_RE = re.compile(r"<path([^>]*)/>")
ATTR_RE = re.compile(r'(\w[\w-]*)="([^"]*)"')


def load(pkg, rel):
    with open(os.path.join(pkg, "svg", rel + ".svg"), encoding="utf-8") as f:
        svg = f.read()
    even_odd = 'fill-rule="evenodd"' in svg
    paths = []
    for m in PATH_RE.finditer(svg):
        attrs = dict(ATTR_RE.findall(m.group(1)))
        if attrs.get("fill", "none") == "none":
            continue  # 24 x 24 bounding box / guide path
        paths.append(attrs["d"])
    if not paths:
        raise SystemExit(f"{rel}: no filled path")
    return paths, even_odd


TOKEN = re.compile(r"[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
ARITY = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}


def fmt(v):
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def subpaths_absolute(d):
    """Splits a path into subpaths with absolute coordinates (so subpaths can be dropped)."""
    toks = TOKEN.findall(d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    subs, cur = [], []
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                cur.append("Z")
                x, y = sx, sy
                continue
        up = cmd.upper()
        rel = cmd.islower()
        args = [float(t) for t in toks[i:i + ARITY[up]]]
        i += ARITY[up]
        if up == "M":
            if cur:
                subs.append(cur)
            cur = []
            nx, ny = (x + args[0], y + args[1]) if rel else args
            cur.append(f"M{fmt(nx)},{fmt(ny)}")
            x, y = sx, sy = nx, ny
            cmd = "l" if rel else "L"  # implicit lineto after moveto
        elif up == "H":
            x = x + args[0] if rel else args[0]
            cur.append(f"H{fmt(x)}")
        elif up == "V":
            y = y + args[0] if rel else args[0]
            cur.append(f"V{fmt(y)}")
        elif up == "A":
            ex, ey = (x + args[5], y + args[6]) if rel else (args[5], args[6])
            cur.append("A" + ",".join(fmt(a) for a in args[:3]) + f",{int(args[3])},{int(args[4])},{fmt(ex)},{fmt(ey)}")
            x, y = ex, ey
        else:
            pts = []
            for k in range(0, len(args), 2):
                px, py = (x + args[k], y + args[k + 1]) if rel else (args[k], args[k + 1])
                pts += [fmt(px), fmt(py)]
            cur.append(up + ",".join(pts))
            x, y = (float(pts[-2]), float(pts[-1]))
    if cur:
        subs.append(cur)
    return ["".join(s) for s in subs]



PREFIX = "ic_mc_"
RES = os.path.join(os.path.dirname(__file__), "..", "android", "app", "src", "main", "res", "drawable")


def drawable_name(name):
    """Kotlin name -> drawable resource name: QuoteBubble -> ic_mc_quote_bubble."""
    return PREFIX + re.sub(r"(?<!^)(?=[A-Z])", "_", name).lower()


def xml_path(d, even_odd, indent):
    fill = ' android:fillType="evenOdd"' if even_odd else ""
    return (f'{indent}<path android:fillColor="#FF000000"{fill}\n'
            f'{indent}    android:pathData="{d}" />\n')


def main():
    if len(sys.argv) > 1:
        pkg = sys.argv[1]
    else:
        data = urllib.request.urlopen(TARBALL).read()
        tmp = os.path.join(tempfile.gettempdir(), f"mediaduet-mingcute-{VERSION}")  # never inside the source tree
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(tmp, filter="data")
        pkg = os.path.join(tmp, "package")

    # (Kotlin name, doc, [(path, evenOdd)], inner path of a composite or None)
    icons = []
    for name, rel, doc in ICONS:
        paths, eo = load(pkg, rel)
        icons.append((name, doc, [(p, eo) for p in paths], None))

    # Volume low: the speaker and the inner wave of volume_line (outer wave dropped).
    vol, eo = load(pkg, "media/volume_line")
    subs = subpaths_absolute(vol[0])
    icons.append(("VolumeDown", None, [("".join(subs[:2] + subs[3:]), eo)], None))
    icons.append(("Minus", None, [(MINUS, False)], None))

    # Lyrics toggle: a speech bubble with small quote marks inside (Apple-Music-like).
    bubble, _ = load(pkg, "contact/chat_3_line")
    quote, _ = load(pkg, "editor/quote_left_line")
    icons.append(("QuoteBubble", "Quote bubble (lyrics toggle).", [(bubble[0], True)], quote[0]))

    os.makedirs(RES, exist_ok=True)
    # Drawables generated before and no longer listed are removed (only the ic_mc_* prefix).
    wanted = {drawable_name(n) + ".xml" for n, *_ in icons}
    for f in os.listdir(RES):
        if f.startswith(PREFIX) and f not in wanted:
            os.remove(os.path.join(RES, f))

    lines = []
    for name, doc, paths, inner in icons:
        body = "".join(xml_path(p, eo, "    ") for p, eo in paths)
        if inner is not None:
            # The bubble at full size, the quote marks scaled down into its body.
            body += ('    <group android:scaleX="0.46" android:scaleY="0.46" android:pivotX="12"\n'
                     '        android:pivotY="12" android:translateY="-1.1">\n'
                     + xml_path(inner, True, "        ") + "    </group>\n")
        xml = ('<?xml version="1.0" encoding="utf-8"?>\n'
               f'<!-- Generated by tools/gen_android_icons.py from MingCute {VERSION} (Apache-2.0); do not edit by hand. -->\n'
               '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
               '    android:width="24dp"\n'
               '    android:height="24dp"\n'
               '    android:viewportWidth="24"\n'
               '    android:viewportHeight="24">\n'
               f"{body}</vector>\n")
        with open(os.path.join(RES, drawable_name(name) + ".xml"), "w", encoding="utf-8", newline="\n") as f:
            f.write(xml)
        if doc:
            lines.append(f"    /** {doc} */")
        lines.append(f"    val {name}: ImageVector @Composable get() = ImageVector.vectorResource(R.drawable.{drawable_name(name)})")

    body = "\n".join(lines)
    kt = f'''// Generated by tools/gen_android_icons.py from MingCute {VERSION} - do not edit by hand.
package app.mediaduet.ui.components

import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.vectorResource
import app.mediaduet.R

/**
 * MingCute icons (https://www.mingcute.com, Apache-2.0) used by the UI: rounded 2 px outlines,
 * filled transport glyphs. Each one is a vector drawable resource (res/drawable/{PREFIX}*.xml, so
 * notifications, tiles and XML can use them too); these accessors load them as tintable
 * ImageVectors (Compose caches each resource).
 */
object AppIcons {{
{body}
}}
'''
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(kt)
    print("wrote", os.path.normpath(OUT), "and", len(icons), "drawables in", os.path.normpath(RES))


if __name__ == "__main__":
    main()
