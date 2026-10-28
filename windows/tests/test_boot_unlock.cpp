#define MD_UNLOCK_SERVICE_TESTING
#include "../unlock/Service.cpp"
#include "md/util/ManualScheduler.h"
#include "md/crypto/Crypto.h"
#include <doctest/doctest.h>
#include <fstream>

namespace md::unlock {
namespace {
struct UserTestAccess {
    static void line(User& user, json j) { user.onLine(j.dump()); }
    static void pending(User& user, uint64_t id) { user.request_ = id; user.want_ = true; }
    static void session(User& user, DWORD id) { *user.session_ = id; }
    static const void* connection(User& user) { return user.cm_.get(); }
    static bool waiting(User& user) { return user.want_ || user.request_ != 0; }
};
const Bytes phoneKey(32, 7);
BootRecord example() {
    BootRecord r;
    r.userSid = "S-1-5-21-1-2-3-1001";
    r.enrollment = "0123456789abcdef0123456789abcdef";
    r.self = {"11111111111111111111111111111111", "TEST PC", "windows", "PC", "Windows", "1.10.0"};
    r.peer.id = "22222222222222222222222222222222";
    r.peer.name = "TEST Phone"; r.peer.platform = "android"; r.peer.key = Bytes(32, 9);
    r.sealed = seal(phoneKey, r.enrollment, r.userSid, "synthetic-password");
    return r;
}
struct SilentFactory : link::ITransportFactory {
    int attempts = 0;
    void connectBluetooth(const link::PeerRecord&, Done done) override { ++attempts; done({}, "offline"); }
    void connectLan(const link::LanEndpoint&, Done done) override { ++attempts; done({}, "offline"); }
};
struct Harness {
    ManualScheduler sched;
    SilentFactory factory;
    bool allowed = true;
    std::vector<json> sent;
    std::vector<BootRecord> saved;
    std::unique_ptr<User> user;
    Harness() {
        User::Environment env;
        env.pipes = false;
        env.allowSession = [this](DWORD session, const std::string&) { return allowed && session == 1; };
        env.signedIn = [this](DWORD, const std::string&) { return !allowed; };
        env.save = [this](const BootRecord& r) { saved.push_back(r); return true; };
        env.erase = [](const std::string&) { return true; };
        env.output = [this](const json& j) { sent.push_back(j); };
        user = std::make_unique<User>(sched, factory, example(), std::move(env));
        UserTestAccess::session(*user, 1);
        UserTestAccess::line(*user, {{"type", "hello"}, {"v", 1}, {"sid", example().userSid}});
    }
    ~Harness() { user.reset(); sched.runPending(); }
    size_t credentials() const {
        return std::count_if(sent.begin(), sent.end(), [](const json& j) { return j.value("type", "") == "credential"; });
    }
    void reply(Bytes key = phoneKey, uint64_t re = 42, std::string peer = "22222222222222222222222222222222") {
        proto::Envelope e; e.type = proto::types::UnlockResult; e.src = peer; e.re = re;
        e.body = {{"ok", true}, {"key", base64Encode(key)}};
        user->onEnvelope(peer, e, 1);
    }
};
}

TEST_CASE("boot unlock: record validates identity, endpoints and key sizes") {
    auto r = example(); auto j = bootRecordJson(r);
    auto parsed = parseBootRecord(j);
    REQUIRE(parsed); CHECK(parsed->peer.key == r.peer.key); CHECK(parsed->sealed == r.sealed);
    CHECK_FALSE(j.contains("password")); CHECK_FALSE(j.contains("key"));
    CHECK(j.dump().find(base64Encode(phoneKey)) == std::string::npos);
    SUBCASE("path traversal SID") { j["sid"] = "../../other-user"; CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("bad schema") { j["v"] = 1; CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("bad pairing key") { j["peer"]["key"] = base64Encode(Bytes(16, 1)); CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("other identity") { j["peer"]["id"] = r.self.id; CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("bad enrollment") { j["enrollment"] = "old"; CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("invalid endpoint") { j["peer"]["endpoints"] = {{{"host", "a\nb"}, {"port", 47321}}}; CHECK_FALSE(parseBootRecord(j)); }
    SUBCASE("too many endpoints") { for (int i = 0; i < 17; ++i) j["peer"]["endpoints"].push_back({{"host", "127.0.0.1"}, {"port", 47321}}); CHECK_FALSE(parseBootRecord(j)); }
}

TEST_CASE("boot unlock: only the pre-sign-in physical console can request a password") {
    CHECK(bootSessionAllowed(1, 1, true, false));
    CHECK_FALSE(bootSessionAllowed(0, 0, true, false));
    CHECK_FALSE(bootSessionAllowed(1, UINT32_MAX, true, false));
    CHECK_FALSE(bootSessionAllowed(2, 1, true, false));
    CHECK_FALSE(bootSessionAllowed(1, 1, false, false));
    CHECK_FALSE(bootSessionAllowed(1, 1, true, true));
    // Windows ARSO can restore a locked user session after restart even when the
    // tray app has no autostart entry. The service can handle that same-user tile.
    CHECK(bootSessionAllowed(1, 1, true, true, true, true, false));
    CHECK_FALSE(bootSessionAllowed(1, 1, true, true, true, true, true));
    CHECK_FALSE(bootSessionAllowed(1, 1, true, true, true, false, false));
    CHECK_FALSE(bootSessionAllowed(1, 1, true, true, false, true, false));
    CHECK(safeSid("S-1-12-1-123-456-789-123"));
    CHECK_FALSE(safeSid("S-1-5-21-1--2")); CHECK_FALSE(safeSid("S-1-5-21-1/2"));
}

TEST_CASE("boot unlock: synthetic password is released once after matching phone approval") {
    Harness h; UserTestAccess::pending(*h.user, 42); h.reply();
    REQUIRE(h.credentials() == 1);
    auto credential = std::find_if(h.sent.begin(), h.sent.end(), [](auto& j) { return j.value("type", "") == "credential"; });
    CHECK((*credential)["password"] == "synthetic-password");
    h.reply(); CHECK(h.credentials() == 1);
}

TEST_CASE("boot unlock: prewarming reuses the link without requesting phone approval") {
    Harness h; h.user->warm(1);
    auto connection = UserTestAccess::connection(*h.user);
    REQUIRE(connection != nullptr);
    CHECK_FALSE(UserTestAccess::waiting(*h.user)); CHECK(h.credentials() == 0);
    UserTestAccess::line(*h.user, {{"type", "request"}});
    CHECK(UserTestAccess::connection(*h.user) == connection);
    CHECK(UserTestAccess::waiting(*h.user));
    UserTestAccess::line(*h.user, {{"type", "cancel"}});
    CHECK(UserTestAccess::connection(*h.user) == connection);
    CHECK_FALSE(UserTestAccess::waiting(*h.user)); CHECK(h.credentials() == 0);
    h.allowed = false; h.sched.advance(500);
    CHECK(UserTestAccess::connection(*h.user) == nullptr);
}

TEST_CASE("boot unlock: foreign phone, late reply, wrong key and switched session fail closed") {
    Harness h; UserTestAccess::pending(*h.user, 42);
    SUBCASE("different peer") { h.reply(phoneKey, 42, "33333333333333333333333333333333"); }
    SUBCASE("different request") { h.reply(phoneKey, 41); }
    SUBCASE("wrong key") { h.reply(Bytes(32, 1)); }
    SUBCASE("desktop signed in before approval") { h.allowed = false; h.reply(); }
    SUBCASE("cancelled tile") { UserTestAccess::line(*h.user, {{"type", "cancel"}}); h.reply(); }
    SUBCASE("wrong user hello") {
        UserTestAccess::line(*h.user, {{"type", "hello"}, {"v", 1}, {"sid", "S-1-5-21-1-2-3-1002"}});
        h.reply();
    }
    CHECK(h.credentials() == 0);
}

TEST_CASE("boot unlock: offline request times out and cannot use a late approval") {
    Harness h;
    UserTestAccess::line(*h.user, {{"type", "request"}});
    h.sched.advance(kRequestWaitMs + 1);
    REQUIRE(!h.sent.empty()); CHECK(h.sent.back()["state"] == "failed"); CHECK(h.sent.back()["code"] == "timeout");
    h.reply(); CHECK(h.credentials() == 0);
}

TEST_CASE("boot unlock: Windows wrong password result persists stale state") {
    Harness h; UserTestAccess::pending(*h.user, 42); h.reply();
    UserTestAccess::line(*h.user, {{"type", "result"}, {"ok", false}, {"status", int64_t(-1073741715)}});
    REQUIRE(!h.saved.empty()); CHECK(h.saved.back().stale);
    CHECK(h.sent.back()["state"] == "stale");
}

TEST_CASE("boot unlock: signing in ends background authentication and records the time") {
    Harness h; UserTestAccess::pending(*h.user, 42); h.reply();
    h.allowed = false; h.sched.advance(500);
    REQUIRE(!h.saved.empty()); CHECK(h.saved.back().lastUnlockAt == h.sched.wallMs());
    UserTestAccess::line(*h.user, {{"type", "request"}});
    h.reply(); CHECK(h.credentials() == 1); CHECK(h.sent.back()["state"] == "not_enrolled");
}

TEST_CASE("boot unlock: protected staging survives a new read without a user-profile DPAPI key") {
    auto r = example(); r.userSid = text::toUtf8(app::currentUserSid());
    auto directory = std::filesystem::temp_directory_path() / (L"MediaDuet-boot-test-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(directory);
    auto file = stageBootRecord(r, directory);
    REQUIRE(file);
    std::ifstream in(*file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), {}); in.close();
    CHECK(bytes.find("synthetic-password") == std::string::npos);
    CHECK(bytes.find(base64Encode(r.peer.key)) == std::string::npos);
    auto restored = readBootRecord(*file);
    REQUIRE(restored); CHECK(restored->userSid == r.userSid); CHECK(restored->sealed == r.sealed);
    CHECK(restored->peer.key == r.peer.key);
    auto blob = protectBootBytes(view(std::string_view("synthetic"))); REQUIRE(blob);
    blob->back() ^= 1; CHECK_FALSE(unprotectBootBytes(*blob));
    std::filesystem::remove(*file); std::filesystem::remove(directory);
}

TEST_CASE("boot unlock: a normal process cannot impersonate the SCM-registered service") {
    CHECK_FALSE(isBootServiceProcess(GetCurrentProcessId()));
    CHECK_FALSE(isBootServicePipe(INVALID_HANDLE_VALUE));
}

TEST_CASE("boot unlock: native session-zero transport delivers data before EOF") {
    WSADATA wsa{}; REQUIRE(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(server != INVALID_SOCKET);
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(listen(server, 1) == 0);
    int size = sizeof(address); REQUIRE(getsockname(server, reinterpret_cast<sockaddr*>(&address), &size) == 0);
    auto echo = std::async(std::launch::async, [server] {
        fd_set readers; FD_ZERO(&readers); FD_SET(server, &readers); timeval timeout{3, 0};
        if (select(0, &readers, nullptr, nullptr, &timeout) != 1) { closesocket(server); return false; }
        SOCKET client = accept(server, nullptr, nullptr); closesocket(server);
        if (client == INVALID_SOCKET) return false;
        DWORD timeoutMs = 3000; setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeoutMs), sizeof(timeoutMs));
        char input[4]; int count = 0;
        while (count < 4) { int n = recv(client, input + count, 4 - count, 0); if (n <= 0) break; count += n; }
        bool ok = count == 4 && ::send(client, input, 4, 0) == 4;
        shutdown(client, SD_SEND); closesocket(client); return ok;
    });
    {
        ManualScheduler sched; ServiceTransportFactory factory(sched);
        link::TransportPtr transport; bool connected = false;
        factory.connectLan({"127.0.0.1", ntohs(address.sin_port), "test"}, [&](auto t, auto) { transport = std::move(t); connected = true; });
        auto wait = [&](auto condition) {
            for (int i = 0; i < 400 && !condition(); ++i) { sched.runPending(); Sleep(10); }
            sched.runPending(); return condition();
        };
        REQUIRE(wait([&] { return connected; })); REQUIRE(transport);
        struct Listener : link::ITransport::Listener {
            Bytes received; bool closed = false;
            void onTransportData(ByteView data) override { received.insert(received.end(), data.begin(), data.end()); }
            void onTransportWritable() override {}
            void onTransportClosed(const std::string&) override { closed = true; }
        } listener;
        transport->start(&listener); transport->send(Bytes{1, 2, 3, 4});
        REQUIRE(wait([&] { return listener.closed; }));
        CHECK(listener.received == Bytes{1, 2, 3, 4}); transport.reset(); sched.runPending();
    }
    CHECK(echo.get()); WSACleanup();
}
}
