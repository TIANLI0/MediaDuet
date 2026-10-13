#include "md/crypto/Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace md::crypto {
namespace {

void check(NTSTATUS st, const char* what) {
    if (!BCRYPT_SUCCESS(st)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s failed (NTSTATUS 0x%08lx)", what, static_cast<unsigned long>(st));
        throw CryptoError(buf);
    }
}

struct Providers {
    BCRYPT_ALG_HANDLE sha256 = nullptr;
    BCRYPT_ALG_HANDLE hmac = nullptr;
    BCRYPT_ALG_HANDLE ecdh = nullptr;
    BCRYPT_ALG_HANDLE aesGcm = nullptr;
};

const Providers& providers() {
    static Providers p;
    static std::once_flag once;
    std::call_once(once, [] {
        check(BCryptOpenAlgorithmProvider(&p.sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "open SHA256");
        check(BCryptOpenAlgorithmProvider(&p.hmac, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
              "open HMAC");
        check(BCryptOpenAlgorithmProvider(&p.ecdh, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0), "open ECDH");
        check(BCryptOpenAlgorithmProvider(&p.aesGcm, BCRYPT_AES_ALGORITHM, nullptr, 0), "open AES");
        check(BCryptSetProperty(p.aesGcm, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(
                                    const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                sizeof(BCRYPT_CHAIN_MODE_GCM), 0),
              "set GCM");
    });
    return p;
}

constexpr ULONG kCoord = 32;

} // namespace

Bytes randomBytes(size_t n) {
    Bytes b(n);
    if (n) check(BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
                 "BCryptGenRandom");
    return b;
}

std::string randomHex(size_t nBytes) { return hexEncode(randomBytes(nBytes)); }

Bytes sha256(ByteView data) {
    Bytes out(32);
    check(BCryptHash(providers().sha256, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), 32),
          "SHA256");
    return out;
}

Bytes hmacSha256(ByteView key, ByteView data) {
    Bytes out(32);
    // BCryptHash rejects a zero-length HMAC key; RFC 2104 treats it as 64 zero bytes.
    static const uint8_t kZero[64] = {};
    PUCHAR k = key.empty() ? const_cast<PUCHAR>(kZero) : const_cast<PUCHAR>(key.data());
    ULONG kl = key.empty() ? 64u : static_cast<ULONG>(key.size());
    check(BCryptHash(providers().hmac, k, kl, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()),
                     out.data(), 32),
          "HMAC");
    return out;
}

Bytes hkdfExtract(ByteView salt, ByteView ikm) { return hmacSha256(salt, ikm); }

Bytes hkdfExpand(ByteView prk, ByteView info, size_t length) {
    if (length > 255 * 32) throw CryptoError("HKDF length too large");
    Bytes out, t;
    uint8_t counter = 1;
    while (out.size() < length) {
        Bytes msg = t;
        append(msg, info);
        msg.push_back(counter++);
        t = hmacSha256(prk, msg);
        append(out, t);
    }
    out.resize(length);
    return out;
}

bool constantTimeEqual(ByteView a, ByteView b) {
    if (a.size() != b.size()) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = diff | (a[i] ^ b[i]);
    return diff == 0;
}

// ---------------------------------------------------------------- ECDH

EcdhKey EcdhKey::generate() {
    EcdhKey k;
    BCRYPT_KEY_HANDLE h = nullptr;
    check(BCryptGenerateKeyPair(providers().ecdh, &h, 256, 0), "ECDH generate");
    k.handle_ = h;
    check(BCryptFinalizeKeyPair(h, 0), "ECDH finalize");
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoord];
    ULONG cb = 0;
    check(BCryptExportKey(h, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &cb, 0), "ECDH export");
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    if (hdr->cbKey != kCoord) throw CryptoError("unexpected ECC key size");
    k.pub_.reserve(65);
    k.pub_.push_back(0x04);
    k.pub_.insert(k.pub_.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoord);
    return k;
}

EcdhKey EcdhKey::fromPrivate(ByteView d, ByteView pub) {
    if (d.size() != kCoord || pub.size() != 65 || pub[0] != 0x04) throw CryptoError("bad private key encoding");
    EcdhKey k;
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 3 * kCoord];
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PRIVATE_P256_MAGIC;
    hdr->cbKey = kCoord;
    std::memcpy(blob + sizeof(*hdr), pub.data() + 1, 2 * kCoord);
    std::memcpy(blob + sizeof(*hdr) + 2 * kCoord, d.data(), kCoord);
    BCRYPT_KEY_HANDLE h = nullptr;
    NTSTATUS st = BCryptImportKeyPair(providers().ecdh, nullptr, BCRYPT_ECCPRIVATE_BLOB, &h, blob, sizeof(blob), 0);
    SecureZeroMemory(blob, sizeof(blob));
    check(st, "ECDH import private");
    k.handle_ = h;
    k.pub_.assign(pub.begin(), pub.end());
    return k;
}

EcdhKey::EcdhKey(EcdhKey&& o) noexcept : handle_(o.handle_), pub_(std::move(o.pub_)) { o.handle_ = nullptr; }
EcdhKey& EcdhKey::operator=(EcdhKey&& o) noexcept {
    if (this != &o) {
        if (handle_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle_));
        handle_ = o.handle_;
        pub_ = std::move(o.pub_);
        o.handle_ = nullptr;
    }
    return *this;
}
EcdhKey::~EcdhKey() {
    if (handle_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(handle_));
}

Bytes EcdhKey::agree(ByteView peer) const {
    if (peer.size() != 65 || peer[0] != 0x04) throw CryptoError("bad peer public key encoding");
    uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 2 * kCoord];
    auto* hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob);
    hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    hdr->cbKey = kCoord;
    std::memcpy(blob + sizeof(*hdr), peer.data() + 1, 2 * kCoord);
    BCRYPT_KEY_HANDLE peerKey = nullptr;
    // CNG validates that the point lies on the curve during import.
    check(BCryptImportKeyPair(providers().ecdh, nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey, blob, sizeof(blob), 0),
          "ECDH import peer");
    BCRYPT_SECRET_HANDLE secret = nullptr;
    NTSTATUS st = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(handle_), peerKey, &secret, 0);
    BCryptDestroyKey(peerKey);
    check(st, "ECDH agreement");
    Bytes z(kCoord);
    ULONG cb = 0;
    st = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(), kCoord, &cb, 0);
    BCryptDestroySecret(secret);
    check(st, "ECDH raw secret");
    if (cb != kCoord) throw CryptoError("unexpected shared secret size");
    // BCRYPT_KDF_RAW_SECRET returns the secret in little-endian order.
    std::reverse(z.begin(), z.end());
    return z;
}

// ---------------------------------------------------------------- AES-GCM

AesGcm::AesGcm(ByteView key32) {
    if (key32.size() != 32) throw CryptoError("AES-256 key must be 32 bytes");
    BCRYPT_KEY_HANDLE h = nullptr;
    check(BCryptGenerateSymmetricKey(providers().aesGcm, &h, nullptr, 0, const_cast<PUCHAR>(key32.data()), 32, 0),
          "AES key");
    key_ = h;
}
AesGcm::AesGcm(AesGcm&& o) noexcept : key_(o.key_) { o.key_ = nullptr; }
AesGcm& AesGcm::operator=(AesGcm&& o) noexcept {
    if (this != &o) {
        if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
        key_ = o.key_;
        o.key_ = nullptr;
    }
    return *this;
}
AesGcm::~AesGcm() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

Bytes AesGcm::seal(ByteView nonce, ByteView aad, ByteView pt) const {
    if (nonce.size() != 12) throw CryptoError("GCM nonce must be 12 bytes");
    Bytes out(pt.size() + 16);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = 12;
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = out.data() + pt.size();
    info.cbTag = 16;
    ULONG cb = 0;
    check(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(pt.data()),
                        static_cast<ULONG>(pt.size()), &info, nullptr, 0, out.data(), static_cast<ULONG>(pt.size()),
                        &cb, 0),
          "GCM encrypt");
    return out;
}

std::optional<Bytes> AesGcm::open(ByteView nonce, ByteView aad, ByteView ct) const {
    if (nonce.size() != 12 || ct.size() < 16) return std::nullopt;
    size_t n = ct.size() - 16;
    Bytes out(n);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce.data());
    info.cbNonce = 12;
    info.pbAuthData = const_cast<PUCHAR>(aad.data());
    info.cbAuthData = static_cast<ULONG>(aad.size());
    info.pbTag = const_cast<PUCHAR>(ct.data() + n);
    info.cbTag = 16;
    ULONG cb = 0;
    NTSTATUS st = BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), const_cast<PUCHAR>(ct.data()),
                                static_cast<ULONG>(n), &info, nullptr, 0, out.data(), static_cast<ULONG>(n), &cb, 0);
    if (!BCRYPT_SUCCESS(st)) return std::nullopt;
    return out;
}

} // namespace md::crypto
