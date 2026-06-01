#include "oramacore/index.hpp"

#include <algorithm>
#include <set>

namespace oramacore::index {

MemoryIndex::MemoryIndex(std::vector<FieldConfig> fields,
                         bm25f::Params global,
                         tokenize::Options tok)
    : fields_(std::move(fields)), global_(global), tok_(tok) {
    for (std::uint32_t i = 0; i < fields_.size(); ++i) {
        field_id_[fields_[i].name] = i;
    }
}

DocId MemoryIndex::add(const Document& doc) {
    const DocId id = static_cast<DocId>(doc_count_);
    // Track, per document, which terms appeared at all (for doc-frequency).
    std::set<std::string> doc_terms;

    for (const auto& [name, text] : doc) {
        // Auto-register fields on first sight (default params) so any document
        // shape is indexable — fields declared up front keep their custom
        // weight/b; everything else gets sensible defaults.
        std::uint32_t field_id;
        if (const auto fit = field_id_.find(name); fit != field_id_.end()) {
            field_id = fit->second;
        } else {
            field_id = static_cast<std::uint32_t>(fields_.size());
            fields_.push_back(FieldConfig{.name = name, .params = {}});
            field_id_[name] = field_id;
        }

        const auto tokens = tokenize::tokenize(text, tok_);
        const std::uint32_t field_len = static_cast<std::uint32_t>(tokens.size());

        // term -> tf within this field of this doc
        std::unordered_map<std::string, std::uint32_t> tf;
        for (const auto& t : tokens) ++tf[t.text];

        for (const auto& [term, freq] : tf) {
            postings_[term].push_back(Posting{
                .doc_id = id, .field_id = field_id,
                .term_freq = freq, .field_len = field_len});
            doc_terms.insert(term);
        }

        field_total_len_[field_id] += field_len;
        field_doc_count_[field_id] += 1;
    }

    for (const auto& term : doc_terms) ++doc_freq_[term];
    ++doc_count_;
    return id;
}

SearchResult MemoryIndex::search(std::string_view query, std::size_t top_k) const {
    SearchResult result;
    if (doc_count_ == 0) return result;

    // Per-field BM25F params + avglen stats, keyed by field_id.
    std::unordered_map<std::uint32_t, bm25f::FieldParams> params;
    std::unordered_map<std::uint32_t, bm25f::FieldStats> stats;
    for (std::uint32_t i = 0; i < fields_.size(); ++i) {
        params[i] = fields_[i].params;
        const auto tit = field_total_len_.find(i);
        const auto cit = field_doc_count_.find(i);
        double avg = 0.0;
        if (tit != field_total_len_.end() && cit != field_doc_count_.end() && cit->second > 0) {
            avg = static_cast<double>(tit->second) / static_cast<double>(cit->second);
        }
        stats[i] = bm25f::FieldStats{.avg_len = avg};
    }

    const auto q_tokens = tokenize::tokenize(query, tok_);

    // doc_id -> accumulated score across query terms.
    std::unordered_map<DocId, double> scores;

    for (const auto& qt : q_tokens) {
        const auto pit = postings_.find(qt.text);
        if (pit == postings_.end()) continue;

        const auto dfit = doc_freq_.find(qt.text);
        const std::uint64_t df = (dfit != doc_freq_.end()) ? dfit->second : 0;
        const double idf = bm25f::idf(doc_count_, df);

        // Gather this term's per-field hits grouped by doc, then BM25F-score
        // (sum field contributions BEFORE saturation — the BM25F invariant).
        std::unordered_map<DocId, std::vector<bm25f::FieldHit>> by_doc;
        for (const auto& p : pit->second) {
            by_doc[p.doc_id].push_back(bm25f::FieldHit{
                .field_id = p.field_id, .term_freq = p.term_freq, .field_len = p.field_len});
        }
        for (const auto& [doc_id, hits] : by_doc) {
            const double tf_tilde = bm25f::pseudo_tf(hits, params, stats);
            scores[doc_id] += bm25f::score_term(idf, tf_tilde, global_);
        }
    }

    result.total_matched = scores.size();
    result.hits.reserve(scores.size());
    for (const auto& [doc_id, s] : scores) {
        if (s > 0.0) result.hits.push_back(Hit{.doc_id = doc_id, .score = s});
    }
    std::sort(result.hits.begin(), result.hits.end(),
              [](const Hit& a, const Hit& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.doc_id < b.doc_id;  // stable tiebreak
              });
    if (top_k > 0 && result.hits.size() > top_k) result.hits.resize(top_k);
    return result;
}

}  // namespace oramacore::index
