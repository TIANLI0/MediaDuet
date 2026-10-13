#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

namespace md::settings {

using json = nlohmann::json;

// Default settings (docs/ipc.md "Settings object").
json defaults();

// Recursively merges `patch` into `base`: objects merge, everything else replaces.
void deepMerge(json& base, const json& patch);

// Fills missing keys from defaults and clamps/validates known values. Unknown keys are kept.
json normalize(json s);

class Settings {
public:
    Settings() : data_(defaults()) {}
    static Settings load(const std::filesystem::path& file);
    // Atomic write (temp file + rename). Returns false on I/O failure.
    bool save(const std::filesystem::path& file) const;
    // Applies a patch; returns the normalised result.
    void apply(const json& patch);
    const json& data() const { return data_; }

    // Path lookup like "policy.mode"; returns fallback if missing or wrong type.
    template <class T>
    T get(const char* path, T fallback) const {
        const json* cur = &data_;
        std::string p(path);
        size_t start = 0;
        while (true) {
            size_t dot = p.find('.', start);
            std::string key = p.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (!cur->is_object()) return fallback;
            auto it = cur->find(key);
            if (it == cur->end()) return fallback;
            cur = &*it;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        try {
            if (cur->is_null()) return fallback;
            return cur->get<T>();
        } catch (...) {
            return fallback;
        }
    }

private:
    json data_;
};

} // namespace md::settings
