// SPDX-License-Identifier: MIT

#include <catch2/catch_test_macros.hpp>

#include <chrono>
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
    REQUIRE(idx.add({{"title", "Postgres tuning guide"}, {"body", "how to tune a postgres database for speed"}}).has_value());
    REQUIRE(idx.add({{"title", "Cooking pasta"},         {"body", "boil water, add salt, add pasta"}}).has_value());
    REQUIRE(idx.add({{"title", "Database indexes"},       {"body", "btree, gin, brin, and postgres indexes explained"}}).has_value());

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
    const auto a = idx.add({{"title", "widget"},      {"body", "nothing relevant here at all"}}).value(); // term in title
    const auto b = idx.add({{"title", "unrelated"},   {"body", "widget appears only in the body text"}}).value(); // term in body

    auto r = idx.search("widget");
    REQUIRE(r.hits.size() == 2);
    // title (weight 3) should outrank body (weight 1) for the same term
    REQUIRE(r.hits[0].doc_id == a);
    REQUIRE(r.hits[1].doc_id == b);
}

TEST_CASE("multi-term query sums per-term scores", "[index]") {
    auto idx = make_index();
    const auto both = idx.add({{"title", "fast postgres"}, {"body", "fast postgres queries"}}).value();
    const auto one  = idx.add({{"title", "fast cars"},     {"body", "fast cars go fast"}}).value();

    auto r = idx.search("fast postgres");
    REQUIRE(r.hits.size() == 2);
    // doc with both query terms should outrank the doc with only "fast"
    REQUIRE(r.hits[0].doc_id == both);
    REQUIRE(r.hits[1].doc_id == one);
}

TEST_CASE("search uses the same tokenization as indexing (stemming/stop-words)", "[index]") {
    auto idx = make_index();
    REQUIRE(idx.add({{"title", "Running servers"}, {"body", "the servers are running quickly"}}).has_value());
    // "server" (stem of servers) and "runn" (stem of running) should match;
    // "the"/"are" are stop-words and shouldn't matter.
    REQUIRE(idx.search("server").hits.size() == 1);
    REQUIRE(idx.search("running").hits.size() == 1);   // running -> runn matches
    REQUIRE(idx.search("the").hits.empty());            // stop word -> no tokens -> no hits
}

TEST_CASE("empty index and no-match queries are safe", "[index]") {
    auto idx = make_index();
    REQUIRE(idx.search("anything").hits.empty());
    REQUIRE(idx.add({{"title", "hello"}, {"body", "world"}}).has_value());
    REQUIRE(idx.search("nonexistentterm").hits.empty());
    REQUIRE(idx.search("hello").hits.size() == 1);
}

TEST_CASE("top_k keeps the best hits whatever order they arrive in", "[index]") {
    // Postings are scanned in DocId order, so inserting weakest-first makes the
    // hits arrive in ascending score: the bounded heap must evict the worst kept
    // hit, never the best.
    auto ascending = make_index();
    std::vector<ix::DocId> ids;
    for (int tf = 1; tf <= 5; ++tf) {
        std::string body;
        for (int i = 0; i < 5; ++i) body += (i < tf) ? "postgres " : "filler ";
        ids.push_back(ascending.add({{"title", "notes"}, {"body", body}}).value());
    }
    const auto all = ascending.search("postgres", 0);
    REQUIRE(all.hits.size() == 5);
    REQUIRE(all.hits[0].doc_id == ids[4]);  // tf 5 ranks first
    for (std::size_t k = 1; k <= 5; ++k) {
        const auto top = ascending.search("postgres", k);
        REQUIRE(top.hits.size() == k);
        for (std::size_t i = 0; i < k; ++i) {
            REQUIRE(top.hits[i].doc_id == all.hits[i].doc_id);
            REQUIRE(top.hits[i].score == all.hits[i].score);
        }
    }

    // A tie at the cutoff keeps the lower DocIds (the documented tie-break).
    auto ties = make_index();
    for (int i = 0; i < 4; ++i) REQUIRE(ties.add({{"body", "common term here"}}).has_value());
    const auto two = ties.search("common", 2);
    REQUIRE(two.hits.size() == 2);
    REQUIRE(two.hits[0].doc_id == 0);
    REQUIRE(two.hits[1].doc_id == 1);
}

TEST_CASE("top_k caps results", "[index]") {
    auto idx = make_index();
    for (int i = 0; i < 5; ++i) REQUIRE(idx.add({{"body", "common term here"}}).has_value());
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
            REQUIRE(shard.add(doc).has_value());
            map.push_back(consolidated.add(doc).value());
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
    REQUIRE(idx.add({{"title", "Postgres tuning"}, {"body", "postgres vacuum"}}).has_value());
    REQUIRE(idx.add({{"title", "Cooking"}, {"body", "boil water pasta"}}).has_value());

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

// ----- field limits (oramacore-cpp#4) ------------------------------------------
//
// Field names come from the documents, so their number and length are bounded,
// and a document over a limit is refused with the index unchanged.

TEST_CASE("a document past the field limit is refused and the index is unchanged",
          "[index][limits]") {
    ix::MemoryIndex idx({}, {}, {}, ix::Limits{.max_fields = 3});
    REQUIRE(idx.add({{"a", "alpha"}, {"b", "beta"}}).has_value());
    REQUIRE(idx.field_count() == 2);

    const auto refused = idx.add({{"c", "gamma"}, {"d", "delta"}});
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(refused.error().message.find("limit") != std::string::npos);
    REQUIRE(idx.doc_count() == 1);
    REQUIRE(idx.field_count() == 2);
    REQUIRE(idx.search("gamma").hits.empty());
    REQUIRE(idx.stats("gamma").fields.size() == 2);

    // Known fields are not new fields: the index still takes documents within the limit.
    REQUIRE(idx.add({{"a", "gamma"}, {"c", "delta"}}).has_value());
    REQUIRE(idx.field_count() == 3);
    REQUIRE(idx.search("gamma").hits.size() == 1);
}

TEST_CASE("declared fields count toward the field limit", "[index][limits]") {
    ix::MemoryIndex idx({{.name = "title"}, {.name = "body"}}, {}, {},
                        ix::Limits{.max_fields = 2});
    REQUIRE(idx.add({{"title", "x"}, {"body", "y"}}).has_value());
    REQUIRE_FALSE(idx.add({{"extra", "z"}}).has_value());
    REQUIRE(idx.doc_count() == 1);
}

TEST_CASE("one document with more fields than the per-document limit is refused",
          "[index][limits]") {
    ix::MemoryIndex idx({}, {}, {}, ix::Limits{.max_fields_per_document = 4});
    ix::Document wide;
    for (int i = 0; i < 5; ++i) wide["f" + std::to_string(i)] = "text";
    const auto refused = idx.add(wide);
    REQUIRE_FALSE(refused.has_value());
    REQUIRE(idx.doc_count() == 0);
    REQUIRE(idx.field_count() == 0);
}

TEST_CASE("a field name longer than the limit is refused", "[index][limits]") {
    ix::MemoryIndex idx({}, {}, {}, ix::Limits{.max_field_name_length = 8});
    REQUIRE_FALSE(idx.add({{std::string(9, 'n'), "text"}}).has_value());
    REQUIRE(idx.add({{std::string(8, 'n'), "text"}}).has_value());
    REQUIRE(idx.field_count() == 1);
}

TEST_CASE("the default limits allow a million fields and 1 KiB names", "[index][limits]") {
    ix::MemoryIndex idx({});
    REQUIRE(idx.limits().max_fields == 1'000'000);
    REQUIRE(idx.limits().max_fields_per_document == 1'000'000);
    REQUIRE(idx.limits().max_field_name_length == 1024);

    REQUIRE(idx.add({{std::string(1024, 'n'), "v"}}).has_value());
    REQUIRE_FALSE(idx.add({{std::string(1025, 'n'), "v"}}).has_value());
    REQUIRE(idx.field_count() == 1);
}

TEST_CASE("per-field scores stay right when a document touches only some fields",
          "[index][limits]") {
    // Incremental cache updates must give the same scores as a fresh index built
    // in the same order (the full rebuild it replaced recomputed every field).
    auto incremental = make_index();
    REQUIRE(incremental.add({{"title", "postgres"}, {"body", "postgres tuning guide for speed"}}).has_value());
    REQUIRE(incremental.add({{"body", "postgres"}}).has_value());
    REQUIRE(incremental.add({{"title", "a much longer postgres title here"}}).has_value());
    REQUIRE(incremental.add({{"extra", "postgres"}}).has_value());

    const auto r = incremental.search("postgres", 0);
    REQUIRE(r.hits.size() == 4);
    const auto global = incremental.stats("postgres");
    const auto g = incremental.search("postgres", 0, global);
    REQUIRE(g.hits.size() == r.hits.size());
    for (std::size_t i = 0; i < r.hits.size(); ++i) {
        REQUIRE(g.hits[i].doc_id == r.hits[i].doc_id);
        REQUIRE(g.hits[i].score == r.hits[i].score);
    }
}

TEST_CASE("adding documents that each bring one new field grows linearly",
          "[.][index][limits][benchmark]") {
    // Hidden (timing): run with `oramacore-cpp-tests "[benchmark]"`, in a release build.
    // 100,000 documents with one new field each used to cost O(N^2) (every add
    // rebuilt every field's cache). Time the second half against the first: with
    // per-document updates the halves cost about the same; the quadratic version
    // made the second half about three times the first.
    constexpr int kDocs = 100'000;
    ix::MemoryIndex idx({}, {}, {}, ix::Limits{.max_fields = kDocs});
    const auto half = [&](int from, int to) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = from; i < to; ++i) {
            REQUIRE(idx.add({{"f" + std::to_string(i), "word"}}).has_value());
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const double first = half(0, kDocs / 2);
    const double second = half(kDocs / 2, kDocs);
    REQUIRE(idx.field_count() == static_cast<std::size_t>(kDocs));
    REQUIRE(second < first * 2.0 + 0.05);
}
