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

// One field's length totals: the sum of its token counts over the documents
// that carry it, and how many documents carry it. avglen_f = total_len / doc_count.
struct FieldLengthStats {
    std::uint64_t total_len = 0;
    std::uint64_t doc_count = 0;

    bool operator==(const FieldLengthStats&) const = default;
};

// The corpus statistics BM25F scores with: the document count N, each query
// term's document frequency n (keyed by the ANALYZED term — tokenized, stemmed),
// and each field's length totals (keyed by field name). A sharded search gathers
// one CorpusStats per shard (MemoryIndex::stats), merges them (merge() sums every
// count), and hands the merged view back to every shard's search(): each shard
// then scores exactly as one index over the whole corpus would.
struct CorpusStats {
    std::uint64_t doc_count = 0;
    std::unordered_map<std::string, std::uint64_t> term_doc_freq;
    std::unordered_map<std::string, FieldLengthStats> fields;

    // Add another shard's statistics into this one (every count sums; shards
    // hold disjoint documents).
    void merge(const CorpusStats& other);

    bool operator==(const CorpusStats&) const = default;
};

// Bounds on the fields an index registers (oramacore-cpp#4). Field names come
// from the documents, so their number and length are input: without a bound,
// one document with a million keys, or a million documents with one new key
// each, makes every later add(), stats() and sharded search() pay for them.
struct Limits {
    std::size_t max_fields = 1024;               // registered fields, declared + auto
    std::size_t max_fields_per_document = 512;   // fields one document may carry
    std::size_t max_field_name_length = 256;     // bytes in an auto-registered name
};

// Minimal in-memory BM25F index.
class MemoryIndex {
public:
    // Declare known fields up front to give them custom BM25F params (e.g. a
    // boosted "title"). Any other field a document carries is auto-registered
    // with defaults (weight 1.0, b 0.75) — so arbitrary document shapes index —
    // within `limits`.
    explicit MemoryIndex(std::vector<FieldConfig> fields,
                         bm25f::Params global = {},
                         tokenize::Options tok = {},
                         Limits limits = {});

    // Index a document; returns its assigned DocId. (Append-only for now —
    // delete/upsert is a later milestone.) Fails, leaving the index unchanged,
    // when the document carries more than `max_fields_per_document` fields, a
    // new field name longer than `max_field_name_length`, or new fields that
    // would take the index past `max_fields`. Cost is O(the document's fields),
    // not O(every registered field).
    [[nodiscard]] Result<DocId> add(const Document& doc);

    // Search across all fields for `query` (tokenized the same way as docs).
    // Multi-term: per-term BM25F scores are summed per document. `top_k` caps
    // the returned hits (0 = all).
    [[nodiscard]] SearchResult search(std::string_view query, std::size_t top_k = 10) const;

    // This index's corpus statistics for `query`: the document count, the
    // document frequency of every analyzed query term (0 for a term this index
    // has never seen, so a merge covers every term), and the length totals of
    // every field this index knows.
    [[nodiscard]] CorpusStats stats(std::string_view query) const;

    // search() scored with externally supplied corpus statistics instead of this
    // index's own: IDF reads `global.doc_count` and `global.term_doc_freq`, and
    // the length normalization reads `global.fields`. Pass the merge of every
    // shard's stats() and each shard ranks exactly as one consolidated index over
    // all of them would, so a per-shard `top_k` cutoff drops nothing that belongs
    // in the global top k. A term, field or document count `global` does not
    // carry falls back to this index's own value.
    [[nodiscard]] SearchResult search(std::string_view query, std::size_t top_k,
                                      const CorpusStats& global) const;

    [[nodiscard]] std::uint64_t doc_count() const { return doc_count_; }
    [[nodiscard]] std::size_t field_count() const { return fields_.size(); }
    [[nodiscard]] const Limits& limits() const { return limits_; }

private:
    struct Posting {
        DocId doc_id;
        std::uint32_t field_id;
        std::uint32_t term_freq;
        std::uint32_t field_len;  // token count of that field in that doc
    };
    // A term's full entry: its postings + the doc-frequency (for IDF). Folding
    // df in here means search does ONE hash lookup per query term, not two.
    struct PostingList {
        std::vector<Posting> postings;
        std::uint64_t doc_freq = 0;  // # distinct docs containing the term
    };

    std::vector<FieldConfig> fields_;
    std::unordered_map<std::string, std::uint32_t> field_id_;       // name -> id
    bm25f::Params global_;
    tokenize::Options tok_;
    Limits limits_;

    // term -> {postings across all fields/docs, doc-frequency}
    std::unordered_map<std::string, PostingList> postings_;

    // per-field running totals to derive avglen_f at search time
    std::unordered_map<std::uint32_t, std::uint64_t> field_total_len_;
    std::unordered_map<std::uint32_t, std::uint64_t> field_doc_count_;

    std::uint64_t doc_count_ = 0;

    // Flat per-field scoring cache (indexed by field_id), kept current by add()
    // for the fields each document touches (the others' averages do not move). The
    // BM25F length-normalization is precomputed into denom = c0 + c1·field_len,
    // so the hot scoring loop has NO per-posting divide for normalization (the
    // b/avglen divide is hoisted here, once per build). This is also the
    // branchless, gather-free form a NEON kernel would vectorize. Keeps search()
    // allocation-free + read-only (safe to call concurrently).
    struct FieldScore {
        double weight = 1.0;  // w_f
        double c0 = 1.0;      // 1 − b_f         (or 1.0 when avglen_f == 0)
        double c1 = 0.0;      // b_f / avglen_f  (or 0.0 when avglen_f == 0)
    };
    std::vector<FieldScore> fscore_;
    void rebuild_field_caches();            // every field: construction only
    void update_field_cache(std::uint32_t field_id);  // one field: add()

    // One field's scoring constants from its params and average length.
    static FieldScore field_score(const FieldConfig& field, double avg_len);

    // The scoring loop shared by both search() overloads: `global` null scores
    // with this index's own statistics.
    SearchResult search_impl(std::string_view query, std::size_t top_k,
                             const CorpusStats* global) const;
};

}  // namespace oramacore::index
