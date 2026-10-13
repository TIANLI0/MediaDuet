#include "md/link/LinkSession.h"
#include "md/util/ManualScheduler.h"

#include <doctest/doctest.h>

using namespace md;
using namespace md::link;

TEST_CASE("dedup window accepts new ids once and rejects ancient ones") {
    DedupWindow w(4);
    CHECK(w.accept(1));
    CHECK_FALSE(w.accept(1));
    CHECK(w.accept(3));
    CHECK(w.accept(2)); // out of order but new
    CHECK(w.accept(10));
    CHECK_FALSE(w.accept(5)); // older than highest - capacity
    CHECK(w.accept(7));
    CHECK_FALSE(w.accept(7));
    CHECK_FALSE(w.accept(0));
}

TEST_CASE("pending commands complete, time out and are listed for re-send") {
    ManualScheduler s;
    LinkSession ls(s, "0123456789abcdef0123456789abcdef");
    ls.reset("fedcba9876543210fedcba9876543210");
    auto a = ls.make("media.command", {{"action", "pause"}});
    auto b = ls.make("media.command", {{"action", "next"}});
    CHECK(b.id == a.id + 1);
    CHECK(a.sid == "fedcba9876543210fedcba9876543210");
    std::vector<CommandOutcome> out;
    ls.track(a, [&](const CommandOutcome& o) { out.push_back(o); });
    ls.track(b, [&](const CommandOutcome& o) { out.push_back(o); });
    s.advance(1000);
    CHECK(ls.resendable(5000).size() == 2);

    proto::Envelope r;
    r.type = "media.command.result";
    r.re = a.id;
    r.body = {{"ok", false}, {"error", {{"code", "action_unsupported"}}}};
    CHECK(ls.completeCommand(r));
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].ok);
    CHECK(out[0].code == "action_unsupported");
    CHECK(out[0].latencyMs == 1000);
    CHECK_FALSE(ls.completeCommand(r)); // already done

    s.advance(4500);
    CHECK(ls.resendable(5000).empty()); // too old to re-send safely
    REQUIRE(out.size() == 2);
    CHECK(out[1].code == "timeout");
}

TEST_CASE("reset fails pending commands and restarts ids") {
    ManualScheduler s;
    LinkSession ls(s, "0123456789abcdef0123456789abcdef");
    ls.reset("00000000000000000000000000000001");
    auto a = ls.make("media.command", {});
    std::string code;
    ls.track(a, [&](const CommandOutcome& o) { code = o.code; });
    ls.reset("00000000000000000000000000000002");
    CHECK(code == "link_lost");
    CHECK(ls.make("ping", {}).id == 1);
}

TEST_CASE("result cache answers duplicates") {
    ManualScheduler s;
    LinkSession ls(s, "0123456789abcdef0123456789abcdef");
    proto::Envelope r;
    r.type = "media.command.result";
    r.re = 7;
    ls.cacheResult(7, r);
    REQUIRE(ls.cachedResult(7));
    CHECK(ls.cachedResult(7)->re == 7u);
    CHECK_FALSE(ls.cachedResult(8));
}
