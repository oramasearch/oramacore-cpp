#pragma once
// Tokenization — the front of the full-text path (feeds BM25F posting lists).
// A minimal, custom, dependency-free pipeline (no libstemmer / no Rust nlp dep,
// per the module decision): normalize -> split -> stop-word filter -> stem.
// English-only for now; the Locale knob is in the interface so other languages
// slot in later without an API change.
//
// Pure/synchronous (no Boost.Capy) so it's trivially testable and reusable —
// only the I/O boundary of the engine is coroutine-shaped (D23). No exceptions
// across the API (D7/D16).
//
// "Minimal stemmer" caveat: this is a small suffix-stripper (a tiny subset of
// Porter), NOT full Snowball. Good enough to unblock posting lists (#97) and to
// demo in the experiment UI; swap in libstemmer/Snowball later if parity (#101)
// demands it.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oramacore::tokenize {

enum class Locale {
    English,   // the only fully-wired locale for now
};

struct Options {
    Locale locale = Locale::English;
    bool lowercase = true;       // normalize case
    bool remove_stop_words = true;
    bool stem = true;            // apply the minimal suffix stemmer
    std::uint32_t min_token_len = 1;  // drop shorter tokens after stemming
};

// A produced token + where it came from in the source (for highlighting/debug).
struct Token {
    std::string text;            // the final, normalized+stemmed token
    std::uint32_t start = 0;     // byte offset of the original word in the input
    std::uint32_t length = 0;    // byte length of the original word
};

// The full pipeline: normalize -> split on non-alphanumeric -> (stop-word) ->
// (stem). Returns the surviving tokens in order (duplicates kept — the caller
// counts term frequency).
[[nodiscard]] std::vector<Token> tokenize(std::string_view text, const Options& opts = {});

// Same pipeline, but writes into a caller-owned buffer (cleared + reused) to
// avoid a per-call allocation on hot paths (e.g. tokenizing every query).
void tokenize_into(std::string_view text, const Options& opts, std::vector<Token>& out);

// --- pieces, exposed for testing + the experiment UI's "show me each step" ---

// Lowercase ASCII (UTF-8 passthrough for non-ASCII bytes).
[[nodiscard]] std::string normalize(std::string_view text);

// Is `word` an English stop-word? (lowercased input expected)
[[nodiscard]] bool is_stop_word(std::string_view word, Locale locale = Locale::English);

// Minimal English suffix stemmer (lowercased input expected).
[[nodiscard]] std::string stem(std::string_view word, Locale locale = Locale::English);

}  // namespace oramacore::tokenize
