#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace md::proto {

using json = nlohmann::json;

inline constexpr int kProtocolVersion = 1;
inline constexpr uint64_t kMaxMessageId = (uint64_t(1) << 53) - 1;

struct Envelope {
    int v = kProtocolVersion;
    std::string type;
    uint64_t id = 0;
    int64_t ts = 0;
    std::string src;
    std::string sid;
    std::optional<uint64_t> re;
    json body = json::object();

    std::string serialize() const;
    // Strict validation per spec §6. Returns nullopt and fills err on failure.
    static std::optional<Envelope> parse(std::string_view text, std::string* err = nullptr);
};

bool isValidDeviceId(std::string_view s);   // 32 lowercase hex chars
bool isValidType(std::string_view s);

namespace types {
inline constexpr const char* PairRequest = "pair.request";
inline constexpr const char* PairResponse = "pair.response";
inline constexpr const char* PairConfirm = "pair.confirm";
inline constexpr const char* PairResult = "pair.result";
inline constexpr const char* Hello = "hello";
inline constexpr const char* HelloAck = "hello.ack";
inline constexpr const char* Auth = "auth";
inline constexpr const char* Error = "error";
inline constexpr const char* SessionReady = "session.ready";
inline constexpr const char* LinkPrimary = "link.primary";
inline constexpr const char* Ping = "ping";
inline constexpr const char* Pong = "pong";
inline constexpr const char* DeviceInfo = "device.info";
inline constexpr const char* TransportInfo = "transport.info";
inline constexpr const char* MediaSnapshot = "media.snapshot";
inline constexpr const char* MediaCommand = "media.command";
inline constexpr const char* MediaCommandResult = "media.command.result";
inline constexpr const char* ArtworkGet = "artwork.get";
inline constexpr const char* LyricsPush = "lyrics.push";
inline constexpr const char* LyricsNone = "lyrics.none";
inline constexpr const char* PolicyStatus = "policy.status";
inline constexpr const char* PolicyConfig = "policy.config";
inline constexpr const char* AudioOffer = "audio.offer";
inline constexpr const char* AudioAnswer = "audio.answer";
inline constexpr const char* AudioStop = "audio.stop";
inline constexpr const char* Bye = "bye";
inline constexpr const char* VolumeState = "volume.state";
inline constexpr const char* VolumeSet = "volume.set";
} // namespace types

} // namespace md::proto
