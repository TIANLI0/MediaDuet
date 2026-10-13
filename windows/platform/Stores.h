#pragma once

#include "md/link/ConnectionManager.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace md::platform {

std::filesystem::path appDataDir();   // %LOCALAPPDATA%\MediaDuet
std::filesystem::path cacheDir();     // %LOCALAPPDATA%\MediaDuet\cache

struct Identity {
    std::string id;   // 32 hex chars
    std::string name; // defaults to the computer name
};
Identity loadOrCreateIdentity();
void saveIdentityName(const std::string& name);

struct DeviceRecord {
    std::string id;
    std::string name;
    std::string platform;
    uint64_t btAddress = 0;
    std::vector<link::LanEndpoint> manualEndpoints;
    Bytes key; // decrypted pairing key, memory only
    int64_t pairedAt = 0;
    int64_t lastSeen = 0;
};

// Paired devices in devices.json; pairing keys are DPAPI-protected.
class DeviceStore {
public:
    void load();
    bool save() const;
    const std::vector<DeviceRecord>& devices() const { return devices_; }
    DeviceRecord* find(const std::string& id);
    void upsert(DeviceRecord rec);
    bool remove(const std::string& id);
    std::vector<link::PeerRecord> peers() const;

private:
    std::vector<DeviceRecord> devices_;
};

std::string formatBtAddress(uint64_t addr);
std::optional<uint64_t> parseBtAddress(const std::string& s);

} // namespace md::platform
