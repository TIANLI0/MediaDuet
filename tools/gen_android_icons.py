#!/usr/bin/env python3
"""Generates android/.../ui/components/Icons.kt from the MingCute icon set (Apache-2.0).

    python tools/gen_android_icons.py [path/to/mingcute_icon/package]

Without an argument the pinned npm tarball is downloaded. Every icon is a filled outline on a
24 x 24 grid; the invisible bounding path of each SVG is dropped and even-odd fill is kept.
"""
import io
import os
import re
import sys
import tarfile
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
    ("Restore", "system/refresh_anticlockwise_1_line", "Reset."),
    ("VolumeUp", "media/volume_line", None),
    ("VolumeOff", "media/volume_off_line", None),
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


def kotlin_string(s):
    return '"' + s + '"'


def main():
    if len(sys.argv) > 1:
        pkg = sys.argv[1]
    else:
        data = urllib.request.urlopen(TARBALL).read()
        tmp = os.path.join(os.path.dirname(OUT), ".mingcute")
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            t.extractall(tmp, filter="data")
        pkg = os.path.join(tmp, "package")

    out = []
    for name, rel, doc in ICONS:
        paths, eo = load(pkg, rel)
        if doc:
            out.append(f"    /** {doc} */")
        args = ", ".join(kotlin_string(p) for p in paths)
        out.append(f'    val {name} = icon("{name[0].lower() + name[1:]}", evenOdd = {str(eo).lower()}, {args})')

    # Volume low: the speaker and the inner wave of volume_line (outer wave dropped).
    vol, eo = load(pkg, "media/volume_line")
    subs = subpaths_absolute(vol[0])
    low = "".join(subs[:2] + subs[3:])
    out.append(f'    val VolumeDown = icon("volumeDown", evenOdd = {str(eo).lower()}, {kotlin_string(low)})')
    out.append(f'    val Minus = icon("minus", evenOdd = false, {kotlin_string(MINUS)})')

    # Lyrics toggle: a speech bubble with small quote marks inside (Apple-Music-like).
    bubble, _ = load(pkg, "contact/chat_3_line")
    quote, _ = load(pkg, "editor/quote_left_line")
    out.append("    /** Quote bubble (lyrics toggle). */")
    out.append(
        "    val QuoteBubble = composite(\n"
        f"        \"quoteBubble\",\n"
        f"        {kotlin_string(bubble[0])},\n"
        f"        {kotlin_string(quote[0])},\n"
        "    )"
    )

    body = "\n".join(out)
    kt = f'''// Generated by tools/gen_android_icons.py from MingCute {VERSION} - do not edit by hand.
package app.mediaduet.ui.components

import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathFillType
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.PathParser
import androidx.compose.ui.unit.dp

/** MingCute icons (https://www.mingcute.com, Apache-2.0) used by the UI: rounded 2 px outlines, filled transport glyphs. */
object AppIcons {{
    private fun ImageVector.Builder.path(d: String, evenOdd: Boolean) = addPath(
        PathParser().parsePathString(d).toNodes(),
        pathFillType = if (evenOdd) PathFillType.EvenOdd else PathFillType.NonZero,
        fill = SolidColor(Color.Black),
    )

    private fun icon(name: String, evenOdd: Boolean, vararg paths: String): ImageVector {{
        val b = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
        for (p in paths) b.path(p, evenOdd)
        return b.build()
    }}

    /** [outer] at full size with [inner] scaled down into its centre (the bubble's body). */
    private fun composite(name: String, outer: String, inner: String): ImageVector {{
        val b = ImageVector.Builder(name, 24.dp, 24.dp, 24f, 24f)
        b.path(outer, evenOdd = true)
        b.addGroup(scaleX = 0.46f, scaleY = 0.46f, pivotX = 12f, pivotY = 12f, translationY = -1.1f)
        b.path(inner, evenOdd = true)
        b.clearGroup()
        return b.build()
    }}

{body}
}}
'''
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(kt)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
