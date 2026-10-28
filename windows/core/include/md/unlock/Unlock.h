#pragma once

// Unlocking the PC with the phone (docs/unlock.md), the pure parts: sealing the Windows
// password with the key K the phone keeps, and the unlock.* message bodies.

#include "md/protocol/Envelope.h"
#include "md/util/Bytes.h"

#include <optional>
#include <string>
#include <string_view>

namespace md::unlock {

using json = nlohmann::json;

inline constexpr size_t kKeyBytes = 32;
inline constexpr size_t kNonceBytes = 12;
// The phone waits 60 s (request) / 120 s (enroll) for its user; the PC 10 s longer.
inline constexpr int64_t kRequestWaitMs = 70'000;
inline constexpr int64_t kEnrollWaitMs = 130'000;

bool isEnrollmentId(std::string_view s); // 32 lower-hex chars

// nonce || AES-256-GCM(K, nonce, aad, password) with
// aad = "MediaDuet-unlock-v1|" + enrollment + "|" + userSid.
Bytes seal(ByteView key, std::string_view enrollment, std::string_view userSid, std::string_view password);
// The password, or nullopt for a wrong key / tampered seal.
std::optional<std::string> open(ByteView key, std::string_view enrollment, std::string_view userSid, ByteView sealed);

json enrollBody(std::string_view enrollment, ByteView key, std::string_view pc);
json requestBody(std::string_view enrollment, std::string_view pc);
json revokeBody(std::string_view enrollment);

struct Result {
    bool ok = false;
    Bytes key;         // ok of an unlock.request: K
    std::string error; // !ok: the phone's error code ("internal" when it sent none)
};
// unlock.result; nullopt when malformed (a key, if present, must be 32 bytes).
std::optional<Result> parseResult(const json& body);
// unlock.revoke: the enrollment id.
std::optional<std::string> parseRevoke(const json& body);

// The UI's unlock.enable error for a phone's unlock.result error code (docs/unlock.md).
std::string enableError(std::string_view phoneCode);

} // namespace md::unlock
