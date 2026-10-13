#include "md/protocol/Messages.h"

#include "md/util/Bytes.h"
#include "md/util/Text.h"

namespace md::proto {

json toJson(const DeviceInfo& d) {
    return {{"id", d.id}, {"name", text::truncate(d.name, 64)}, {"platform", d.platform},
            {"model", text::truncate(d.model, 128)}, {"os", text::truncate(d.os, 64)}, {"app", text::truncate(d.app, 32)}};
}

std::optional<DeviceInfo> deviceInfoFromJson(const json& j, std::string* err) {
    auto fail = [&](const char* m) -> std::optional<DeviceInfo> {
        if (err) *err = m;
        return std::nullopt;
    };
    if (!j.is_object()) return fail("device not object");
    DeviceInfo d;
    auto str = [&](const char* k, size_t maxCp, std::string& out, bool required) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return !required;
        if (!it->is_string()) return false;
        if (text::codePoints(it->get_ref<const std::string&>()) > maxCp) return false;
        out = it->get<std::string>();
        return true;
    };
    if (!str("id", 32, d.id, true) || !isValidDeviceId(d.id)) return fail("device.id");
    if (!str("name", 64, d.name, true)) return fail("device.name");
    if (!str("platform", 16, d.platform, true)) return fail("device.platform");
    if (!str("model", 128, d.model, false) || !str("os", 64, d.os, false) || !str("app", 32, d.app, false))
        return fail("device fields");
    return d;
}

json errorBody(std::string_view code, std::string_view message) {
    json j = {{"code", code}};
    if (!message.empty()) j["message"] = text::truncate(message, 512);
    return j;
}

std::optional<Bytes> getB64(const json& body, const char* key, size_t exactLen) {
    auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return std::nullopt;
    const auto& s = it->get_ref<const std::string&>();
    if (s.size() > 4 * ((exactLen + 2) / 3)) return std::nullopt;
    auto b = base64Decode(s);
    if (!b || b->size() != exactLen) return std::nullopt;
    return b;
}

} // namespace md::proto
