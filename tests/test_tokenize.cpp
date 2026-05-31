#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "oramacore/tokenize.hpp"

namespace tk = oramacore::tokenize;

static std::vector<std::string> texts(const std::vector<tk::Token>& toks) {
    std::vector<std::string> out;
    for (const auto& t : toks) out.push_back(t.text);
    return out;
}

TEST_CASE("normalize lowercases ASCII", "[tokenize]") {
    REQUIRE(tk::normalize("Hello WORLD") == "hello world");
}

TEST_CASE("splits on non-alphanumeric and lowercases", "[tokenize]") {
    tk::Options o{.remove_stop_words = false, .stem = false};
    REQUIRE(texts(tk::tokenize("Hello, World! foo-bar", o))
            == std::vector<std::string>{"hello", "world", "foo", "bar"});
}

TEST_CASE("removes stop words", "[tokenize]") {
    tk::Options o{.remove_stop_words = true, .stem = false};
    // "the", "is", "a", "on" dropped
    REQUIRE(texts(tk::tokenize("the cat is a server on fire", o))
            == std::vector<std::string>{"cat", "server", "fire"});
}

TEST_CASE("minimal stemmer strips common suffixes", "[tokenize]") {
    REQUIRE(tk::stem("running") == "runn");     // -ing
    REQUIRE(tk::stem("stories") == "story");    // -ies -> y
    REQUIRE(tk::stem("studied") == "study");    // -ied -> y
    REQUIRE(tk::stem("servers") == "server");   // plural -s
    REQUIRE(tk::stem("quickly") == "quick");    // -ly
    // guards: don't mangle short words / -ss
    REQUIRE(tk::stem("is") == "is");
    REQUIRE(tk::stem("less") == "less");
}

TEST_CASE("full pipeline: normalize + stop-words + stem", "[tokenize]") {
    auto toks = tk::tokenize("The Servers are Running quickly");
    // "the","are" stopped; "servers"->server, "running"->runn, "quickly"->quick
    REQUIRE(texts(toks) == std::vector<std::string>{"server", "runn", "quick"});
}

TEST_CASE("tokens carry source offsets", "[tokenize]") {
    tk::Options o{.remove_stop_words = false, .stem = false};
    auto toks = tk::tokenize("ab cd", o);
    REQUIRE(toks.size() == 2);
    REQUIRE(toks[0].start == 0);
    REQUIRE(toks[0].length == 2);
    REQUIRE(toks[1].start == 3);
    REQUIRE(toks[1].length == 2);
}
