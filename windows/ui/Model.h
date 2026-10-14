// Parsed view of the host -> UI messages described in docs/ipc.md.
// Everything is tolerant of missing / unknown fields: the raw JSON is kept alongside the
// typed fields so that new host versions never break the UI.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mdui
{
    using json = nlohmann::json;

    // ---- JSON helpers (never throw) -------------------------------------------------------
    std::string JStr(json const& j, char const* key, std::string const& def = {});
    int64_t JInt(json const& j, char const* key, int64_t def = 0);
    double JDbl(json const& j, char const* key, double def = 0.0);
    bool JBool(json const& j, char const* key, bool def = false);
    json const& JObj(json const& j, char const* key);   // returns a static empty object when absent
    json const& JArr(json const& j, char const* key);   // returns a static empty array when absent
    // Dotted path lookup ("ui.compact.snap"); returns nullptr when absent.
    json const* JPath(json const& j, std::string_view path);
    // Builds {"a":{"b":value}} from "a.b".
    json MakePatch(std::string_view path, json value);
    // Deep merge used for optimistic settings updates (objects merge, null removes, everything else replaces).
    void DeepMerge(json& target, json const& patch);

    std::wstring Widen(std::string_view utf8);
    std::string Narrow(std::wstring_view wide);

    // High-resolution monotonic clock in milliseconds (QueryPerformanceCounter). GetTickCount64 only
    // advances in ~16 ms steps, which is too coarse for per-frame lyric animation.
    double QpcNowMs();
    // GetTickCount64() for position extrapolation (the host's atTick time base). Both clocks can be
    // frozen and stepped by the off-screen render test (--render-test) for deterministic frames.
    uint64_t TickNow();
    namespace testclock
    {
        void Freeze();               // virtual time = real time now, then only Advance() moves it
        void Advance(double ms);
        bool Frozen();
    }

    // ---- Typed state ------------------------------------------------------------------------
    enum class Side { Local, Remote };
    inline char const* SideName(Side s) { return s == Side::Local ? "local" : "remote"; }

    struct Volume
    {
        bool available{ false };
        double level{ 0 };   // 0..1
        bool muted{ false };
        int steps{ 0 };      // phone volume steps (0 = unknown)
        std::string error;   // e.g. "background_restricted" (Android blocks background volume changes)
    };

    struct Session
    {
        Side side{ Side::Local };
        std::string key;
        std::string appId;
        std::string appName;
        std::string title;
        std::string artist;
        std::string album;
        std::string trackId;
        int64_t durationMs{ -1 };      // -1 = unknown
        int64_t positionMs{ -1 };      // -1 = unknown
        uint64_t atTick{ 0 };
        double anchorQpcMs{ 0 };   // QPC time corresponding to atTick (set on receipt)
        double rate{ 1.0 };
        std::string status;            // playing, paused, stopped, buffering, unknown
        std::vector<std::string> actions;
        std::string artworkPath;       // empty = none
        std::string kind;
        std::string policyState;
        std::string source;              // smtc | compat | remote (may be empty)
        bool positionEstimated{ false }; // position is approximate (e.g. compat sessions)
        Volume appVolume;                // per-app mixer volume (local sessions)
        // Lyrics timing (host is the single source of truth): total delay = output latency + the
        // per-app manual offset. Lyrics are shown at position - lyricsDelayMs. 0 with older hosts.
        int64_t lyricsDelayMs{ 0 };
        int64_t outputLatencyMs{ 0 };
        bool hasLyricsDelay{ false };    // the host reports lyricsDelayMs (and accepts lyrics.offset)

        bool Has(std::string_view action) const;
        bool IsPlaying() const { return status == "playing"; }
        bool CanPlay() const { return Has("play") || Has("toggle"); }
        bool CanPause() const { return Has("pause") || Has("toggle"); }
        bool CanSeek() const { return Has("seek") && durationMs > 0; }
        // Extrapolated position at the given GetTickCount64() value (-1 when unknown).
        int64_t PositionAt(uint64_t nowTick) const;
        // Sub-millisecond position for animation: positionMs + (qpcNow - anchor) * rate, clamped.
        double PositionPrecise(double qpcNowMs) const;
        // Local optimistic update after a seek.
        void Rebase(int64_t newPositionMs);
        std::string const& DisplayApp() const { return appName.empty() ? appId : appName; }
    };

    struct SideState
    {
        int64_t gen{ 0 };
        std::string active;
        std::vector<Session> sessions;
    };

    struct Peer
    {
        std::string id;
        std::string name;
        std::string platform;
        std::string app;            // app version (DeviceInfo.app); empty = unknown
        std::string versionMatch;   // vs this PC (host): equal | patch | mismatch | unknown
    };

    struct Connection
    {
        std::string status{ "no_peer" };   // no_peer, disconnected, connecting, connected
        std::string transport;              // bt, lan
        std::string secondary;
        Peer peer;
        uint64_t sinceTick{ 0 };
        int64_t rttMs{ -1 };
        std::string mode;
        std::string lastError;
        int64_t bytesIn{ 0 };
        int64_t bytesOut{ 0 };
        // connection.bluetooth {adapter, on}; adapter may be a bool (present) or the adapter name.
        bool btKnown{ false };
        bool btAdapter{ false };
        std::string btAdapterName;
        bool btOn{ false };
    };

    struct Device
    {
        std::string id;
        std::string name;
        std::string platform;
        std::string btAddress;
        std::string lanHost;
        bool connected{ false };
        std::string transport;
        int64_t lastSeen{ 0 };   // unix ms
        std::string app;            // last app version the phone reported (kept while disconnected)
        std::string versionMatch;   // equal | patch | mismatch | unknown (host: md::version)
    };

    struct Focus
    {
        Side side{ Side::Local };
        bool hasSide{ false };
        std::string key;
        bool automatic{ true };
    };

    struct PolicyAction
    {
        std::string what;
        std::string reason;
        std::string app;
        uint64_t atTick{ 0 };
    };

    struct Policy
    {
        std::string mode;
        bool enabled{ false };
        std::string remote;
        std::string local;
        bool windowsAudioActive{ false };
        bool suspended{ false };
        bool gated{ false };   // policy.onlyWithOutputs is set and the default output is not in it
        std::vector<std::string> activeApps;
        std::optional<PolicyAction> lastAction;
    };

    struct AudioEndpoint
    {
        bool available{ false };
        std::string state{ "off" };
        std::string device;
        std::string error;
        bool IsOn() const { return !state.empty() && state != "off"; }
    };

    struct Taskbar
    {
        bool known{ false };
        bool supported{ false };
        bool shown{ false };
        std::string reason;
    };

    struct AudioOutput
    {
        std::string id;
        std::string name;
        bool isDefault{ false };
    };

    struct Pairing
    {
        std::string state{ "idle" };
        std::string sas;
        std::string peerName;
        std::string error;
    };

    struct Discovered
    {
        std::string kind;   // bt, lan
        std::string id;
        std::string name;
        std::string detail;
        bool paired{ false };
        bool ready{ true };   // absent = ready
    };

    struct State
    {
        bool valid{ false };
        uint64_t tick{ 0 };
        Peer self;                 // self.app = the host's (this PC's) version
        bool standalone{ false };  // settings mode "standalone": this PC only, no phone
        Connection connection;
        std::vector<Device> devices;
        SideState local;
        SideState remote;
        Focus focus;
        Policy policy;
        AudioEndpoint a2dpSink;
        AudioEndpoint lanSend;
        Volume localVolume;
        Volume remoteVolume;
        Taskbar taskbar;
        // audioOutputLatency {bluetooth, ms}: latency of Windows' current output as the host applies it.
        struct OutputLatency
        {
            bool known{ false };
            bool bluetooth{ false };
            int64_t ms{ 0 };
        } outputLatency;
        std::vector<AudioOutput> audioOutputs;
        AudioOutput const* DefaultOutput() const;
        Pairing pairing;
        std::vector<Discovered> discovered;
        bool overlayVisible{ false };
        json settings = json::object();
        bool diagnosticsEnabled{ false };

        // The session shown by the cards: focus.side/focus.key, else that side's active session,
        // else the first playing session, else the first session of any side. nullptr if none.
        Session const* FocusedSession() const;
        Session const* Find(Side side, std::string_view key) const;
        std::string PeerDisplayName() const;

        // Settings accessors with the documented defaults.
        std::string Setting(std::string_view path, std::string const& def) const;
        bool Setting(std::string_view path, bool def) const;
        double SettingNumber(std::string_view path, double def) const;
        // Manual lyrics offset for an app (settings lyrics.offsets[appId], ms; app ids contain dots).
        int64_t LyricsOffset(std::string const& appId) const;
    };

    State ParseState(json const& j, uint64_t receivedTick);

    struct LyricWord
    {
        int64_t t{ 0 };
        int64_t d{ 0 };
        std::string text;
    };

    struct LyricLine
    {
        int64_t t{ 0 };
        int64_t d{ 0 };
        std::string text;
        std::string tr;   // translated line ("tr"), empty when there is none
        std::vector<LyricWord> words;
    };

    struct Lyrics
    {
        bool valid{ false };
        std::string trackId;
        Side side{ Side::Local };
        std::string key;
        std::string status;   // loading, found, notfound, disabled
        std::string source;
        bool synced{ false };
        bool hasTranslation{ false };   // at least one line carries a translation
        std::vector<LyricLine> lines;
        // The song these lyrics were found for (newer hosts; empty / -1 otherwise).
        std::string title;
        std::string artist;
        int64_t durationMs{ -1 };

        bool HasWords() const;
        // These lyrics are for the given session: same trackId, or the same player session and song
        // under a new trackId (lyrics::SameSong). Older hosts without key: trackId, else side + key.
        bool BelongsTo(Session const* s) const;
        // True when these lyrics belong to the given session and contain displayable lines.
        bool MatchesAndShowable(Session const* s) const;
        // Identity of the content (lines, timing, translations, source) for rebuild decisions;
        // independent of the trackId, so a same-song trackId change does not rebuild a view.
        std::string ContentKey() const;
        // Index of the line active at positionMs (-1 before the first line).
        int LineAt(int64_t positionMs) const;
    };

    Lyrics ParseLyrics(json const& j);

    // Same player session (side + key) and the same song, also across a trackId change for it
    // (players re-reporting the duration). Views use it instead of comparing trackIds.
    bool SameSong(Session const& a, Session const& b);

    std::wstring FormatTime(int64_t ms);
    std::wstring FormatBytes(int64_t bytes);
}
