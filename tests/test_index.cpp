#include <catch2/catch_test_macros.hpp>

#include <string>

#include "oramacore/index.hpp"

namespace ix = oramacore::index;

static ix::MemoryIndex make_index() {
    return ix::MemoryIndex({
        {.name = "title", .params = {.weight = 3.0, .b = 0.75}},  // title boosted
        {.name = "body",  .params = {.weight = 1.0, .b = 0.75}},
    });
}

TEST_CASE("add + search returns matching docs ranked", "[index]") {
    auto idx = make_index();
    idx.add({{"title", "Postgres tuning guide"}, {"body", "how to tune a postgres database for speed"}});
    idx.add({{"title", "Cooking pasta"},         {"body", "boil water, add salt, add pasta"}});
    idx.add({{"title", "Database indexes"},       {"body", "btree, gin, brin, and postgres indexes explained"}});

    REQUIRE(idx.doc_count() == 3);

    auto r = idx.search("postgres");
    REQUIRE(r.total_matched == 2);          // docs 0 and 2 mention postgres; doc 1 doesn't
    REQUIRE(r.hits.size() == 2);
    // every returned hit has positive score, sorted desc
    REQUIRE(r.hits[0].score >= r.hits[1].score);
    REQUIRE(r.hits[0].score > 0.0);
}

TEST_CASE("title weight boosts a title match over a body-only match", "[index]") {
    auto idx = make_index();
    const auto a = idx.add({{"title", "widget"},      {"body", "nothing relevant here at all"}}); // term in title
    const auto b = idx.add({{"title", "unrelated"},   {"body", "widget appears only in the body text"}}); // term in body

    auto r = idx.search("widget");
    REQUIRE(r.hits.size() == 2);
    // title (weight 3) should outrank body (weight 1) for the same term
    REQUIRE(r.hits[0].doc_id == a);
    REQUIRE(r.hits[1].doc_id == b);
}

TEST_CASE("multi-term query sums per-term scores", "[index]") {
    auto idx = make_index();
    const auto both = idx.add({{"title", "fast postgres"}, {"body", "fast postgres queries"}});
    const auto one  = idx.add({{"title", "fast cars"},     {"body", "fast cars go fast"}});

    auto r = idx.search("fast postgres");
    REQUIRE(r.hits.size() == 2);
    // doc with both query terms should outrank the doc with only "fast"
    REQUIRE(r.hits[0].doc_id == both);
    REQUIRE(r.hits[1].doc_id == one);
}

TEST_CASE("search uses the same tokenization as indexing (stemming/stop-words)", "[index]") {
    auto idx = make_index();
    idx.add({{"title", "Running servers"}, {"body", "the servers are running quickly"}});
    // "server" (stem of servers) and "runn" (stem of running) should match;
    // "the"/"are" are stop-words and shouldn't matter.
    REQUIRE(idx.search("server").hits.size() == 1);
    REQUIRE(idx.search("running").hits.size() == 1);   // running -> runn matches
    REQUIRE(idx.search("the").hits.empty());            // stop word -> no tokens -> no hits
}

TEST_CASE("empty index and no-match queries are safe", "[index]") {
    auto idx = make_index();
    REQUIRE(idx.search("anything").hits.empty());
    idx.add({{"title", "hello"}, {"body", "world"}});
    REQUIRE(idx.search("nonexistentterm").hits.empty());
    REQUIRE(idx.search("hello").hits.size() == 1);
}

TEST_CASE("top_k caps results", "[index]") {
    auto idx = make_index();
    for (int i = 0; i < 5; ++i) idx.add({{"body", "common term here"}});
    auto r = idx.search("common", /*top_k=*/3);
    REQUIRE(r.total_matched == 5);
    REQUIRE(r.hits.size() == 3);
}
