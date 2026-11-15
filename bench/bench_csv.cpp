// bench_csv — profile the oramacore-cpp BM25F engine on a real CSV.
//
// Build (RelWithDebInfo, with symbols, so `sample`/Instruments can attribute):
//   cmake -S . -B build-bench -G Ninja -DORAMACORE_CPP_BENCH=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
//   cmake --build build-bench
//   ./build-bench/oramacore-cpp-bench /path/customers-10000.csv
//
// Phases timed separately so the profile isn't muddied by CSV parsing:
//   read file  ->  parse CSV (excluded from engine timing)  ->  index build  ->  search.
//
// Flags (positional + named):
//   <csv>                 required
//   --searches N          number of timed searches (default 20000)
//   --topk K              top-k per query (default 100)
//   --search-secs S       instead of N, loop searches for S seconds (for `sample`)
//   --index-loops K       rebuild the whole index K times (for sampling the build path)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/resource.h>  // getrusage — peak RSS

#include "oramacore/index.hpp"
#include "oramacore/tokenize.hpp"
#include "oramacore/version.hpp"

using Clock = std::chrono::steady_clock;
static double ms_since(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Peak resident-set high-water mark in MB. macOS reports ru_maxrss in BYTES
// (Linux uses KB) — we measure on macOS, so divide by 1e6.
static double peak_rss_mb() {
    struct rusage ru {};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<double>(ru.ru_maxrss) / 1e6;
}

// Minimal quote-aware CSV split (parse cost is measured separately from engine).
static std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool q = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (q) {
            if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { cur.push_back('"'); ++i; } else q = false; }
            else cur.push_back(c);
        } else if (c == '"') q = true;
        else if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: %s <csv> [--searches N] [--topk K] [--search-secs S] [--index-loops K]\n", argv[0]); return 1; }
    const char* path = argv[1];
    std::size_t n_searches = 20000;
    std::size_t topk = 100;
    double search_secs = 0.0;
    int index_loops = 1;
    bool single_field = false;  // concatenate text cols into one BM25 field (Rust-comparable)
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--searches") && i + 1 < argc) n_searches = std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i], "--topk") && i + 1 < argc) topk = std::stoul(argv[++i]);
        else if (!std::strcmp(argv[i], "--search-secs") && i + 1 < argc) search_secs = std::stod(argv[++i]);
        else if (!std::strcmp(argv[i], "--index-loops") && i + 1 < argc) index_loops = std::stoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--single-field")) single_field = true;
    }

    // ---- read file ----
    auto t = Clock::now();
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const double read_ms = ms_since(t);

    // ---- parse CSV (NOT counted as engine time) ----
    t = Clock::now();
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
    {
        std::string line;
        bool first = true;
        for (std::size_t i = 0; i <= raw.size(); ++i) {
            if (i == raw.size() || raw[i] == '\n') {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    auto fields = split_csv(line);
                    if (first) { headers = std::move(fields); first = false; }
                    else rows.push_back(std::move(fields));
                }
                line.clear();
            } else line.push_back(raw[i]);
        }
    }
    const double parse_ms = ms_since(t);
    const double rss_after_parse_mb = peak_rss_mb();  // raw CSV + parsed rows resident

    // For --single-field: the lexical text columns concatenated into one field
    // (so it matches a single Rust StringStorage / single-field BM25).
    std::vector<int> textcols;
    {
        auto colidx = [&](const char* name) -> int {
            for (std::size_t i = 0; i < headers.size(); ++i) if (headers[i] == name) return (int)i;
            return -1;
        };
        for (const char* nm : {"First Name", "Last Name", "Company", "City", "Country"}) {
            const int c = colidx(nm);
            if (c >= 0) textcols.push_back(c);
        }
    }

    auto build_index = [&]() {
        oramacore::index::MemoryIndex idx({});  // all columns auto-registered
        for (const auto& r : rows) {
            oramacore::index::Document doc;
            if (single_field) {
                std::string t;
                for (int c : textcols) {
                    if (c < (int)r.size()) { if (!t.empty()) t += ' '; t += r[c]; }
                }
                doc["text"] = std::move(t);
            } else {
                for (std::size_t c = 0; c < headers.size() && c < r.size(); ++c) doc[headers[c]] = r[c];
            }
            if (const auto added = idx.add(doc); !added) {
                std::fprintf(stderr, "cannot index row: %s\n", added.error().message.c_str());
                std::exit(1);
            }
        }
        return idx;
    };

    // ---- index build (timed; optionally looped for sampling) ----
    t = Clock::now();
    oramacore::index::MemoryIndex index = build_index();
    double build_ms = ms_since(t);
    for (int i = 1; i < index_loops; ++i) { auto tmp = build_index(); (void)tmp; }
    if (index_loops > 1) build_ms = ms_since(t) / index_loops;  // avg per build
    const double rss_after_build_mb = peak_rss_mb();  // + the in-RAM index

    // ---- build a realistic query pool from the data (names / cities / companies) ----
    std::vector<std::string> terms;
    {
        std::unordered_set<std::string> seen;
        auto col = [&](const char* name) -> int {
            for (std::size_t i = 0; i < headers.size(); ++i) if (headers[i] == name) return (int)i;
            return -1;
        };
        const int cols[] = {col("First Name"), col("Last Name"), col("City"), col("Company"), col("Country")};
        for (const auto& r : rows) {
            for (int ci : cols) {
                if (ci < 0 || (std::size_t)ci >= r.size()) continue;
                for (const auto& tok : oramacore::tokenize::tokenize(r[ci])) {
                    if (tok.text.size() >= 3 && seen.insert(tok.text).second) terms.push_back(tok.text);
                }
            }
        }
    }
    if (terms.empty()) { std::fprintf(stderr, "no query terms extracted\n"); return 1; }

    std::mt19937 rng(42);
    std::uniform_int_distribution<std::size_t> pick(0, terms.size() - 1);
    std::uniform_int_distribution<int> nterms(1, 2);
    auto next_query = [&]() {
        std::string q = terms[pick(rng)];
        if (nterms(rng) == 2) { q += " "; q += terms[pick(rng)]; }
        return q;
    };

    // ---- search ----
    std::fprintf(stderr, "SEARCH_PHASE_BEGIN\n");
    std::fflush(stderr);
    std::uint64_t sink = 0;
    std::size_t did = 0;
    double search_ms = 0.0;
    std::vector<double> lat;  // per-query microseconds

    if (search_secs > 0.0) {
        auto start = Clock::now();
        while (ms_since(start) < search_secs * 1000.0) {
            auto q = next_query();
            auto r = index.search(q, topk);
            sink += r.total_matched + r.hits.size();
            ++did;
        }
        search_ms = ms_since(start);
    } else {
        lat.reserve(n_searches);
        // pre-generate queries so query construction isn't in the timed region
        std::vector<std::string> qs;
        qs.reserve(n_searches);
        for (std::size_t i = 0; i < n_searches; ++i) qs.push_back(next_query());
        auto start = Clock::now();
        for (std::size_t i = 0; i < n_searches; ++i) {
            auto q0 = Clock::now();
            auto r = index.search(qs[i], topk);
            lat.push_back(std::chrono::duration<double, std::micro>(Clock::now() - q0).count());
            sink += r.total_matched + r.hits.size();
        }
        search_ms = ms_since(start);
        did = n_searches;
    }

    // ---- stats ----
    auto pct = [&](double p) {
        if (lat.empty()) return 0.0;
        std::vector<double> s = lat;
        std::sort(s.begin(), s.end());
        std::size_t idx = (std::size_t)(p / 100.0 * (s.size() - 1));
        return s[idx];
    };
    double mean_us = 0.0;
    if (!lat.empty()) { for (double v : lat) mean_us += v; mean_us /= lat.size(); }

    std::printf("\n=== oramacore-cpp bench (%s) ===\n", oramacore::kVersion.data());
    std::printf("file:           %.2f MB, read %.1f ms\n", raw.size() / 1e6, read_ms);
    std::printf("parse CSV:      %.1f ms  (%zu rows, %zu cols) [excluded from engine timing]\n",
                parse_ms, rows.size(), headers.size());
    std::printf("INDEX BUILD:    %.1f ms  (%.0f docs/s, %zu fields/doc)%s\n",
                build_ms, rows.size() / (build_ms / 1000.0), headers.size(),
                index_loops > 1 ? "  [avg of loops]" : "");
    std::printf("query pool:     %zu distinct terms\n", terms.size());
    std::printf("SEARCH:         %zu queries in %.1f ms  ->  %.0f queries/s\n",
                did, search_ms, did / (search_ms / 1000.0));
    if (!lat.empty())
        std::printf("  latency us:   mean %.2f | p50 %.2f | p90 %.2f | p99 %.2f | max %.2f\n",
                    mean_us, pct(50), pct(90), pct(99), pct(99.99));
    std::printf("  (top-k=%zu, avg work/query sink=%llu)\n", topk, (unsigned long long)sink);
    const double rss_peak_mb = peak_rss_mb();
    std::printf("MEMORY:         peak RSS %.0f MB | index in-RAM ~%.0f MB | on-disk 0 MB (pure in-memory)\n",
                rss_peak_mb, rss_after_build_mb - rss_after_parse_mb);
    return 0;
}
