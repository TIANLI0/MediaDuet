#include "md/protocol/Envelope.h"

#include "md/util/Text.h"

namespace md::proto {

bool isValidDeviceId(std::string_view s) {
    if (s.size() != 32) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

bool isValidType(std::string_view s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    return true;
}

std::string Envelope::serialize() const {
    json j = {{"v", v}, {"type", type}, {"id", id}, {"ts", ts}, {"src", src}, {"sid", sid}, {"body", body}};
    if (re) j["re"] = *re;
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

static bool fail(std::string* err, const char* why) {
    if (err) *err = why;
    return false;
}

std::optional<Envelope> Envelope::parse(std::string_view text, std::string* err) {
    if (!text::isValidUtf8(text)) {
        fail(err, "invalid utf-8");
        return std::nullopt;
    }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        fail(err, "not a json object");
        return std::nullopt;
    }
    Envelope e;
    auto v = j.find("v");
    if (v == j.end() || !v->is_number_integer() || v->get<int64_t>() < 1 || v->get<int64_t>() > 1000) {
        fail(err, "bad v");
        return std::nullopt;
    }
    e.v = v->get<int>();
    auto t = j.find("type");
    if (t == j.end() || !t->is_string() || !isValidType(t->get_ref<const std::string&>())) {
        fail(err, "bad type");
        return std::nullopt;
    }
    e.type = t->get<std::string>();
    auto id = j.find("id");
    if (id == j.end() || !id->is_number_unsigned() || id->get<uint64_t>() < 1 || id->get<uint64_t>() > kMaxMessageId) {
        fail(err, "bad id");
        return std::nullopt;
    }
    e.id = id->get<uint64_t>();
    auto ts = j.find("ts");
    if (ts == j.end() || !ts->is_number_integer()) {
        fail(err, "bad ts");
        return std::nullopt;
    }
    e.ts = ts->get<int64_t>();
    auto src = j.find("src");
    if (src == j.end() || !src->is_string() || !isValidDeviceId(src->get_ref<const std::string&>())) {
        fail(err, "bad src");
        return std::nullopt;
    }
    e.src = src->get<std::string>();
    auto sid = j.find("sid");
    if (sid == j.end() || !sid->is_string()) {
        fail(err, "bad sid");
        return std::nullopt;
    }
    e.sid = sid->get<std::string>();
    if (!e.sid.empty() && !isValidDeviceId(e.sid)) {
        fail(err, "bad sid format");
        return std::nullopt;
    }
    auto re = j.find("re");
    if (re != j.end() && !re->is_null()) {
        if (!re->is_number_unsigned() || re->get<uint64_t>() > kMaxMessageId) {
            fail(err, "bad re");
            return std::nullopt;
        }
        e.re = re->get<uint64_t>();
    }
    auto body = j.find("body");
    if (body == j.end() || !body->is_object()) {
        fail(err, "bad body");
        return std::nullopt;
    }
    e.body = std::move(*body);
    return e;
}

} // namespace md::proto
