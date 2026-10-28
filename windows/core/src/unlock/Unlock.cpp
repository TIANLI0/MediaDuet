#include "md/unlock/Unlock.h"

#include "md/crypto/Crypto.h"
#include "md/protocol/Messages.h"
#include "md/util/Text.h"
#include <stdexcept>

namespace md::unlock {

namespace {

Bytes aad(std::string_view enrollment, std::string_view userSid) {
    std::string s = "MediaDuet-unlock-v1|";
    s.append(enrollment).append("|").append(userSid);
    return toBytes(s);
}

} // namespace

bool isEnrollmentId(std::string_view s) { return proto::isValidDeviceId(s); }

Bytes seal(ByteView key, std::string_view enrollment, std::string_view userSid, std::string_view password) {
    if (key.size() != kKeyBytes || !isEnrollmentId(enrollment)) throw std::invalid_argument("invalid unlock enrollment key or id");
    Bytes nonce = crypto::randomBytes(kNonceBytes);
    Bytes ct = crypto::AesGcm(key).seal(nonce, aad(enrollment, userSid), view(password));
    return concat({nonce, ct});
}

std::optional<std::string> open(ByteView key, std::string_view enrollment, std::string_view userSid, ByteView sealed) {
    if (key.size() != kKeyBytes || !isEnrollmentId(enrollment) || sealed.size() < kNonceBytes + 16) return std::nullopt;
    auto pt = crypto::AesGcm(key).open(sealed.first(kNonceBytes), aad(enrollment, userSid), sealed.subspan(kNonceBytes));
    if (!pt) return std::nullopt;
    std::string out = toString(*pt);
    secureZero(*pt);
    return out;
}

json enrollBody(std::string_view enrollment, ByteView key, std::string_view pc) {
    return {{"enrollment", enrollment}, {"key", base64Encode(key)}, {"pc", text::truncate(pc, 64)}};
}

json requestBody(std::string_view enrollment, std::string_view pc) {
    return {{"enrollment", enrollment}, {"pc", text::truncate(pc, 64)}};
}

json revokeBody(std::string_view enrollment) { return {{"enrollment", enrollment}}; }

std::optional<Result> parseResult(const json& body) {
    if (!body.is_object()) return std::nullopt;
    auto ok = body.find("ok");
    if (ok == body.end() || !ok->is_boolean()) return std::nullopt;
    Result r;
    r.ok = ok->get<bool>();
    if (body.contains("key")) {
        auto k = proto::getB64(body, "key", kKeyBytes);
        if (!k) return std::nullopt;
        r.key = std::move(*k);
    }
    if (!r.ok) {
        r.error = "internal";
        auto e = body.find("error");
        if (e != body.end() && e->is_object()) {
            auto c = e->find("code");
            if (c != e->end() && c->is_string() && c->get_ref<const std::string&>().size() <= 64) r.error = c->get<std::string>();
        }
    }
    return r;
}

std::optional<std::string> parseRevoke(const json& body) {
    if (!body.is_object()) return std::nullopt;
    auto e = body.find("enrollment");
    if (e == body.end() || !e->is_string() || !isEnrollmentId(e->get_ref<const std::string&>())) return std::nullopt;
    return e->get<std::string>();
}

std::string enableError(std::string_view phoneCode) {
    if (phoneCode == "denied") return "phone_denied";
    if (phoneCode == "timeout") return "phone_timeout";
    if (phoneCode == "no_auth") return "phone_no_auth";
    if (phoneCode == "busy") return "busy";
    return "phone_failed";
}

} // namespace md::unlock
