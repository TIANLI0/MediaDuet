#include "md/util/AppVersion.h"

#include <doctest/doctest.h>

using namespace md::version;

TEST_CASE("app version parsing ignores suffixes") {
    CHECK(parse("1.2.1") == AppVersion{1, 2, 1});
    CHECK(parse("1.2.1-beta.2") == AppVersion{1, 2, 1});
    CHECK(parse("1.2.1+build5") == AppVersion{1, 2, 1});
    CHECK(parse("v2.0.10 (debug)") == AppVersion{2, 0, 10});
    CHECK(parse(" 1.3") == AppVersion{1, 3, 0});
    CHECK(parse("1.3-rc1") == AppVersion{1, 3, 0});
    CHECK_FALSE(parse(""));
    CHECK_FALSE(parse("1"));
    CHECK_FALSE(parse("abc"));
    CHECK_FALSE(parse("1.x.3"));
    CHECK_FALSE(parse(".1.2"));
    CHECK_FALSE(parse("1.2.3.4"));
    CHECK_FALSE(parse("1234567.1.0"));
}

TEST_CASE("app version mismatch rule") {
    CHECK(compare("1.2.1", "1.2.1") == Match::Equal);
    CHECK(compare("1.2.1", "1.2.1-beta") == Match::Equal);
    CHECK(compare("1.2.1", "1.2.0") == Match::PatchDiffers);
    CHECK(compare("1.2.1", "1.2.3+7") == Match::PatchDiffers);
    CHECK(compare("1.2.1", "1.1.0") == Match::Mismatch);
    CHECK(compare("1.2.1", "2.2.1") == Match::Mismatch);
    CHECK(compare("1.2.1", "1.3") == Match::Mismatch);
    CHECK(compare("1.2.1", "") == Match::Unknown);
    CHECK(compare("1.2.1", "unknown") == Match::Unknown);
    CHECK(compare("", "1.2.1") == Match::Unknown);
    CHECK(std::string(toString(Match::Mismatch)) == "mismatch");
    CHECK(std::string(toString(Match::PatchDiffers)) == "patch");
    CHECK(std::string(toString(Match::Equal)) == "equal");
    CHECK(std::string(toString(Match::Unknown)) == "unknown");
}
