#pragma once
// BM25F — the first index type ported from OramaCore (the crown-jewel full-text
// scorer; Tommaso: "keep as is"). See docs/bm25f-port-notes.md.
//
// BM25F differs from plain BM25 in TWO ways (this is the whole point):
//   1. per-field WEIGHT      w_f   — boost a field's contribution
//   2. per-field LENGTH-NORM b_f   — normalize by THAT field's avg length
// The per-field contributions are summed into a pseudo-tf BEFORE the saturation
// term is applied (you cannot compute per-field BM25 and add — the math differs):
//
//   tf~(t,d) = Σ_f  w_f · tf_{t,f} / (1 − b_f + b_f · (len_f(d) / avglen_f))
//   score(t,d) = IDF(t) · tf~ / (k1 + tf~)            // global k1 saturation
//
// This header is the scaffold contract: pure, testable scoring math now;
// the posting lists / committed index / async search land later (coroutine-
// shaped via Boost.Capy, D23). No exceptions across the API (D7/D16).
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "oramacore/error.hpp"

namespace oramacore::bm25f {

// Per-field BM25F parameters. Defaults match OramaCore (weight 1.0, b 0.75).
struct FieldParams {
    double weight = 1.0;   // w_f — field boost
    double b = 0.75;       // b_f — per-field length normalization [0,1]
};

// Global saturation parameter (shared across fields, like OramaCore).
struct Params {
    double k1 = 1.2;
};

// Per-field stats the scorer needs (filled from the index at commit time).
struct FieldStats {
    double avg_len = 0.0;  // avglen_f — average length of this field across the corpus
};

// One field's contribution to a term in a document, at score time.
struct FieldHit {
    std::uint32_t field_id = 0;
    std::uint32_t term_freq = 0;   // tf_{t,f} — term freq in this field of this doc
    std::uint32_t field_len = 0;   // len_f(d) — length of this field in this doc
};

// IDF(t) — inverse document frequency for a term. Robertson/Sparck-Jones form,
// matching OramaCore: ln(1 + (N − n + 0.5)/(n + 0.5)).
[[nodiscard]] double idf(std::uint64_t total_docs, std::uint64_t docs_with_term);

// The pseudo-term-frequency tf~ — the BM25F core. Sums per-field weighted,
// length-normalized contributions. `params` and `stats` are keyed by field_id.
[[nodiscard]] double pseudo_tf(
    const std::vector<FieldHit>& hits,
    const std::unordered_map<std::uint32_t, FieldParams>& params,
    const std::unordered_map<std::uint32_t, FieldStats>& stats);

// score(t,d) = IDF · tf~ / (k1 + tf~).
[[nodiscard]] double score_term(
    double idf_value,
    double pseudo_tf_value,
    const Params& params);

// Convenience: full single-term BM25F score for one document.
[[nodiscard]] double score(
    std::uint64_t total_docs,
    std::uint64_t docs_with_term,
    const std::vector<FieldHit>& hits,
    const std::unordered_map<std::uint32_t, FieldParams>& params,
    const std::unordered_map<std::uint32_t, FieldStats>& stats,
    const Params& global);

// ---------------------------------------------------------------------------
// TODO (port milestones — see docs/bm25f-port-notes.md):
//   - Tokenization (port the `nlp` crate: stemming + stop-words).
//   - Posting lists + the uncommitted→committed two-index pattern.
//   - co_await search over the committed index (Boost.Capy task<T>, D23).
//   - Multi-term query scoring + the score-accumulation order.
// ---------------------------------------------------------------------------

}  // namespace oramacore::bm25f
