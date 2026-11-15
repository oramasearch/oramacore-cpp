// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include "oramacore/error.hpp"
#include "oramacore/version.hpp"

TEST_CASE("version is set", "[scaffold]") {
    REQUIRE(oramacore::kName == "oramacore-cpp");
    REQUIRE_FALSE(oramacore::kVersion.empty());
}

TEST_CASE("Result carries success and failure", "[scaffold]") {
    oramacore::Result<int> ok = 7;
    REQUIRE(ok.has_value());
    REQUIRE(*ok == 7);

    oramacore::Result<int> bad = oramacore::fail("nope");
    REQUIRE_FALSE(bad.has_value());
    REQUIRE(bad.error().message == "nope");
}
