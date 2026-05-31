#include "oramacore/tokenize.hpp"

#include <array>
#include <cctype>
#include <string>

namespace oramacore::tokenize {

namespace {

// A compact English stop-word set. Not exhaustive — the common closed-class
// words that hurt BM25 signal. (Lowercased.)
bool english_stop(std::string_view w) {
    static constexpr std::array<std::string_view, 40> kStop{
        "a", "an", "and", "are", "as", "at", "be", "but", "by", "for",
        "if", "in", "into", "is", "it", "no", "not", "of", "on", "or",
        "such", "that", "the", "their", "then", "there", "these", "they",
        "this", "to", "was", "will", "with", "we", "you", "i", "he", "she",
        "from", "has",
    };
    for (auto s : kStop) {
        if (w == s) return true;
    }
    return false;
}

bool is_word_byte(unsigned char c) {
    // alphanumeric ASCII, or any non-ASCII byte (keep UTF-8 multibyte words).
    return std::isalnum(c) != 0 || c >= 0x80;
}

}  // namespace

std::string normalize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) {
        out.push_back(static_cast<char>(c < 0x80 ? std::tolower(c) : c));
    }
    return out;
}

bool is_stop_word(std::string_view word, Locale locale) {
    switch (locale) {
        case Locale::English:
            return english_stop(word);
    }
    return false;
}

// Minimal English suffix stemmer. A tiny, conservative subset of Porter:
// strip a few common inflectional suffixes, guarding a minimum stem length so
// we don't mangle short words. NOT full Snowball (see header caveat).
std::string stem(std::string_view word, Locale locale) {
    if (locale != Locale::English) return std::string(word);
    std::string w(word);
    const auto ends_with = [&](std::string_view suf) {
        return w.size() >= suf.size() &&
               w.compare(w.size() - suf.size(), suf.size(), suf) == 0;
    };
    const auto strip = [&](std::string_view suf, std::size_t min_stem) {
        if (ends_with(suf) && w.size() - suf.size() >= min_stem) {
            w.resize(w.size() - suf.size());
            return true;
        }
        return false;
    };

    // order matters: longest/most-specific first
    if (strip("ingly", 3) || strip("edly", 3)) { /* fell through to base */ }
    else if (strip("ing", 3)) {}
    else if (strip("ies", 2)) { w += "y"; }   // "stories" -> "story"
    else if (strip("ied", 2)) { w += "y"; }   // "studied" -> "study"
    else if (strip("ess", 4)) {}              // keep "less"/"ness" stems sane via min
    else if (strip("ed", 3)) {}
    else if (strip("ly", 3)) {}
    else if (strip("es", 3)) {}
    else if (!ends_with("ss") && strip("s", 2)) {}  // plural -s, but never -ss ("less" stays "less")
    return w;
}

std::vector<Token> tokenize(std::string_view text, const Options& opts) {
    std::vector<Token> tokens;
    const std::string norm = opts.lowercase ? normalize(text) : std::string(text);

    std::size_t i = 0;
    const std::size_t n = norm.size();
    while (i < n) {
        // skip separators
        while (i < n && !is_word_byte(static_cast<unsigned char>(norm[i]))) ++i;
        const std::size_t start = i;
        while (i < n && is_word_byte(static_cast<unsigned char>(norm[i]))) ++i;
        if (i == start) break;

        std::string_view raw(norm.data() + start, i - start);
        if (opts.remove_stop_words && is_stop_word(raw, opts.locale)) continue;

        std::string tok = opts.stem ? stem(raw, opts.locale) : std::string(raw);
        if (tok.size() < opts.min_token_len) continue;

        tokens.push_back(Token{
            .text = std::move(tok),
            .start = static_cast<std::uint32_t>(start),
            .length = static_cast<std::uint32_t>(i - start),
        });
    }
    return tokens;
}

}  // namespace oramacore::tokenize
