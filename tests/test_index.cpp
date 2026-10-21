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

// ----- corpus statistics: a sharded search scored with merged globals ----------
//
// A skewed corpus: shard A holds one weak match among nine misses (so its local
// IDF for the term is high), shard B holds five strong matches and nothing else
// (so its local IDF is near zero). Scored locally, A's weak match outranks B's
// strong ones and a per-shard top-k cutoff loses a document that belongs in the
// global top k. Scored with the merged statistics, every shard ranks exactly as
// one index over both would.

namespace {

// The body of a document mentioning "postgres" `tf` times, always 5 tokens long
// so only the corpus statistics (not the field lengths) differ between shards.
std::string body_with(int tf) {
    std::string body;
    const char* fillers[] = {"vacuum", "replica", "shard", "backup", "index"};
    for (int i = 0; i < 5; ++i) {
        if (!body.empty()) body += ' ';
        body += (i < tf) ? "postgres" : fillers[i];
    }
    return body;
}

struct SkewedCorpus {
    ix::MemoryIndex shard_a = make_index();
    ix::MemoryIndex shard_b = make_index();
    ix::MemoryIndex consolidated = make_index();
    // consolidated DocId of each shard's DocId
    std::vector<ix::DocId> a_to_all, b_to_all;

    SkewedCorpus() {
        auto add = [&](ix::MemoryIndex& shard, std::vector<ix::DocId>& map,
                       const ix::Document& doc) {
            shard.add(doc);
            map.push_back(consolidated.add(doc));
        };
        add(shard_a, a_to_all, {{"title", "notes"}, {"body", body_with(1)}});
        for (int i = 0; i < 9; ++i) {
            add(shard_a, a_to_all, {{"title", "notes"}, {"body", "boil water pasta salt basil"}});
        }
        for (int tf = 5; tf >= 1; --tf) {
            add(shard_b, b_to_all, {{"title", "notes"}, {"body", body_with(tf)}});
        }
    }
};

}  // namespace

TEST_CASE("stats reports document count, term frequencies and field lengths", "[index][stats]") {
    auto idx = make_index();
    idx.add({{"title", "Postgres tuning"}, {"body", "postgres vacuum"}});
    idx.add({{"title", "Cooking"}, {"body", "boil water pasta"}});

    const ix::CorpusStats s = idx.stats("postgres unseenterm");
    REQUIRE(s.doc_count == 2);
    // Keyed by the analyzed (stemmed) term; an unseen term is reported as 0 so a
    // merge covers every query term.
    REQUIRE(s.term_doc_freq.size() == 2);
    std::uint64_t seen = 0, unseen = 0;
    for (const auto& [term, df] : s.term_doc_freq) (df == 1 ? seen : unseen) += 1;
    REQUIRE(seen == 1);
    REQUIRE(unseen == 1);
    REQUIRE(s.fields.at("title").doc_count == 2);
    REQUIRE(s.fields.at("title").total_len == 3);  // "postgr tune" + "cook"
    REQUIRE(s.fields.at("body").total_len == 5);
}

TEST_CASE("merged shard statistics equal the consolidated index's", "[index][stats]") {
    SkewedCorpus c;
    ix::CorpusStats merged = c.shard_a.stats("postgres");
    merged.merge(c.shard_b.stats("postgres"));
    REQUIRE(merged == c.consolidated.stats("postgres"));
}

TEST_CASE("search with an index's own statistics equals plain search", "[index][stats]") {
    SkewedCorpus c;
    const auto plain = c.shard_b.search("postgres", 0);
    const auto with_own = c.shard_b.search("postgres", 0, c.shard_b.stats("postgres"));
    REQUIRE(plain.hits.size() == with_own.hits.size());
    for (std::size_t i = 0; i < plain.hits.size(); ++i) {
        REQUIRE(plain.hits[i].doc_id == with_own.hits[i].doc_id);
        REQUIRE(plain.hits[i].score == with_own.hits[i].score);
    }
}

TEST_CASE("shards scored with merged statistics rank exactly like one index", "[index][stats]") {
    SkewedCorpus c;
    ix::CorpusStats global = c.shard_a.stats("postgres");
    global.merge(c.shard_b.stats("postgres"));

    // The consolidated scores, by consolidated DocId.
    std::unordered_map<ix::DocId, double> oracle;
    for (const auto& hit : c.consolidated.search("postgres", 0).hits) oracle[hit.doc_id] = hit.score;
    REQUIRE(oracle.size() == 6);

    // Every shard hit scored with the globals equals the consolidated score exactly.
    for (const auto& hit : c.shard_a.search("postgres", 0, global).hits) {
        REQUIRE(hit.score == oracle.at(c.a_to_all[hit.doc_id]));
    }
    for (const auto& hit : c.shard_b.search("postgres", 0, global).hits) {
        REQUIRE(hit.score == oracle.at(c.b_to_all[hit.doc_id]));
    }

    // Local statistics invert the order: shard A's single tf-1 match outscores
    // shard B's tf-5 match, which is the defect the globals remove.
    const double a_local = c.shard_a.search("postgres", 1).hits.at(0).score;
    const double b_local = c.shard_b.search("postgres", 1).hits.at(0).score;
    REQUIRE(a_local > b_local);
    const double a_global = c.shard_a.search("postgres", 1, global).hits.at(0).score;
    const double b_global = c.shard_b.search("postgres", 1, global).hits.at(0).score;
    REQUIRE(b_global > a_global);
}
