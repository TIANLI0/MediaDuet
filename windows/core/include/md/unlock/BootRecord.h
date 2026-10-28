#pragma once
#include "md/link/ConnectionManager.h"

namespace md::unlock {
// The boot service has only a sealed password and the existing link pairing key.
// The phone's password-unsealing key is never persisted on the PC.
struct BootRecord {
    BootRecord() = default;
    BootRecord(const BootRecord&) = default;
    BootRecord(BootRecord&&) = default;
    BootRecord& operator=(const BootRecord&) = default;
    BootRecord& operator=(BootRecord&&) = default;
    ~BootRecord() { secureZero(peer.key); }
    std::string userSid, enrollment, language = "en-US";
    proto::DeviceInfo self;
    link::PeerRecord peer;
    Bytes sealed;
    bool stale = false;
    int64_t lastUnlockAt = 0;
};
bool safeSid(std::string_view sid);
nlohmann::json bootRecordJson(const BootRecord& record);
std::optional<BootRecord> parseBootRecord(const nlohmann::json& json);
// Unknown state fails closed. An existing locked desktop is served only for the
// same SID when its user host is absent, including Windows restart restoration.
bool bootSessionAllowed(uint32_t clientSession, uint32_t consoleSession, bool querySucceeded, bool hasUser,
                        bool locked = false, bool sameUser = false, bool userHostPresent = false);
}
