#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <unordered_map>
#include <vector>

#include "oramacore/bm25f.hpp"

using Catch::Matchers::WithinAbs;
namespace bm = oramacore::bm25f;

TEST_CASE("IDF is positive and decreases with document frequency", "[bm25f]") {
    // Rarer term (fewer docs contain it) => higher IDF.
    const double rare = bm::idf(/*N=*/1000, /*n=*/1);
    const double common = bm::idf(/*N=*/1000, /*n=*/500);
    REQUIRE(rare > common);
    REQUIRE(common > 0.0);
}

TEST_CASE("pseudo_tf applies per-field weight", "[bm25f]") {
    // Two fields, same tf and length == avg_len (so length-norm denom == 1),
    // but field 1 is weighted 2x. Its contribution should be doubled.
    std::unordered_map<std::uint32_t, bm::FieldParams> params{
        {0, {.weight = 1.0, .b = 0.75}},
        {1, {.weight = 2.0, .b = 0.75}},
    };
    std::unordered_map<std::uint32_t, bm::FieldStats> stats{
        {0, {.avg_len = 10.0}},
        {1, {.avg_len = 10.0}},
    };
    const double tf0 = bm::pseudo_tf({{.field_id = 0, .term_freq = 3, .field_len = 10}}, params, stats);
    const double tf1 = bm::pseudo_tf({{.field_id = 1, .term_freq = 3, .field_len = 10}}, params, stats);
    REQUIRE_THAT(tf1, WithinAbs(2.0 * tf0, 1e-9));
    // length == avg_len => denom == 1 => tf~ == weight * tf
    REQUIRE_THAT(tf0, WithinAbs(3.0, 1e-9));
}

TEST_CASE("per-field length normalization penalizes long fields", "[bm25f]") {
    // Same field, same tf, but one doc's field is longer than average => lower tf~.
    std::unordered_map<std::uint32_t, bm::FieldParams> params{{0, {.weight = 1.0, .b = 0.75}}};
    std::unordered_map<std::uint32_t, bm::FieldStats> stats{{0, {.avg_len = 10.0}}};
    const double short_doc = bm::pseudo_tf({{.field_id = 0, .term_freq = 2, .field_len = 5}}, params, stats);
    const double long_doc = bm::pseudo_tf({{.field_id = 0, .term_freq = 2, .field_len = 40}}, params, stats);
    REQUIRE(short_doc > long_doc);
}

TEST_CASE("BM25F sums field contributions before saturation", "[bm25f]") {
    // The whole point of BM25F: combine fields into one pseudo-tf, THEN saturate.
    // Verify score == idf * tf~/(k1+tf~) with tf~ being the cross-field sum.
    std::unordered_map<std::uint32_t, bm::FieldParams> params{
        {0, {.weight = 1.0, .b = 0.0}},   // b=0 => no length norm, tf~ == weight*tf
        {1, {.weight = 1.0, .b = 0.0}},
    };
    std::unordered_map<std::uint32_t, bm::FieldStats> stats{
        {0, {.avg_len = 10.0}}, {1, {.avg_len = 10.0}}};
    std::vector<bm::FieldHit> hits{
        {.field_id = 0, .term_freq = 2, .field_len = 10},
        {.field_id = 1, .term_freq = 1, .field_len = 10},
    };
    const double tf = bm::pseudo_tf(hits, params, stats);
    REQUIRE_THAT(tf, WithinAbs(3.0, 1e-9));  // 2 + 1, no length norm

    const bm::Params g{.k1 = 1.2};
    const double idfv = bm::idf(1000, 10);
    const double expected = idfv * (3.0 / (1.2 + 3.0));
    const double got = bm::score(1000, 10, hits, params, stats, g);
    REQUIRE_THAT(got, WithinAbs(expected, 1e-9));
}

TEST_CASE("zero term frequency scores zero", "[bm25f]") {
    std::unordered_map<std::uint32_t, bm::FieldParams> params{{0, {}}};
    std::unordered_map<std::uint32_t, bm::FieldStats> stats{{0, {.avg_len = 10.0}}};
    REQUIRE_THAT(bm::score(1000, 10, {}, params, stats, {}), WithinAbs(0.0, 1e-12));
}
