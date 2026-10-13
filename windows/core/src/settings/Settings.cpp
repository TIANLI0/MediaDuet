#include "md/settings/Settings.h"

#include "md/util/Log.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace md::settings {

json defaults() {
    return json::parse(R"({
  "language": "auto",
  "theme": "system",
  "autostart": true,
  "connection": {"mode": "auto", "switchBackDelaySec": 10},
  "policy": {
    "mode": "windows_first",
    "autoResume": true,
    "resumeDelaySec": 5,
    "activationMs": 1500,
    "sensitivity": 0.02,
    "pauseForCalls": true,
    "headsetAssist": true,
    "onlyWithOutputs": [],
    "blacklist": [],
    "whitelist": [],
    "priorities": {}
  },
  "smtcBridge": true,
  "compat": {"neteaseDesktop": true},
  "taskbar": {"enabled": true, "content": "auto", "position": "left", "offset": 0, "width": 360, "fontSize": 13},
  "lyrics": {
    "online": true,
    "providers": ["netease", "qqmusic", "lrclib"],
    "translation": true,
    "localFolder": "",
    "latency": {"auto": true, "bluetoothMs": 180},
    "offsets": {},
    "overlay": {
      "visible": false, "locked": false, "clickThrough": false, "autoHide": true,
      "topmost": true, "hideInFullscreen": false, "layout": "alternate",
      "fontFamily": "Microsoft YaHei UI", "fontSize": 32, "opacity": 0.92,
      "karaoke": true, "twoLines": true,
      "colorActive": "#FFFFFFFF", "colorInactive": "#B3FFFFFF",
      "x": null, "y": null, "width": 900
    }
  },
  "ui": {
    "alwaysOnTop": false,
    "animations": true,
    "lowPower": false,
    "main": {"x": null, "y": null, "width": null, "height": null, "maximized": false},
    "compact": {"x": null, "y": null, "alwaysOnTop": true, "snap": true}
  },
  "diagnostics": false
})");
}

void deepMerge(json& base, const json& patch) {
    if (!patch.is_object() || !base.is_object()) {
        base = patch;
        return;
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        auto b = base.find(it.key());
        if (b != base.end() && b->is_object() && it->is_object())
            deepMerge(*b, *it);
        else
            base[it.key()] = *it;
    }
}

namespace {

// Ensures `s[key]` has the same JSON type as the default (or null when allowed).
void conform(json& s, const json& def) {
    if (!s.is_object() || !def.is_object()) return;
    for (auto it = def.begin(); it != def.end(); ++it) {
        auto cur = s.find(it.key());
        if (cur == s.end()) {
            s[it.key()] = *it;
            continue;
        }
        if (it->is_object()) {
            if (!cur->is_object()) *cur = *it;
            else conform(*cur, *it);
            continue;
        }
        if (it->is_null()) {
            if (!cur->is_null() && !cur->is_number()) *cur = nullptr;
            continue;
        }
        bool ok = (it->is_boolean() && cur->is_boolean()) || (it->is_number() && cur->is_number()) ||
                  (it->is_string() && cur->is_string()) || (it->is_array() && cur->is_array());
        if (!ok) *cur = *it;
    }
}

void clampNum(json& j, const char* key, double lo, double hi) {
    if (auto it = j.find(key); it != j.end() && it->is_number()) {
        double v = it->get<double>();
        if (v < lo || v > hi || v != v) *it = std::clamp(v != v ? lo : v, lo, hi);
    }
}

void oneOf(json& j, const char* key, std::initializer_list<const char*> allowed, const char* fallback) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        j[key] = fallback;
        return;
    }
    for (auto a : allowed)
        if (*it == a) return;
    *it = fallback;
}

void stringList(json& arr, size_t maxItems, size_t maxLen) {
    json out = json::array();
    for (auto& x : arr)
        if (x.is_string() && !x.get_ref<const std::string&>().empty() &&
            x.get_ref<const std::string&>().size() <= maxLen && out.size() < maxItems)
            out.push_back(x);
    arr = out;
}

} // namespace

json normalize(json s) {
    if (!s.is_object()) s = json::object();
    json def = defaults();
    conform(s, def);
    oneOf(s, "language", {"auto", "zh-CN", "en-US"}, "auto");
    oneOf(s, "theme", {"system", "light", "dark"}, "system");
    oneOf(s["connection"], "mode", {"auto", "bt_only", "lan_only"}, "auto");
    clampNum(s["connection"], "switchBackDelaySec", 0, 600);
    auto& p = s["policy"];
    oneOf(p, "mode", {"off", "windows_first", "android_first", "latest", "custom"}, "windows_first");
    clampNum(p, "resumeDelaySec", 0, 600);
    clampNum(p, "activationMs", 0, 60000);
    clampNum(p, "sensitivity", 0.0005, 1.0);
    stringList(p["blacklist"], 200, 260);
    stringList(p["whitelist"], 200, 260);
    if (!p["priorities"].is_object()) p["priorities"] = json::object();
    json pr = json::object();
    for (auto it = p["priorities"].begin(); it != p["priorities"].end(); ++it)
        if (it->is_number_integer() && it.key().size() <= 260 && pr.size() < 200)
            pr[it.key()] = std::clamp<int64_t>(it->get<int64_t>(), -1000, 1000);
    p["priorities"] = pr;
    {
        json prov = json::array();
        for (auto& x : s["lyrics"]["providers"])
            if (x.is_string() && (x == "netease" || x == "qqmusic" || x == "lrclib") &&
                std::find(prov.begin(), prov.end(), x) == prov.end())
                prov.push_back(x);
        s["lyrics"]["providers"] = prov;
    }
    auto& tb = s["taskbar"];
    oneOf(tb, "content", {"auto", "lyrics", "player"}, "auto");
    oneOf(tb, "position", {"left", "right"}, "left");
    clampNum(tb, "offset", -4000, 4000);
    clampNum(tb, "width", 160, 1200);
    clampNum(tb, "fontSize", 9, 24);
    auto& o = s["lyrics"]["overlay"];
    clampNum(o, "fontSize", 12, 96);
    clampNum(o, "opacity", 0.2, 1.0);
    clampNum(o, "width", 300, 4000);
    oneOf(o, "layout", {"alternate", "center"}, "alternate");
    auto& lat = s["lyrics"]["latency"];
    clampNum(lat, "bluetoothMs", 0, 1000);
    if (lat["bluetoothMs"].is_number_float()) lat["bluetoothMs"] = static_cast<int64_t>(std::llround(lat["bluetoothMs"].get<double>()));
    {
        // Per-app lyrics offsets {appId: ms}; null (or a non-number) removes an entry.
        json offs = json::object();
        for (auto& [k, v] : s["lyrics"]["offsets"].items())
            if (v.is_number() && !k.empty() && k.size() <= 260 && offs.size() < 200) {
                double ms = v.get<double>();
                if (ms != ms) continue;
                int64_t iv = std::clamp<int64_t>(static_cast<int64_t>(std::llround(std::clamp(ms, -1e7, 1e7))), -5000, 5000);
                if (iv != 0) offs[k] = iv;
            }
        s["lyrics"]["offsets"] = offs;
    }
    if (s["lyrics"]["localFolder"].get_ref<const std::string&>().size() > 1024) s["lyrics"]["localFolder"] = "";
    return s;
}

Settings Settings::load(const std::filesystem::path& file) {
    Settings st;
    std::ifstream in(file, std::ios::binary);
    if (!in) return st;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        log::warn("settings", "settings file is corrupt; using defaults");
        return st;
    }
    st.data_ = normalize(std::move(j));
    return st;
}

bool Settings::save(const std::filesystem::path& file) const {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data_.dump(2);
        if (!out) return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void Settings::apply(const json& patch) {
    json merged = data_;
    deepMerge(merged, patch);
    data_ = normalize(std::move(merged));
}

} // namespace md::settings
