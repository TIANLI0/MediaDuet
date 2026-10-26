#pragma once

#include "md/protocol/Envelope.h"
#include "md/util/Bytes.h"

#include <optional>
#include <string>
#include <vector>

namespace md::proto {

struct DeviceInfo {
    std::string id;
    std::string name;
    std::string platform; // "windows" | "android"
    std::string model;
    std::string os;
    std::string app;
};

json toJson(const DeviceInfo& d);
std::optional<DeviceInfo> deviceInfoFromJson(const json& j, std::string* err = nullptr);

json errorBody(std::string_view code, std::string_view message = {});

namespace caps {
inline constexpr const char* MediaSource = "media.source";
inline constexpr const char* MediaControl = "media.control";
inline constexpr const char* Artwork = "artwork";
inline constexpr const char* Lyrics = "lyrics";
inline constexpr const char* PolicyHost = "policy.host";
inline constexpr const char* PolicyClient = "policy.client";
inline constexpr const char* AudioA2dpSink = "audio.a2dp_sink";
inline constexpr const char* AudioRecvPcm = "audio.recv.pcm";
inline constexpr const char* AudioSendPcm = "audio.send.pcm";
inline constexpr const char* SmtcBridge = "smtc.bridge";
inline constexpr const char* Volume = "volume";
inline constexpr const char* Handoff = "handoff";
inline constexpr const char* LyricsOffset = "lyrics.offset"; // manual lyrics offsets follow each other
} // namespace caps

// Reads a base64 field of an exact decoded length; nullopt if missing/invalid.
std::optional<Bytes> getB64(const json& body, const char* key, size_t exactLen);

} // namespace md::proto
