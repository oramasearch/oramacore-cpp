#include "oramacore/bm25f.hpp"

#include <cmath>

namespace oramacore::bm25f {

double idf(std::uint64_t total_docs, std::uint64_t docs_with_term) {
    // ln(1 + (N − n + 0.5) / (n + 0.5)) — always positive, matches OramaCore.
    const double n = static_cast<double>(docs_with_term);
    const double big_n = static_cast<double>(total_docs);
    return std::log(1.0 + (big_n - n + 0.5) / (n + 0.5));
}

double pseudo_tf(
    const std::vector<FieldHit>& hits,
    const std::unordered_map<std::uint32_t, FieldParams>& params,
    const std::unordered_map<std::uint32_t, FieldStats>& stats) {
    double acc = 0.0;
    for (const auto& h : hits) {
        const auto pit = params.find(h.field_id);
        const auto sit = stats.find(h.field_id);
        const FieldParams fp = (pit != params.end()) ? pit->second : FieldParams{};
        const double avg_len = (sit != stats.end()) ? sit->second.avg_len : 0.0;

        // Avoid divide-by-zero before any docs are indexed for this field.
        const double len_ratio = (avg_len > 0.0)
            ? (static_cast<double>(h.field_len) / avg_len)
            : 1.0;
        const double denom = 1.0 - fp.b + fp.b * len_ratio;
        if (denom <= 0.0) continue;  // pathological b/len; skip rather than blow up
        acc += fp.weight * (static_cast<double>(h.term_freq) / denom);
    }
    return acc;
}

double score_term(double idf_value, double pseudo_tf_value, const Params& params) {
    if (pseudo_tf_value <= 0.0) return 0.0;
    return idf_value * (pseudo_tf_value / (params.k1 + pseudo_tf_value));
}

double score(
    std::uint64_t total_docs,
    std::uint64_t docs_with_term,
    const std::vector<FieldHit>& hits,
    const std::unordered_map<std::uint32_t, FieldParams>& params,
    const std::unordered_map<std::uint32_t, FieldStats>& stats,
    const Params& global) {
    return score_term(idf(total_docs, docs_with_term),
                      pseudo_tf(hits, params, stats), global);
}

}  // namespace oramacore::bm25f
