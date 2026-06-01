#pragma once
// MemoryIndex — a minimal in-memory BM25F full-text index: add documents,
// search them. Ties together the tokenizer + posting lists + per-field stats +
// the BM25F scorer. This is the "uncommitted" (write-optimized) index of the
// two-index pattern; the committed/mmap tier + async search (Boost.Capy, D23)
// come in later milestones (#98/#99). Synchronous + pure-ish for now so the
// experiment GUI can call it directly and it's trivially testable.
//
// A document is a set of named fields (e.g. "title", "body"); BM25F weights and
// length-normalizes per field (that's the whole point — see bm25f.hpp).
// No exceptions across the API (D7/D16).
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "oramacore/bm25f.hpp"
#include "oramacore/error.hpp"
#include "oramacore/tokenize.hpp"

namespace oramacore::index {

using DocId = std::uint32_t;

// Per-field configuration: its BM25F weight + length-norm, and a stable id.
struct FieldConfig {
    std::string name;
    bm25f::FieldParams params{};  // weight, b
};

// A document to index: field name -> raw text.
using Document = std::unordered_map<std::string, std::string>;

// One search hit.
struct Hit {
    DocId doc_id = 0;
    double score = 0.0;
};

struct SearchResult {
    std::vector<Hit> hits;          // sorted by score desc
    std::uint64_t total_matched = 0;
};

// Minimal in-memory BM25F index.
class MemoryIndex {
public:
    // Declare known fields up front to give them custom BM25F params (e.g. a
    // boosted "title"). Any other field a document carries is auto-registered
    // with defaults (weight 1.0, b 0.75) — so arbitrary document shapes index.
    explicit MemoryIndex(std::vector<FieldConfig> fields,
                         bm25f::Params global = {},
                         tokenize::Options tok = {});

    // Index a document; returns its assigned DocId. (Append-only for now —
    // delete/upsert is a later milestone.)
    DocId add(const Document& doc);

    // Search across all fields for `query` (tokenized the same way as docs).
    // Multi-term: per-term BM25F scores are summed per document. `top_k` caps
    // the returned hits (0 = all).
    [[nodiscard]] SearchResult search(std::string_view query, std::size_t top_k = 10) const;

    [[nodiscard]] std::uint64_t doc_count() const { return doc_count_; }
    [[nodiscard]] std::size_t field_count() const { return fields_.size(); }

private:
    struct Posting {
        DocId doc_id;
        std::uint32_t field_id;
        std::uint32_t term_freq;
        std::uint32_t field_len;  // token count of that field in that doc
    };

    std::vector<FieldConfig> fields_;
    std::unordered_map<std::string, std::uint32_t> field_id_;       // name -> id
    bm25f::Params global_;
    tokenize::Options tok_;

    // term -> postings (across all fields/docs)
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    // term -> # distinct docs containing it (for IDF)
    std::unordered_map<std::string, std::uint64_t> doc_freq_;

    // per-field running totals to derive avglen_f at search time
    std::unordered_map<std::uint32_t, std::uint64_t> field_total_len_;
    std::unordered_map<std::uint32_t, std::uint64_t> field_doc_count_;

    std::uint64_t doc_count_ = 0;

    // Flat per-field scoring caches (indexed by field_id), rebuilt on add().
    // They let search() stay allocation-free AND fully read-only — so it's safe
    // to call concurrently (e.g. from the decode-loop coroutines).
    std::vector<bm25f::FieldParams> params_cache_;
    std::vector<bm25f::FieldStats> stats_cache_;
    void rebuild_field_caches();
};

}  // namespace oramacore::index
