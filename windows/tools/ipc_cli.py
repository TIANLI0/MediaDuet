#!/usr/bin/env python3
"""Developer/test harness for the MediaDuet host IPC pipe (docs/ipc.md).

Connects to \\\\.\\pipe\\MediaDuet.UI.<SID> like MediaDuetUI.exe does, sends JSON
commands and prints what the host reports. Not shipped to users.

Examples:
  python -I ipc_cli.py state                       # print one state snapshot
  python -I ipc_cli.py watch 10                    # print messages for 10 s
  python -I ipc_cli.py send '{"type":"pair.start","kind":"lan","target":"127.0.0.1:47321"}' 3
  python -I ipc_cli.py pair-lan 127.0.0.1:47321    # start pairing and auto-accept the SAS
  python -I ipc_cli.py cmd remote toggle
"""
import json
import subprocess
import sys
import threading
import time


def user_sid():
    out = subprocess.check_output(["whoami", "/user", "/fo", "csv", "/nh"], text=True).strip()
    return out.split(",")[-1].strip('"')


class Pipe:
    def __init__(self):
        name = r"\\.\pipe\MediaDuet.UI." + user_sid()
        for _ in range(50):
            try:
                self.f = open(name, "r+b", buffering=0)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("host pipe not available: is MediaDuet.exe running?")
        self.messages = []
        self.lock = threading.Lock()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while True:
            try:
                chunk = self.f.read(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                try:
                    msg = json.loads(line.decode("utf-8"))
                except ValueError:
                    continue
                with self.lock:
                    self.messages.append(msg)

    def send(self, obj):
        self.f.write((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))

    def wait_for(self, pred, timeout):
        end = time.time() + timeout
        while time.time() < end:
            with self.lock:
                for m in self.messages:
                    if pred(m):
                        return m
            time.sleep(0.05)
        return None

    def latest(self, typ):
        with self.lock:
            for m in reversed(self.messages):
                if m.get("type") == typ:
                    return m
        return None


def brief_state(s):
    c = s["connection"]
    out = {
        "connection": {k: c.get(k) for k in ("status", "transport", "secondary", "rttMs", "lastError")},
        "peer": (c.get("peer") or {}).get("name"),
        "pairing": s["pairing"],
        "focus": s["focus"],
        "policy": {k: s["policy"].get(k) for k in ("mode", "remote", "windowsAudioActive", "activeApps", "lastAction")},
        "local": [(x["title"], x["status"], x.get("positionMs")) for x in s["local"]["sessions"]],
        "remote": [(x["title"], x["artist"], x["status"], x.get("positionMs"), bool(x.get("artworkPath")))
                   for x in s["remote"]["sessions"]],
        "devices": [(d["name"], d["connected"], d["transport"]) for d in s["devices"]],
        "audio": s["audio"],
    }
    return json.dumps(out, ensure_ascii=False, indent=1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    p = Pipe()
    cmd = sys.argv[1]
    if cmd == "state":
        s = p.wait_for(lambda m: m.get("type") == "state", 5)
        print(brief_state(s) if s else "no state")
    elif cmd == "rawstate":
        s = p.wait_for(lambda m: m.get("type") == "state", 5)
        print(json.dumps(s, ensure_ascii=False, indent=1))
    elif cmd == "watch":
        dur = float(sys.argv[2]) if len(sys.argv) > 2 else 10
        seen = 0
        end = time.time() + dur
        while time.time() < end:
            with p.lock:
                new = p.messages[seen:]
                seen = len(p.messages)
            for m in new:
                if m.get("type") == "state":
                    print("STATE", brief_state(m))
                else:
                    print(m.get("type"), json.dumps(m, ensure_ascii=False)[:400])
            time.sleep(0.2)
    elif cmd == "send":
        p.send(json.loads(sys.argv[2]))
        time.sleep(float(sys.argv[3]) if len(sys.argv) > 3 else 1)
        r = p.latest("result")
        print("result:", r)
        s = p.latest("state")
        if s:
            print(brief_state(s))
    elif cmd == "pair-lan":
        p.send({"type": "pair.start", "kind": "lan", "target": sys.argv[2]})
        m = p.wait_for(lambda m: m.get("type") == "state" and m["pairing"]["state"] in ("awaiting_user", "failed"), 60)
        if not m:
            raise SystemExit("pairing did not reach SAS stage")
        print("pairing:", m["pairing"])
        if m["pairing"]["state"] == "awaiting_user":
            print("SAS shown on PC:", m["pairing"]["sas"], "-> accepting on PC")
            p.send({"type": "pair.respond", "accept": True})
            m = p.wait_for(lambda m: m.get("type") == "state" and m["pairing"]["state"] in ("succeeded", "failed"), 120)
            print("pairing result:", m["pairing"] if m else "timeout")
    elif cmd == "cmd":
        side, action = sys.argv[2], sys.argv[3]
        msg = {"type": "media.command", "side": side, "action": action, "rid": 1}
        if len(sys.argv) > 4:
            msg["positionMs"] = int(sys.argv[4])
        p.send(msg)
        r = p.wait_for(lambda m: m.get("type") == "result", 5)
        print("result:", r)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
