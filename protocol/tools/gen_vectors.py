#!/usr/bin/env python3
"""Reference implementation of the MediaDuet v1 key schedule.

Generates deterministic test vectors (protocol/test-vectors/*.json) with the
independent `cryptography` package. The C++ (Windows) and Kotlin (Android)
implementations must reproduce every value in their unit tests.

Usage:  python -I protocol/tools/gen_vectors.py
"""
import base64
import hashlib
import hmac
import json
import os
import struct

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "test-vectors")


def hkdf_extract(salt: bytes, ikm: bytes) -> bytes:
    return hmac.new(salt, ikm, hashlib.sha256).digest()


def hkdf_expand(prk: bytes, info: bytes, length: int) -> bytes:
    out, t, i = b"", b"", 1
    while len(out) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        out += t
        i += 1
    return out[:length]


def hmac256(key: bytes, data: bytes) -> bytes:
    return hmac.new(key, data, hashlib.sha256).digest()


def keypair(d_hex: str):
    priv = ec.derive_private_key(int(d_hex, 16), ec.SECP256R1())
    pub = priv.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint)
    return priv, pub


def ecdh(priv, peer_pub: bytes) -> bytes:
    peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), peer_pub)
    return priv.exchange(ec.ECDH(), peer)


def frame(ftype: int, body: bytes) -> bytes:
    return struct.pack(">I", 1 + len(body)) + bytes([ftype]) + body


def seal(key: bytes, iv4: bytes, seq: int, ftype: int, plaintext: bytes) -> bytes:
    nonce = iv4 + struct.pack(">Q", seq)
    aad = b"MDv1" + bytes([ftype])
    return AESGCM(key).encrypt(nonce, plaintext, aad)


def track_id(title, artist, album, duration_ms):
    dur = "" if duration_ms is None else str((int(duration_ms) + 500) // 1000)
    data = "\x1f".join([title, artist, album, dur]).encode("utf-8")
    return hashlib.sha256(data).hexdigest()[:16]


def main():
    os.makedirs(OUT, exist_ok=True)

    id_c = "0123456789abcdef0123456789abcdef"  # Windows (client)
    id_s = "fedcba9876543210fedcba9876543210"  # Android (server)
    d_c = "1f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a79881f2e3d4c5b6a7988"
    d_s = "6a5b4c3d2e1f00116a5b4c3d2e1f00116a5b4c3d2e1f00116a5b4c3d2e1f0011"
    n_c = bytes(range(0x00, 0x20))
    n_s = bytes(range(0x20, 0x40))

    priv_c, epk_c = keypair(d_c)
    priv_s, epk_s = keypair(d_s)
    z = ecdh(priv_c, epk_s)
    assert z == ecdh(priv_s, epk_c)

    # ---- pairing ----
    th_p = hashlib.sha256(b"MediaDuet-pair-v1" + n_c + n_s + epk_c + epk_s
                          + id_c.encode() + id_s.encode()).digest()
    prk_p = hkdf_extract(th_p, z)
    sas = struct.unpack(">I", hkdf_expand(prk_p, b"sas", 4))[0] % 1_000_000
    k = hkdf_expand(prk_p, b"pairing key", 32)
    mac_confirm_c = hmac256(k, b"confirm" + b"C" + th_p)
    mac_confirm_s = hmac256(k, b"confirm" + b"S" + th_p)
    # QR pairing (§3.5): the one-time secret T from the QR code replaces the SAS comparison.
    qr_t = bytes(range(0x40, 0x50))
    mac_qr_c = hmac256(qr_t, b"qr confirm" + b"C" + th_p)
    mac_qr_s = hmac256(qr_t, b"qr confirm" + b"S" + th_p)

    # ---- session (reuses the same ephemeral keys for brevity) ----
    th_s = hashlib.sha256(b"MediaDuet-session-v1" + n_c + n_s + epk_c + epk_s
                          + id_c.encode() + id_s.encode()).digest()
    mac_server = hmac256(k, b"server" + th_s)
    mac_client = hmac256(k, b"client" + th_s)
    prk_s = hkdf_extract(k, z)
    key_c2s = hkdf_expand(prk_s, b"c2s key" + th_s, 32)
    key_s2c = hkdf_expand(prk_s, b"s2c key" + th_s, 32)
    iv_c2s = hkdf_expand(prk_s, b"c2s iv" + th_s, 4)
    iv_s2c = hkdf_expand(prk_s, b"s2c iv" + th_s, 4)

    ctrl = json.dumps({"v": 1, "type": "ping", "id": 1, "ts": 1760000000000,
                       "src": id_c, "sid": "00112233445566778899aabbccddeeff",
                       "body": {"t": 1234}}, separators=(",", ":")).encode()
    rec0_plain = b"\x01" + ctrl
    rec0 = seal(key_c2s, iv_c2s, 0, 0x02, rec0_plain)
    rec1_plain = b"\x01" + b'{"hello":"\xe4\xb8\x96\xe7\x95\x8c"}'
    rec1 = seal(key_c2s, iv_c2s, 1, 0x02, rec1_plain)
    rec_s0_plain = b"\x01" + b"{}"
    rec_s0 = seal(key_s2c, iv_s2c, 0, 0x02, rec_s0_plain)

    stream_id = bytes.fromhex("a1a2a3a4a5a6a7a8a9aaabacadaeafb0")
    audio_key = hmac256(key_c2s, b"audio key" + stream_id)
    audio_iv = hmac256(key_c2s, b"audio iv" + stream_id)[:4]
    audio_plain = struct.pack(">Q", 123456789) + bytes([1, 0, 2, 0, 3, 0, 4, 0])
    audio_rec = seal(audio_key, audio_iv, 0, 0x03, audio_plain)

    blob = bytes(range(256)) * 4
    blob_hash = hashlib.sha256(blob).digest()
    blob_chunk = b"\x02" + blob_hash + struct.pack(">II", len(blob), 0) + blob[:512]

    vectors = {
        "description": "MediaDuet protocol v1 crypto vectors. Hex unless noted.",
        "ids": {"client": id_c, "server": id_s},
        "ephemeral": {
            "client_private": d_c, "client_public": epk_c.hex(),
            "server_private": d_s, "server_public": epk_s.hex(),
            "client_nonce": n_c.hex(), "server_nonce": n_s.hex(),
            "shared_secret": z.hex(),
        },
        "pairing": {
            "transcript_hash": th_p.hex(), "prk": prk_p.hex(),
            "sas": f"{sas:06d}", "pairing_key": k.hex(),
            "confirm_mac_client": mac_confirm_c.hex(),
            "confirm_mac_server": mac_confirm_s.hex(),
            "qr_token": qr_t.hex(),
            "qr_token_b64url": base64.urlsafe_b64encode(qr_t).rstrip(b"=").decode(),
            "qr_confirm_mac_client": mac_qr_c.hex(),
            "qr_confirm_mac_server": mac_qr_s.hex(),
        },
        "session": {
            "pairing_key": k.hex(),
            "transcript_hash": th_s.hex(),
            "mac_server": mac_server.hex(), "mac_client": mac_client.hex(),
            "prk": prk_s.hex(),
            "key_c2s": key_c2s.hex(), "key_s2c": key_s2c.hex(),
            "iv_c2s": iv_c2s.hex(), "iv_s2c": iv_s2c.hex(),
        },
        "records": [
            {"dir": "c2s", "seq": 0, "frame_type": 2, "plaintext": rec0_plain.hex(),
             "ciphertext": rec0.hex(), "frame": frame(0x02, rec0).hex()},
            {"dir": "c2s", "seq": 1, "frame_type": 2, "plaintext": rec1_plain.hex(),
             "ciphertext": rec1.hex(), "frame": frame(0x02, rec1).hex()},
            {"dir": "s2c", "seq": 0, "frame_type": 2, "plaintext": rec_s0_plain.hex(),
             "ciphertext": rec_s0.hex(), "frame": frame(0x02, rec_s0).hex()},
        ],
        "audio": {
            "stream_id": stream_id.hex(), "base": "key_c2s",
            "audio_key": audio_key.hex(), "audio_iv": audio_iv.hex(),
            "seq": 0, "plaintext": audio_plain.hex(), "ciphertext": audio_rec.hex(),
        },
        "blob": {
            "content_sha256": blob_hash.hex(), "total": len(blob),
            "first_chunk_plaintext": blob_chunk.hex(),
        },
        "handshake_frame": {
            "json": '{"v":1,"type":"auth","id":3,"ts":0,"src":"' + id_c + '","sid":"","body":{"mac":"AA=="}}',
            "frame": frame(0x01, ('{"v":1,"type":"auth","id":3,"ts":0,"src":"' + id_c
                                  + '","sid":"","body":{"mac":"AA=="}}').encode()).hex(),
        },
        "track_ids": [
            {"title": "晴天", "artist": "周杰伦", "album": "叶惠美", "durationMs": 269000,
             "trackId": track_id("晴天", "周杰伦", "叶惠美", 269000)},
            {"title": "Song", "artist": "", "album": "", "durationMs": None,
             "trackId": track_id("Song", "", "", None)},
            {"title": "Round", "artist": "A", "album": "B", "durationMs": 1499,
             "trackId": track_id("Round", "A", "B", 1499)},
            {"title": "Half", "artist": "A", "album": "B", "durationMs": 2500,
             "trackId": track_id("Half", "A", "B", 2500)},
        ],
    }
    path = os.path.join(OUT, "crypto-v1.json")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(vectors, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("wrote", os.path.normpath(path), "SAS", vectors["pairing"]["sas"])


if __name__ == "__main__":
    main()
