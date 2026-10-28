#include "md/unlock/BootRecord.h"
#include "md/unlock/Unlock.h"
#include "md/util/Text.h"

namespace md::unlock {
bool safeSid(std::string_view s) {
    if (!s.starts_with("S-1-") || s.size() > 184 || s.size() < 7 || s.back() == '-') return false;
    bool digit = false;
    for (char c : s.substr(4)) {
        if (c == '-') { if (!digit) return false; digit = false; }
        else if (c >= '0' && c <= '9') digit = true;
        else return false;
    }
    return digit;
}
bool bootSessionAllowed(uint32_t client, uint32_t console, bool known, bool user, bool locked, bool sameUser, bool host) {
    return client != 0 && console != UINT32_MAX && client == console && known &&
           (!user || (locked && sameUser && !host));
}
nlohmann::json bootRecordJson(const BootRecord& r) {
    nlohmann::json endpoints = nlohmann::json::array();
    for (const auto& e : r.peer.manualEndpoints) endpoints.push_back({{"host", e.host}, {"port", e.port}});
    return {{"v", 2}, {"sid", r.userSid}, {"enrollment", r.enrollment}, {"seal", base64Encode(r.sealed)},
        {"self", proto::toJson(r.self)}, {"peer", {{"id", r.peer.id}, {"name", r.peer.name},
        {"key", base64Encode(r.peer.key)}, {"bt", r.peer.btAddress}, {"endpoints", endpoints}}},
        {"lang", r.language}, {"stale", r.stale}, {"lastUnlockAt", r.lastUnlockAt}};
}
std::optional<BootRecord> parseBootRecord(const nlohmann::json& j) {
    try {
        if (!j.is_object() || j.at("v") != 2) return {};
        BootRecord r;
        r.userSid = j.at("sid").get<std::string>();
        r.enrollment = j.at("enrollment").get<std::string>();
        auto self = proto::deviceInfoFromJson(j.at("self"));
        auto sealed = base64Decode(j.at("seal").get<std::string>());
        const auto& peer = j.at("peer");
        auto key = proto::getB64(peer, "key", 32);
        r.peer.id = peer.at("id").get<std::string>();
        r.peer.name = peer.at("name").get<std::string>();
        r.peer.platform = "android";
        r.peer.btAddress = peer.value("bt", uint64_t(0));
        r.language = j.value("lang", std::string("en-US"));
        r.stale = j.value("stale", false);
        r.lastUnlockAt = j.value("lastUnlockAt", int64_t(0));
        if (!safeSid(r.userSid) || !isEnrollmentId(r.enrollment) || !self || self->platform != "windows" ||
            !sealed || sealed->size() < 28 || sealed->size() > 4096 || !key ||
            !proto::isValidDeviceId(r.peer.id) || r.peer.id == self->id || text::codePoints(r.peer.name) > 64 ||
            r.peer.btAddress > 0xffffffffffffULL || (r.language != "en-US" && r.language != "zh-CN")) return {};
        r.self = std::move(*self); r.sealed = std::move(*sealed); r.peer.key = std::move(*key);
        const auto& eps = peer.at("endpoints");
        if (!eps.is_array() || eps.size() > 16) return {};
        for (const auto& ep : eps) {
            auto host = ep.at("host").get<std::string>();
            int port = ep.at("port").get<int>();
            if (host.empty() || host.size() > 253 || port <= 0 || port > 65535 ||
                host.find_first_of("\r\n\t /\\\"\0", 0, 9) != std::string::npos) return {};
            r.peer.manualEndpoints.push_back({host, static_cast<uint16_t>(port), "boot"});
        }
        return r;
    } catch (...) { return {}; }
}
}
