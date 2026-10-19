#!/usr/bin/env python3
"""MediaDuet protocol v1 reference *client* (the Windows "C" role) for interop testing.

Independent implementation built only on the Python `cryptography` package and the
specification. It talks to the Android server over TCP (e.g. through
`adb forward tcp:47321 tcp:47321`).

    # 1. open pairing mode in the Android app, then:
    python -I protocol/tools/peer_client.py --state peer.json pair --yes
    # 2. connect, print snapshots, send a pause, fetch the artwork:
    python -I protocol/tools/peer_client.py --state peer.json session --pause --artwork art.jpg

The state file holds this test client's identity and the pairing key in plain text: it is a
test tool, never use it for real devices.
"""
import argparse
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import struct
import sys
import time
import functools

print = functools.partial(print, flush=True)  # noqa: A001 - live output when piped

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HANDSHAKE, SECURE = 0x01, 0x02
CONTROL, BLOB = 0x01, 0x02
MAX_FRAME = 262144


# --------------------------------------------------------------------------------------------
# primitives
# --------------------------------------------------------------------------------------------

def hkdf_extract(salt, ikm):
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk, info, length):
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def hmac256(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()


def new_ephemeral():
    priv = ec.generate_private_key(ec.SECP256R1())
    pub = priv.public_key().public_bytes(serialization.Encoding.X962, serialization.PublicFormat.UncompressedPoint)
    return priv, pub


def ecdh(priv, peer_pub):
    return priv.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), peer_pub))


def b64(b):
    return base64.b64encode(b).decode()


def unb64(s):
    return base64.b64decode(s, validate=True)


class Record:
    """One direction of §5 record protection."""

    def __init__(self, key, iv4, ftype):
        self.aead, self.iv, self.seq = AESGCM(key), iv4, 0
        self.aad = b"MDv1" + bytes([ftype])

    def seal(self, plaintext):
        out = self.aead.encrypt(self.iv + struct.pack(">Q", self.seq), plaintext, self.aad)
        self.seq += 1
        return out

    def open(self, ciphertext):
        out = self.aead.decrypt(self.iv + struct.pack(">Q", self.seq), ciphertext, self.aad)
        self.seq += 1
        return out


# --------------------------------------------------------------------------------------------
# connection
# --------------------------------------------------------------------------------------------

class Conn:
    def __init__(self, host, port, my_id):
        self.sock = socket.create_connection((host, port), timeout=15)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.my_id = my_id
        self.next_id = 1
        self.sid = ""
        self.tx = self.rx = None

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("connection closed by server")
            buf += chunk
        return buf

    def read_frame(self):
        length = struct.unpack(">I", self._read_exact(4))[0]
        if not 1 <= length <= MAX_FRAME:
            raise ValueError(f"bad frame length {length}")
        body = self._read_exact(length)
        return body[0], body[1:]

    def write_frame(self, ftype, body):
        self.sock.sendall(struct.pack(">I", len(body) + 1) + bytes([ftype]) + body)

    def envelope(self, mtype, body, re=None, msg_id=None):
        if msg_id is None:
            msg_id = self.next_id
            self.next_id += 1
        env = {"v": 1, "type": mtype, "id": msg_id, "ts": int(time.time() * 1000), "src": self.my_id, "sid": self.sid}
        if re is not None:
            env["re"] = re
        env["body"] = body
        return env

    def send_handshake(self, mtype, body):
        env = self.envelope(mtype, body)
        self.write_frame(HANDSHAKE, json.dumps(env, separators=(",", ":"), ensure_ascii=False).encode())
        return env

    def read_handshake(self):
        ftype, body = self.read_frame()
        if ftype != HANDSHAKE:
            raise ValueError(f"expected HANDSHAKE frame, got {ftype}")
        return json.loads(body.decode())

    def send(self, mtype, body, re=None, msg_id=None):
        env = self.envelope(mtype, body, re, msg_id)
        data = json.dumps(env, separators=(",", ":"), ensure_ascii=False).encode()
        self.write_frame(SECURE, self.tx.seal(bytes([CONTROL]) + data))
        return env

    def resend(self, env):
        data = json.dumps(env, separators=(",", ":"), ensure_ascii=False).encode()
        self.write_frame(SECURE, self.tx.seal(bytes([CONTROL]) + data))

    def read_record(self):
        ftype, body = self.read_frame()
        if ftype != SECURE:
            if ftype == HANDSHAKE:
                raise ValueError("server answered with a HANDSHAKE frame: " + body.decode())
            raise ValueError(f"unexpected frame type {ftype}")
        plain = self.rx.open(body)
        return plain[0], plain[1:]


def device_info(my_id, name):
    return {"id": my_id, "name": name, "platform": "windows", "model": "peer_client.py",
            "os": sys.platform, "app": "test"}


# --------------------------------------------------------------------------------------------
# pairing (§3.3)
# --------------------------------------------------------------------------------------------

def cmd_pair(args, state):
    c = Conn(args.host, args.port, state["id"])
    priv, epk = new_ephemeral()
    n_c = secrets.token_bytes(32)
    c.send_handshake("pair.request", {"device": device_info(state["id"], state["name"]), "epk": b64(epk), "nonce": b64(n_c)})
    resp = c.read_handshake()
    if resp["type"] != "pair.response":
        print("server refused:", json.dumps(resp["body"]))
        return 1
    server = resp["body"]["device"]
    epk_s, n_s = unb64(resp["body"]["epk"]), unb64(resp["body"]["nonce"])
    z = ecdh(priv, epk_s)
    th = hashlib.sha256(b"MediaDuet-pair-v1" + n_c + n_s + epk + epk_s + state["id"].encode() + server["id"].encode()).digest()
    prk = hkdf_extract(th, z)
    sas = "%06d" % (struct.unpack(">I", hkdf_expand(prk, b"sas", 4))[0] % 1_000_000)
    k = hkdf_expand(prk, b"pairing key", 32)
    print(f"SAS: {sas}   (server: {server['name']} {server['id']})", flush=True)
    if not args.yes and input("Do the codes match? [y/N] ").strip().lower() != "y":
        c.send_handshake("pair.result", {"ok": False, "reason": "rejected"})
        return 1
    c.send_handshake("pair.confirm", {"mac": b64(hmac256(k, b"confirm" + b"C" + th))})
    print("sent pair.confirm, waiting for the phone user to accept ...", flush=True)
    c.sock.settimeout(130)
    while True:
        msg = c.read_handshake()
        if msg["type"] == "pair.confirm":
            if not hmac.compare_digest(unb64(msg["body"]["mac"]), hmac256(k, b"confirm" + b"S" + th)):
                print("server confirm MAC mismatch!")
                return 1
            print("server pair.confirm verified")
        elif msg["type"] == "pair.result":
            if not msg["body"].get("ok"):
                print("pairing failed:", msg["body"].get("reason"))
                return 1
            state.update(server_id=server["id"], server_name=server["name"], key=k.hex())
            save_state(args.state, state)
            print("PAIRED; key stored in", args.state)
            return 0
        else:
            print("unexpected", msg["type"], msg["body"])
            return 1


# --------------------------------------------------------------------------------------------
# session (§4) + messages
# --------------------------------------------------------------------------------------------

def open_session(args, state, resume=None):
    c = Conn(args.host, args.port, state["id"])
    k = bytes.fromhex(state["key"])
    priv, epk = new_ephemeral()
    n_c = secrets.token_bytes(32)
    body = {"device": device_info(state["id"], state["name"]), "protocol": {"min": 1, "max": 1},
            "caps": ["media.source", "media.control", "artwork", "lyrics", "policy.host", "volume", "handoff"],
            "epk": b64(epk), "nonce": b64(n_c), "transport": "lan"}
    if resume:
        body["resume"] = resume
        # Message ids are per link session (§7): continue the counter when resuming.
        c.next_id = state.get("next_id", 1)
    c.send_handshake("hello", body)
    ack = c.read_handshake()
    if ack["type"] != "hello.ack":
        print("handshake refused:", ack["type"], json.dumps(ack["body"]))
        return None, None
    b = ack["body"]
    epk_s, n_s = unb64(b["epk"]), unb64(b["nonce"])
    z = ecdh(priv, epk_s)
    th = hashlib.sha256(b"MediaDuet-session-v1" + n_c + n_s + epk + epk_s + state["id"].encode() + ack["src"].encode()).digest()
    if not hmac.compare_digest(unb64(b["mac"]), hmac256(k, b"server" + th)):
        raise ValueError("hello.ack MAC mismatch")
    c.send_handshake("auth", {"mac": b64(hmac256(k, b"client" + th))})
    prk = hkdf_extract(k, z)
    c.key_c2s = hkdf_expand(prk, b"c2s key" + th, 32)
    c.tx = Record(hkdf_expand(prk, b"c2s key" + th, 32), hkdf_expand(prk, b"c2s iv" + th, 4), SECURE)
    c.rx = Record(hkdf_expand(prk, b"s2c key" + th, 32), hkdf_expand(prk, b"s2c iv" + th, 4), SECURE)
    c.sid = b["sid"]
    print(f"session {b['sid']} resumed={b['resumed']} server caps={b['caps']} protocol={b['protocol']}")
    return c, b


class Session:
    def __init__(self, conn):
        self.c = conn
        self.served = {}          # artwork we serve: sha256 hex -> bytes
        self.demo = None          # our own (Windows-side) snapshot when --demo is used
        self.demo_volume = {"level": 0.6, "muted": False, "steps": 50}   # our device volume (demo)
        self.peer_volume = {}     # last volume.state from the phone, by re (None = unsolicited)
        self.gen = 0
        self.no_reply = False     # --no-reply: never answer media.command (timeout testing)
        self.snapshot = None
        self.blobs = {}
        self.results = {}
        self.pongs = {}
        self.handoff = {}         # handoff.result by re
        self.handoff_apps = None  # the phone's handoff.apps

    def pump(self, until=None, timeout=10.0):
        """Reads records, handling them, until until() is true or the timeout expires."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if until and until():
                return True
            self.c.sock.settimeout(max(0.1, deadline - time.time()))
            try:
                channel, data = self.c.read_record()
            except socket.timeout:
                break
            if channel == BLOB:
                digest, total, offset = data[:32].hex(), *struct.unpack(">II", data[32:40])
                entry = self.blobs.setdefault(digest, {"total": total, "parts": {}})
                entry["parts"][offset] = data[40:]
                continue
            env = json.loads(data.decode())
            t, body = env["type"], env["body"]
            if t == "ping":
                self.c.send("pong", {"t": body.get("t")}, re=env["id"])
            elif t == "pong":
                self.pongs[env.get("re")] = time.time()
            elif t == "media.snapshot":
                self.snapshot = body
                print_snapshot(body)
            elif t == "media.command.result":
                self.results.setdefault(env.get("re"), []).append(body)
                print(f"<- media.command.result re={env.get('re')} {json.dumps(body)}")
            elif t == "artwork.get":
                data = self.served.get(body.get("hash"))
                if data is None:
                    self.c.send("error", {"code": "not_found"}, re=env["id"])
                else:
                    digest = bytes.fromhex(body["hash"])
                    for off in range(0, len(data), 32768):
                        part = data[off:off + 32768]
                        chunk = bytes([BLOB]) + digest + struct.pack(">II", len(data), off) + part
                        self.c.write_frame(SECURE, self.c.tx.seal(chunk))
                    print(f"-> served artwork {body['hash'][:16]} ({len(data)} bytes)")
            elif t == "media.command" and self.no_reply:
                print(f"<- media.command {json.dumps(body)} (ignored: --no-reply)")
            elif t == "media.command":
                print(f"<- media.command {json.dumps(body)} (origin {body.get('origin')})")
                ok = self.demo is not None
                self.c.send("media.command.result", {"ok": ok} if ok else {"ok": False, "error": {"code": "not_found"}}, re=env["id"])
                if ok:
                    sess = self.demo["sessions"][0]
                    a = body.get("action")
                    if a in ("pause", "stop") or (a == "toggle" and sess["status"] == "playing"):
                        sess["status"] = "paused"
                    elif a in ("play", "toggle"):
                        sess["status"] = "playing"
                    elif a == "seek":
                        sess["positionMs"] = body.get("positionMs", 0)
                    self.send_demo()
            elif t == "volume.state":
                self.peer_volume[env.get("re")] = body
                print(f"<- volume.state re={env.get('re')} {json.dumps(body)}")
            elif t == "volume.set":
                print(f"<- volume.set {json.dumps(body)}")
                if self.demo is None:
                    self.c.send("error", {"code": "unsupported"}, re=env["id"])
                elif body.get("key"):
                    # Per-app volume of one of our (demo) sessions.
                    for sess in self.demo["sessions"]:
                        if sess["key"] == body["key"] and "volume" in sess:
                            if "level" in body:
                                sess["volume"]["level"] = max(0.0, min(1.0, float(body["level"])))
                            if "muted" in body:
                                sess["volume"]["muted"] = bool(body["muted"])
                    self.send_demo()
                else:
                    v = self.demo_volume
                    if "level" in body:
                        v["level"] = max(0.0, min(1.0, float(body["level"])))
                    if "delta" in body:
                        v["level"] = max(0.0, min(1.0, v["level"] + int(body["delta"]) / v["steps"]))
                    if "muted" in body:
                        v["muted"] = bool(body["muted"])
                    self.c.send("volume.state", dict(v), re=env["id"])
                    print(f"-> volume.state {json.dumps(v)}")
            elif t == "handoff.apps":
                self.handoff_apps = body.get("apps")
                print(f"<- handoff.apps {json.dumps(body)}")
            elif t == "handoff.progress":
                print(f"<- handoff.progress re={env.get('re')} {json.dumps(body)}")
            elif t == "handoff.result":
                self.handoff[env.get("re")] = body
                print(f"<- handoff.result re={env.get('re')} {json.dumps(body, ensure_ascii=False)}")
            elif t == "handoff.start":
                print(f"<- handoff.start {json.dumps(body, ensure_ascii=False)}")
                self.c.send("handoff.result", {"ok": False, "to": "pc", "title": "", "app": None,
                                               "error": {"code": "unsupported"}}, re=env["id"])
            elif t == "policy.config":
                print(f"<- policy.config {json.dumps(body)}")
            elif t == "error":
                self.results.setdefault(env.get("re"), []).append(body)
                print(f"<- error re={env.get('re')} {json.dumps(body)}")
            else:
                print(f"<- {t} {json.dumps(body, ensure_ascii=False)}")
        return bool(until and until())


def send_demo_snapshot(self):
    self.gen += 1
    self.demo["gen"] = self.gen
    self.c.send("media.snapshot", self.demo)
    print(f"-> media.snapshot gen={self.gen} status={self.demo['sessions'][0]['status']}")


Session.send_demo = send_demo_snapshot


def track_id(title, artist, album, duration_ms):
    dur = "" if duration_ms is None else str((int(duration_ms) + 500) // 1000)
    return hashlib.sha256("".join([title, artist, album, dur]).encode()).hexdigest()[:16]


def print_snapshot(s):
    print(f"<- media.snapshot gen={s['gen']} active={s.get('active')}")
    for x in s.get("sessions", []):
        art = (x.get("artwork") or {}).get("hash", "")[:16]
        print(f"     [{x['status']:>9}] {x['key']}  {x.get('title')!r} / {x.get('artist')!r}  "
              f"pos={x.get('positionMs')} dur={x.get('durationMs')} trackId={x.get('trackId')} "
              f"actions={','.join(x.get('actions', []))} art={art}")


def active_session(snapshot):
    if not snapshot:
        return None
    for s in snapshot.get("sessions", []):
        if s["key"] == snapshot.get("active"):
            return s
    return (snapshot.get("sessions") or [None])[0]


def cmd_session(args, state):
    c, ack = open_session(args, state, resume=state.get("sid") if args.resume else None)
    if c is None:
        return 2
    if not ack["resumed"]:
        c.next_id = max(c.next_id, 1)
    state["sid"] = ack["sid"]
    save_state(args.state, state)
    s = Session(c)
    s.no_reply = args.no_reply
    s.pump(lambda: s.snapshot is not None, timeout=5)
    c.send("link.primary", {})

    ping = c.send("ping", {"t": int(time.monotonic() * 1000)})
    t0 = time.time()
    if s.pump(lambda: ping["id"] in s.pongs, timeout=5):
        print(f"ping/pong RTT {1000 * (s.pongs[ping['id']] - t0):.1f} ms")

    rc = 0
    if args.demo:
        art = open(args.demo, "rb").read()
        h = hashlib.sha256(art).hexdigest()
        s.served[h] = art
        title, artist, album, dur = "晴天", "周杰伦", "叶惠美", 269000
        s.demo = {"gen": 0, "active": "Spotify.exe", "sessions": [{
            "key": "Spotify.exe", "app": {"id": "Spotify.exe", "name": "Spotify"},
            "title": title, "artist": artist, "album": album, "albumArtist": artist,
            "trackId": track_id(title, artist, album, dur), "durationMs": dur, "positionMs": 3000,
            "rate": 1.0, "status": "playing", "actions": ["play", "pause", "toggle", "next", "previous", "seek"],
            "artwork": {"hash": h, "mime": "image/jpeg", "size": len(art)}, "kind": "music",
            "volume": {"level": 0.8, "muted": False}}]}
        s.send_demo()
        c.send("volume.state", dict(s.demo_volume))
        print(f"-> volume.state {json.dumps(s.demo_volume)}")
        if args.lyrics:
            text = open(args.lyrics, encoding="utf-8").read()
            extra = {"translation": open(args.translation, encoding="utf-8").read()} if args.translation else {}
            c.send("lyrics.push", {"trackId": s.demo["sessions"][0]["trackId"], "format": "lrc", "source": "peer_client", "text": text, **extra})
            print("-> lyrics.push for the demo track")
            # The host also fetches lyrics for the phone's focused track.
            phone_track = (active_session(s.snapshot) or {}).get("trackId")
            if phone_track:
                c.send("lyrics.push", {"trackId": phone_track, "format": "lrc", "source": "peer_client", "text": text, **extra})
                print(f"-> lyrics.push for the phone track {phone_track}")
    if args.lyrics and not args.demo:
        a = active_session(s.snapshot)
        if a:
            text = open(args.lyrics, encoding="utf-8").read()
            c.send("lyrics.push", {"trackId": a["trackId"], "format": "lrc", "source": "peer_client", "text": text})
            print(f"-> lyrics.push for trackId {a['trackId']}")
    if args.policy:
        c.send("policy.status", {"mode": "windows_first", "enabled": True, "remote": "playing", "local": "idle",
                                 "lastAction": {"what": "pause_remote", "reason": "windows_audio_active",
                                                "app": "peer_client.py", "at": int(time.time() * 1000)}})
        print("-> policy.status")
    if args.pause or args.play:
        action = "pause" if args.pause else "play"
        cmd = c.send("media.command", {"key": None, "action": action, "origin": "user", "reason": "peer_client"})
        print(f"-> media.command {action} id={cmd['id']}")
        if not s.pump(lambda: cmd["id"] in s.results, timeout=6):
            print("no result!")
            rc = 1
        if args.dup:
            print(f"-> re-sending media.command id={cmd['id']} (duplicate; must not execute twice)")
            c.resend(cmd)
            s.pump(lambda: len(s.results.get(cmd["id"], [])) >= 2, timeout=6)
        want = "paused" if args.pause else "playing"
        if s.pump(lambda: (active_session(s.snapshot) or {}).get("status") == want, timeout=6):
            print(f"OK: snapshot shows status={want}")
        else:
            print(f"FAIL: status did not become {want}")
            rc = 1
    if args.artwork:
        a = active_session(s.snapshot)
        ref = (a or {}).get("artwork")
        if not ref:
            print("active session has no artwork")
            rc = 1
        else:
            c.send("artwork.get", {"hash": ref["hash"]})
            h = ref["hash"]

            def complete():
                e = s.blobs.get(h)
                return e is not None and sum(len(p) for p in e["parts"].values()) >= e["total"]

            if s.pump(complete, timeout=10):
                e = s.blobs[h]
                data = b"".join(e["parts"][o] for o in sorted(e["parts"]))
                ok = hashlib.sha256(data).hexdigest() == h
                with open(args.artwork, "wb") as f:
                    f.write(data)
                print(f"artwork {len(data)} bytes in {len(e['parts'])} chunk(s), sha256 {'OK' if ok else 'MISMATCH'} -> {args.artwork}")
                rc |= 0 if ok else 1
            else:
                print("artwork transfer incomplete")
                rc = 1
    if args.volume_set is not None or args.mute or args.unmute or args.delta:
        req = {}
        if args.volume_set is not None:
            req["level"] = args.volume_set
        if args.delta:
            req["delta"] = args.delta
        if args.mute or args.unmute:
            req["muted"] = bool(args.mute)
        msg = c.send("volume.set", req)
        print(f"-> volume.set {json.dumps(req)} id={msg['id']}")
        if s.pump(lambda: msg["id"] in s.peer_volume or msg["id"] in s.results, timeout=5) and msg["id"] in s.peer_volume:
            print(f"OK: phone volume is now {json.dumps(s.peer_volume[msg['id']])}")
        else:
            print("FAIL: no volume.state answer")
            rc = 1
    if args.handoff_play:
        # Act as the coordinating PC (§8.7): ask the phone to play a song in one of its apps.
        song = {"title": args.title, "artist": args.artist, "album": args.album,
                "durationMs": args.duration if args.duration else None}
        body = {"app": args.handoff_play, "song": song}
        if args.ids:
            body["ids"] = json.loads(args.ids)
        msg = c.send("handoff.play", body)
        print(f"-> handoff.play {json.dumps(body, ensure_ascii=False)} id={msg['id']}")
        if not s.pump(lambda: msg["id"] in s.handoff, timeout=args.handoff_wait):
            print("FAIL: no handoff.result")
            rc = 1
        else:
            r = s.handoff[msg["id"]]
            if r.get("ok") and r.get("state") == "playing":
                print(f"OK: {r.get('key')} plays the song")
                if args.seek is not None:
                    cmd = c.send("media.command", {"key": r.get("key"), "action": "seek", "positionMs": args.seek,
                                                   "origin": "user", "reason": "handoff"})
                    s.pump(lambda: cmd["id"] in s.results, timeout=6)
                    s.pump(timeout=3)
                    a = next((x for x in (s.snapshot or {}).get("sessions", []) if x["key"] == r.get("key")), None)
                    print(f"after seek: pos={a and a.get('positionMs')} status={a and a.get('status')}")
            else:
                print(f"handoff answered {json.dumps(r)}")
                rc = 0 if r.get("ok") else 1
    if args.audio:
        rc |= stream_audio(args, s)
    if args.listen:
        print(f"listening for {args.listen} s ...")
        s.pump(timeout=args.listen)
    if args.bye:
        c.send("bye", {"reason": "peer_client exit"})
    c.sock.close()
    state["next_id"] = c.next_id
    save_state(args.state, state)
    return rc


def stream_audio(args, s):
    """Windows->Android PCM stream (§9): offer, connect, streamId, AUDIO frames."""
    import math
    import subprocess
    c = s.c
    stream_id = secrets.token_bytes(16)
    offer = c.send("audio.offer", {"streamId": stream_id.hex(), "direction": "windows_to_android", "codec": "pcm_s16le",
                                   "sampleRate": 48000, "channels": 2, "frameMs": 10, "transport": "tcp"})
    deadline = time.time() + 6
    port = None
    while time.time() < deadline and port is None:
        c.sock.settimeout(max(0.1, deadline - time.time()))
        try:
            channel, data = c.read_record()
        except socket.timeout:
            break
        if channel != CONTROL:
            continue
        env = json.loads(data.decode())
        if env["type"] == "audio.answer" and env.get("re") == offer["id"]:
            print(f"<- audio.answer {json.dumps(env['body'])}")
            if not env["body"].get("ok"):
                return 1
            port = env["body"]["port"]
        elif env["type"] == "ping":
            c.send("pong", {"t": env["body"].get("t")}, re=env["id"])
    if port is None:
        print("no audio.answer")
        return 1
    if args.adb:
        subprocess.run([args.adb, "forward", f"tcp:{port}", f"tcp:{port}"], check=True, capture_output=True)
    a = socket.create_connection((args.host, port), timeout=10)
    a.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    a.sendall(stream_id)
    key = hmac256(c.key_c2s, b"audio key" + stream_id)
    iv = hmac256(c.key_c2s, b"audio iv" + stream_id)[:4]
    rec = Record(key, iv, 0x03)
    frames = int(args.audio * 100)
    phase = 0.0
    start = time.monotonic()
    for n in range(frames):
        pcm = bytearray()
        for _ in range(480):
            v = int(math.sin(phase) * 2500)
            pcm += struct.pack("<hh", v, v)
            phase += 2 * math.pi * 330.0 / 48000
        plain = struct.pack(">Q", int((time.monotonic() - start) * 1e6)) + bytes(pcm)
        body = rec.seal(plain)
        a.sendall(struct.pack(">I", len(body) + 1) + bytes([0x03]) + body)
        sleep_for = start + (n + 1) * 0.01 - time.monotonic()
        if sleep_for > 0:
            time.sleep(sleep_for)
    print(f"-> sent {frames} encrypted AUDIO frames ({args.audio:.1f} s of 48 kHz stereo PCM)")
    c.send("audio.stop", {"streamId": stream_id.hex(), "reason": "done"})
    a.close()
    return 0


def load_state(path, name):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    return {"id": secrets.token_hex(16), "name": name}


def save_state(path, state):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=47321)
    p.add_argument("--state", default="peer_client_state.json")
    p.add_argument("--name", default="peer_client.py")
    sub = p.add_subparsers(dest="cmd", required=True)
    pp = sub.add_parser("pair")
    pp.add_argument("--yes", action="store_true", help="accept the SAS without asking")
    ps = sub.add_parser("session")
    ps.add_argument("--pause", action="store_true")
    ps.add_argument("--play", action="store_true")
    ps.add_argument("--dup", action="store_true", help="re-send the command with the same id")
    ps.add_argument("--artwork", metavar="FILE")
    ps.add_argument("--lyrics", metavar="LRC_FILE")
    ps.add_argument("--translation", metavar="LRC_FILE", help="translation LRC sent with --lyrics (lyrics.push translation)")
    ps.add_argument("--policy", action="store_true", help="send a policy.status")
    ps.add_argument("--volume-set", type=float, metavar="LEVEL", help="set the phone's media volume (0..1)")
    ps.add_argument("--delta", type=int, default=0, metavar="STEPS", help="change the phone's volume by device steps")
    ps.add_argument("--mute", action="store_true")
    ps.add_argument("--no-reply", action="store_true", help="never answer media.command (tests timeouts)")
    ps.add_argument("--unmute", action="store_true")
    ps.add_argument("--audio", type=float, default=0, metavar="SECONDS", help="stream a test tone (audio receive must be enabled)")
    ps.add_argument("--adb", metavar="ADB_PATH", help="run 'adb forward' for the audio port")
    ps.add_argument("--demo", metavar="JPEG", help="act as a media source: send a Windows snapshot with this artwork")
    ps.add_argument("--resume", action="store_true", help="resume the stored sid")
    ps.add_argument("--listen", type=float, default=0, metavar="SECONDS")
    ps.add_argument("--bye", action="store_true")
    ps.add_argument("--handoff-play", metavar="APP", help="handoff.play: netease | qqmusic | spotify")
    ps.add_argument("--title", default="起风了")
    ps.add_argument("--artist", default="买辣椒也用券")
    ps.add_argument("--album", default="起风了")
    ps.add_argument("--duration", type=int, default=325000, metavar="MS")
    ps.add_argument("--ids", metavar="JSON", help='catalogue ids, e.g. {"qqmusic":"003hKob536i6FA"}')
    ps.add_argument("--seek", type=int, metavar="MS", help="after the handoff: seek the target (as the PC does)")
    ps.add_argument("--handoff-wait", type=float, default=100, metavar="SECONDS")
    args = p.parse_args()
    state = load_state(args.state, args.name)
    save_state(args.state, state)
    if args.cmd == "pair":
        return cmd_pair(args, state)
    return cmd_session(args, state)


if __name__ == "__main__":
    sys.exit(main())
