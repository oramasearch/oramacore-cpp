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
            postings_[term].postings.push_back(Posting{
                .doc_id = id, .field_id = field_id,
                .term_freq = freq, .field_len = field_len});
            doc_terms.insert(term);
        }

        field_total_len_[field_id] += field_len;
        field_doc_count_[field_id] += 1;
    }

    for (const auto& term : doc_terms) ++postings_[term].doc_freq;
    ++doc_count_;
    rebuild_field_caches();  // keep flat scoring caches current (cheap, O(fields))
    return id;
}

MemoryIndex::FieldScore MemoryIndex::field_score(const FieldConfig& field,
                                                double avg_len) {
    const double b = field.params.b;
    FieldScore fs;
    fs.weight = field.params.weight;
    // denom = 1 − b + b·(len/avglen) = (1−b) + (b/avglen)·len  →  c0 + c1·len.
    // avglen == 0 (field unseen) ⇒ len_ratio defined as 1 ⇒ denom == 1.
    if (avg_len > 0.0) { fs.c0 = 1.0 - b; fs.c1 = b / avg_len; }
    else { fs.c0 = 1.0; fs.c1 = 0.0; }
    return fs;
}

void MemoryIndex::rebuild_field_caches() {
    fscore_.resize(fields_.size());
    for (std::uint32_t i = 0; i < fields_.size(); ++i) {
        const auto tit = field_total_len_.find(i);
        const auto cit = field_doc_count_.find(i);
        double avg = 0.0;
        if (tit != field_total_len_.end() && cit != field_doc_count_.end() && cit->second > 0) {
            avg = static_cast<double>(tit->second) / static_cast<double>(cit->second);
        }
        fscore_[i] = field_score(fields_[i], avg);
    }
}

void CorpusStats::merge(const CorpusStats& other) {
    doc_count += other.doc_count;
    for (const auto& [term, df] : other.term_doc_freq) term_doc_freq[term] += df;
    for (const auto& [name, lengths] : other.fields) {
        FieldLengthStats& mine = fields[name];
        mine.total_len += lengths.total_len;
        mine.doc_count += lengths.doc_count;
    }
}

CorpusStats MemoryIndex::stats(std::string_view query) const {
    CorpusStats out;
    out.doc_count = doc_count_;
    std::vector<tokenize::Token> q_tokens;
    tokenize::tokenize_into(query, tok_, q_tokens);
    for (const auto& qt : q_tokens) {
        const auto pit = postings_.find(qt.text);
        out.term_doc_freq[qt.text] = (pit == postings_.end()) ? 0 : pit->second.doc_freq;
    }
    for (std::uint32_t i = 0; i < fields_.size(); ++i) {
        FieldLengthStats lengths;
        if (const auto tit = field_total_len_.find(i); tit != field_total_len_.end()) {
            lengths.total_len = tit->second;
        }
        if (const auto cit = field_doc_count_.find(i); cit != field_doc_count_.end()) {
            lengths.doc_count = cit->second;
        }
        out.fields[fields_[i].name] = lengths;
    }
    return out;
}

SearchResult MemoryIndex::search(std::string_view query, std::size_t top_k) const {
    return search_impl(query, top_k, nullptr);
}

SearchResult MemoryIndex::search(std::string_view query, std::size_t top_k,
                                 const CorpusStats& global) const {
    return search_impl(query, top_k, &global);
}

SearchResult MemoryIndex::search_impl(std::string_view query, std::size_t top_k,
                                      const CorpusStats* global) const {
    SearchResult result;
    if (doc_count_ == 0) return result;

    // The corpus statistics to score with: this index's own, or the supplied
    // global view (falling back to our own for anything it does not carry).
    const std::uint64_t total_docs =
        (global != nullptr && global->doc_count > 0) ? global->doc_count : doc_count_;
    thread_local std::vector<FieldScore> global_fscore;  // reused per query
    if (global != nullptr) {
        global_fscore = fscore_;
        for (std::uint32_t i = 0; i < fields_.size(); ++i) {
            const auto fit = global->fields.find(fields_[i].name);
            if (fit == global->fields.end()) continue;  // keep our own
            const double avg = fit->second.doc_count > 0
                ? static_cast<double>(fit->second.total_len) /
                      static_cast<double>(fit->second.doc_count)
                : 0.0;
            global_fscore[i] = field_score(fields_[i], avg);
        }
    }
    const std::vector<FieldScore>& fscore = (global != nullptr) ? global_fscore : fscore_;

    // Search is read-only on the index (concurrency-safe); all scratch is
    // per-thread + reused, so there's no heap allocation per query after warmup.
    thread_local std::vector<tokenize::Token> q_tokens;  // reused query-token buffer
    tokenize::tokenize_into(query, tok_, q_tokens);

    // Resolve query terms to posting lists once (a single hash lookup per term).
    struct ActiveTerm { double idf; const std::vector<Posting>* postings; };
    thread_local std::vector<ActiveTerm> active;
    active.clear();
    for (const auto& qt : q_tokens) {
        const auto pit = postings_.find(qt.text);
        if (pit == postings_.end()) continue;
        std::uint64_t doc_freq = pit->second.doc_freq;
        if (global != nullptr) {
            if (const auto git = global->term_doc_freq.find(qt.text);
                git != global->term_doc_freq.end()) {
                doc_freq = git->second;
            }
        }
        active.push_back(ActiveTerm{bm25f::idf(total_docs, doc_freq), &pit->second.postings});
    }
    if (active.empty()) return result;

    // Final ranking order, and the "worst-on-top" order for the bounded top-k
    // heap (so we can compare a candidate against the current worst and evict).
    const auto better = [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.doc_id < b.doc_id;
    };
    const auto worse_than = [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.doc_id > b.doc_id;
    };
    thread_local std::vector<Hit> heap;  // bounded to top_k (worst on top); reused
    heap.clear();
    const bool bounded = (top_k > 0);
    const auto consider = [&](DocId doc, double s) {
        if (s <= 0.0) return;
        const Hit h{.doc_id = doc, .score = s};
        if (!bounded) { heap.push_back(h); return; }  // top_k == 0 means "all"
        if (heap.size() < top_k) {
            heap.push_back(h);
            std::push_heap(heap.begin(), heap.end(), worse_than);
        } else if (worse_than(heap.front(), h)) {  // h ranks above the current worst
            std::pop_heap(heap.begin(), heap.end(), worse_than);
            heap.back() = h;
            std::push_heap(heap.begin(), heap.end(), worse_than);
        }
    };

    // Score one doc's contiguous posting run for one term (advances i). Postings
    // for a term are doc-ascending and same-doc-contiguous (append-only add()),
    // so a doc's fields are adjacent. Sum field contributions BEFORE saturation.
    const auto score_run = [&](const std::vector<Posting>& plist, std::size_t& i,
                               std::size_t n) -> double {
        const DocId doc = plist[i].doc_id;
        double tf_tilde = 0.0;
        while (i < n && plist[i].doc_id == doc) {
            const Posting& p = plist[i];
            const FieldScore& fs = fscore[p.field_id];
            const double denom = fs.c0 + fs.c1 * static_cast<double>(p.field_len);
            if (denom > 0.0) tf_tilde += fs.weight * (static_cast<double>(p.term_freq) / denom);
            ++i;
        }
        return tf_tilde;
    };

    std::uint64_t matched = 0;

    if (active.size() == 1) {
        // Single-term fast path: sequential read of one posting list straight
        // into the bounded heap — no score buffer, no random scatter, no sort
        // of the full candidate set.
        const double idf = active[0].idf;
        const auto& plist = *active[0].postings;
        const std::size_t n = plist.size();
        for (std::size_t i = 0; i < n;) {
            const DocId doc = plist[i].doc_id;
            const double tf_tilde = score_run(plist, i, n);
            ++matched;
            consider(doc, bm25f::score_term(idf, tf_tilde, global_));
        }
    } else {
        // Multi-term: accumulate per-doc scores across terms, then select. The
        // dense score buffer is the only place a scatter is unavoidable.
        thread_local std::vector<double> scores;      // doc_id -> accumulated score
        thread_local std::vector<std::uint8_t> seen;  // doc_id -> touched this query
        thread_local std::vector<DocId> touched;      // the doc_ids we touched
        if (scores.size() < doc_count_) { scores.assign(doc_count_, 0.0); seen.assign(doc_count_, 0); }
        touched.clear();
        for (const auto& at : active) {
            const auto& plist = *at.postings;
            const std::size_t n = plist.size();
            for (std::size_t i = 0; i < n;) {
                const DocId doc = plist[i].doc_id;
                const double tf_tilde = score_run(plist, i, n);
                if (!seen[doc]) { seen[doc] = 1; touched.push_back(doc); }
                scores[doc] += bm25f::score_term(at.idf, tf_tilde, global_);
            }
        }
        matched = touched.size();
        for (const DocId doc : touched) {
            consider(doc, scores[doc]);
            scores[doc] = 0.0;  // reset scratch for the next query
            seen[doc] = 0;
        }
    }

    result.total_matched = matched;  // distinct docs that matched a query term
    result.hits.assign(heap.begin(), heap.end());
    std::sort(result.hits.begin(), result.hits.end(), better);
    return result;
}

}  // namespace oramacore::index
