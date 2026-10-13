#pragma once

// Thin wrappers over Windows CNG (BCrypt). Only standard primitives are used:
// SHA-256, HMAC-SHA-256, HKDF (RFC 5869), ECDH P-256, AES-256-GCM.

#include "md/util/Bytes.h"

#include <memory>
#include <optional>
#include <stdexcept>

namespace md::crypto {

struct CryptoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Bytes randomBytes(size_t n);
std::string randomHex(size_t nBytes);

Bytes sha256(ByteView data);
Bytes hmacSha256(ByteView key, ByteView data);
Bytes hkdfExtract(ByteView salt, ByteView ikm);
Bytes hkdfExpand(ByteView prk, ByteView info, size_t length);
bool constantTimeEqual(ByteView a, ByteView b);

// Ephemeral or test ECDH key on NIST P-256.
class EcdhKey {
public:
    static EcdhKey generate();
    // Imports a private scalar (32-byte big-endian d) with its public point.
    static EcdhKey fromPrivate(ByteView d, ByteView publicUncompressed65);
    EcdhKey(EcdhKey&&) noexcept;
    EcdhKey& operator=(EcdhKey&&) noexcept;
    ~EcdhKey();

    // SEC1 uncompressed point (0x04 || X || Y), 65 bytes.
    const Bytes& publicKey() const { return pub_; }
    // Shared secret: 32-byte big-endian X coordinate. Throws on invalid peer key.
    Bytes agree(ByteView peerPublic65) const;

private:
    EcdhKey() = default;
    void* handle_ = nullptr; // BCRYPT_KEY_HANDLE
    Bytes pub_;
};

// AES-256-GCM with a fixed key. AAD and nonces are supplied per call.
class AesGcm {
public:
    explicit AesGcm(ByteView key32);
    AesGcm(AesGcm&&) noexcept;
    AesGcm& operator=(AesGcm&&) noexcept;
    ~AesGcm();
    // Returns ciphertext || 16-byte tag.
    Bytes seal(ByteView nonce12, ByteView aad, ByteView plaintext) const;
    // Returns nullopt on authentication failure.
    std::optional<Bytes> open(ByteView nonce12, ByteView aad, ByteView ciphertextAndTag) const;

private:
    void* key_ = nullptr; // BCRYPT_KEY_HANDLE
};

} // namespace md::crypto
