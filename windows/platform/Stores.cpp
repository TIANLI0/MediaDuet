#include "Stores.h"

#include "SecureStore.h"

#include "md/crypto/Crypto.h"
#include "md/util/Log.h"
#include "md/util/Text.h"

#include <windows.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace md::platform {

using nlohmann::json;
namespace fs = std::filesystem;

fs::path appDataDir() {
    PWSTR p = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &p))) {
        base = p;
        CoTaskMemFree(p);
    } else {
        base = fs::temp_directory_path();
    }
    auto dir = base / L"MediaDuet";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

fs::path cacheDir() {
    auto d = appDataDir() / L"cache";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

static std::optional<json> readJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

static bool writeJson(const fs::path& p, const json& j) {
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << j.dump(2);
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

static std::string computerName() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 64];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameExW(ComputerNamePhysicalDnsHostname, buf, &n)) return text::toUtf8(std::wstring_view(buf, n));
    return "Windows PC";
}

Identity loadOrCreateIdentity() {
    auto path = appDataDir() / L"identity.json";
    Identity id;
    if (auto j = readJson(path); j && j->is_object()) {
        id.id = j->value("id", std::string());
        id.name = j->value("name", std::string());
    }
    if (id.id.size() != 32) {
        id.id = crypto::randomHex(16);
        log::info("store", "created new device identity");
    }
    if (id.name.empty()) id.name = text::truncate(computerName(), 64);
    writeJson(path, {{"id", id.id}, {"name", id.name}});
    return id;
}

void saveIdentityName(const std::string& name) {
    auto path = appDataDir() / L"identity.json";
    auto j = readJson(path).value_or(json::object());
    j["name"] = text::truncate(name, 64);
    writeJson(path, j);
}

std::string formatBtAddress(uint64_t a) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", unsigned((a >> 40) & 0xFF),
                  unsigned((a >> 32) & 0xFF), unsigned((a >> 24) & 0xFF), unsigned((a >> 16) & 0xFF),
                  unsigned((a >> 8) & 0xFF), unsigned(a & 0xFF));
    return buf;
}

std::optional<uint64_t> parseBtAddress(const std::string& s) {
    uint64_t v = 0;
    int digits = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c == ':' || c == '-') continue;
        else return std::nullopt;
        v = (v << 4) | static_cast<uint64_t>(d);
        ++digits;
    }
    if (digits != 12) return std::nullopt;
    return v;
}

void DeviceStore::load() {
    devices_.clear();
    auto j = readJson(appDataDir() / L"devices.json");
    if (!j || !j->is_object() || !(*j)["devices"].is_array()) return;
    for (auto& d : (*j)["devices"]) {
        if (!d.is_object()) continue;
        DeviceRecord r;
        r.id = d.value("id", std::string());
        r.name = d.value("name", std::string());
        r.platform = d.value("platform", std::string("android"));
        if (auto a = parseBtAddress(d.value("btAddress", std::string()))) r.btAddress = *a;
        r.pairedAt = d.value("pairedAt", int64_t(0));
        r.lastSeen = d.value("lastSeen", int64_t(0));
        if (d.contains("manual") && d["manual"].is_array())
            for (auto& e : d["manual"])
                if (e.is_object())
                    r.manualEndpoints.push_back(
                        {e.value("host", std::string()), static_cast<uint16_t>(e.value("port", 0)), "manual"});
        auto blob = base64Decode(d.value("key", std::string()));
        if (!blob) continue;
        auto key = unprotect(*blob);
        if (!key || key->size() != 32) {
            log::warn("store", "cannot decrypt pairing key for {} (different Windows user?)", r.name);
            continue;
        }
        r.key = std::move(*key);
        if (r.id.size() == 32) devices_.push_back(std::move(r));
    }
}

bool DeviceStore::save() const {
    json arr = json::array();
    for (auto& r : devices_) {
        auto blob = protect(r.key);
        if (!blob) {
            log::error("store", "DPAPI protect failed");
            return false;
        }
        json manual = json::array();
        for (auto& e : r.manualEndpoints) manual.push_back({{"host", e.host}, {"port", e.port}});
        arr.push_back({{"id", r.id},
                       {"name", r.name},
                       {"platform", r.platform},
                       {"btAddress", r.btAddress ? formatBtAddress(r.btAddress) : ""},
                       {"manual", manual},
                       {"pairedAt", r.pairedAt},
                       {"lastSeen", r.lastSeen},
                       {"key", base64Encode(*blob)}});
    }
    return writeJson(appDataDir() / L"devices.json", {{"version", 1}, {"devices", arr}});
}

DeviceRecord* DeviceStore::find(const std::string& id) {
    for (auto& d : devices_)
        if (d.id == id) return &d;
    return nullptr;
}

void DeviceStore::upsert(DeviceRecord rec) {
    if (auto* d = find(rec.id)) *d = std::move(rec);
    else devices_.push_back(std::move(rec));
}

bool DeviceStore::remove(const std::string& id) {
    for (auto it = devices_.begin(); it != devices_.end(); ++it)
        if (it->id == id) {
            secureZero(it->key);
            devices_.erase(it);
            return true;
        }
    return false;
}

std::vector<link::PeerRecord> DeviceStore::peers() const {
    std::vector<link::PeerRecord> out;
    for (auto& d : devices_) {
        link::PeerRecord p;
        p.id = d.id;
        p.name = d.name;
        p.platform = d.platform;
        p.btAddress = d.btAddress;
        p.manualEndpoints = d.manualEndpoints;
        p.key = d.key;
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace md::platform
