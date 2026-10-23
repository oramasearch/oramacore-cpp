# BM25F port notes — the first index type

> How we port OramaCore's **BM25F** full-text scorer to C++23 + Boost.Capy coroutines. BM25F is the crown jewel (Tommaso: *"keep as is — change only if you really know what you are doing"*; ~90% of real queries are full-text). It's the **first** index type in `oramacore-cpp`; everything else (prefix/FST, geo, filters, vector-if-kept) follows the same pattern. See the platform analysis: [`oramacore-what-to-keep.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md) + [`research/oramacore-cpp-rebuild-alternatives.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/research/oramacore-cpp-rebuild-alternatives.md).

## What makes BM25F *not* BM25 (the thing to get right)

Two per-field knobs, applied **before** saturation:
1. **per-field weight** `w_f` — boost a field's contribution (title vs. body).
2. **per-field length normalization** `b_f` — normalize by *that field's* average length, not one global doc length.

The field contributions are summed into a **pseudo term-frequency**, and *only then* is the saturation term applied:

```
tf~(t,d) = Σ_f  w_f · tf_{t,f} / (1 − b_f + b_f · (len_f(d) / avglen_f))
score(t,d) = IDF(t) · tf~ / (k1 + tf~)
```

**Critical correctness note (from Tommaso + the OramaCore source):** you **cannot** compute a per-field BM25 and add the results — the math is different. You must *"collect all the contributions first, and only later finalize the score."* The committed code does exactly this: accumulate `w_f · tf_f / norm_f` across fields → one `tf~` → one saturation. Our [`pseudo_tf()`](../include/oramacore/bm25f.hpp) mirrors that; the unit tests in [`tests/test_bm25f.cpp`](../tests/test_bm25f.cpp) pin the "sum-before-saturate" behavior.

- **IDF:** `ln(1 + (N − n + 0.5)/(n + 0.5))` (Robertson/Sparck-Jones, always positive) — matches OramaCore.
- **Defaults:** `w_f = 1.0`, `b_f = 0.75`, global `k1 = 1.2`.

## Sharded scoring: corpus statistics from outside the index

`N`, `df_t` and `avglen_f` are **corpus** statistics, so a shard that scores with its own sees a
different corpus than the whole: a shard where a term is rare gives it a high IDF, a shard where it
is everywhere gives it almost none. A per-shard `top_k` cutoff taken on those local scores can drop
a document that belongs in the global top k (zetta [#851](https://github.com/oramasearch/zetta/issues/851)).

`MemoryIndex` carries the global-statistics pass (the `dfs_query_then_fetch` shape) for a caller
that shards it:

- `stats(query)` — this index's `CorpusStats` for the query: `doc_count`, each analyzed query term's
  document frequency (0 when unseen, so a merge covers every term), and every field's
  `{total_len, doc_count}`.
- `CorpusStats::merge(other)` — sums every count (shards hold disjoint documents).
- `search(query, top_k, global)` — scores with the merged statistics instead of the index's own
  (anything `global` does not carry falls back to the local value).

Scored with the merge of every shard's `stats()`, each shard's scores are **bit-identical** to one
index over all of them (the same integer inputs reach the same `idf` and `avglen` arithmetic), so
the per-shard cutoff is safe. [`tests/test_index.cpp`](../tests/test_index.cpp) (`[stats]`) pins
this on a deliberately skewed two-shard corpus.

## Source map (OramaCore Rust → here)

| OramaCore (Rust) | What it does | oramacore-cpp target |
|---|---|---|
| `src/collection_manager/bm25.rs` (`BM25FFieldParams`) | per-field weight + b; the scorer | `include/oramacore/bm25f.hpp` + `src/bm25f.cpp` ✅ (scoring math) |
| `nlp` sub-crate | tokenization: stemming + stop-words | `tokenize/` (TODO) — port or wrap |
| `oramacore-fields` (committed field storage) | posting lists, per-field stats (avglen, df), the concurrent update-while-search store | `index/` (TODO) — **the hard part** |
| two-index pattern (uncommitted→committed, atomic swap) | write-optimized → read-optimized | `index/` (TODO) |

## Port milestones (in order)

1. **Scoring core** ✅ *(this scaffold)* — pure, testable `idf` / `pseudo_tf` / `score_term` / `score`; no I/O, no coroutines. Unit-tested.
2. **Tokenization** ✅ *(custom, minimal)* — [`tokenize.hpp`](../include/oramacore/tokenize.hpp)/[`tokenize.cpp`](../src/tokenize.cpp): normalize → split → English stop-words → minimal suffix stemmer. **Custom + dependency-free** (decided: no libstemmer / no Rust `nlp` dep — keep it a clean L1 leaf). English-only for now; the `Locale` knob is in the interface for later languages. The stemmer is a tiny Porter subset (not full Snowball) — good enough to unblock posting lists + demo in the experiment UI; swap in Snowball later if score-parity (#101) needs it. Pure/sync; unit-tested ([`test_tokenize.cpp`](../tests/test_tokenize.cpp)). Pieces (`normalize`/`is_stop_word`/`stem`) are exposed so the experiment UI can show each step.
3. **Posting lists + per-field stats** — term → postings (`doc_id`, per-field tf, per-field len); maintain `avglen_f`, `df_t`, `N`. Start in-memory (the **uncommitted** index).
4. **Two-index pattern** — uncommitted (write-optimized, in-memory) → **committed** (read-optimized, mmap-backed) with an **atomic pointer swap**; readers never see a half-built index. (This is one of Tommaso's two "never change" pillars.)
5. **Async search (Boost.Capy, D23)** — `search()` returns a Capy `task<T>`; scatter/gather across postings is `co_await`-shaped; streamed results are `std::generator` / Capy streams. Multi-term queries accumulate per-term scores in the right order.
6. **Persistence** — committed index via mmap + atomic rename (LMDB or our own fs; see research §6); cold tier via Parquet/S3 + [`frameset`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/frameset.md) blobrefs.

## Boost.Capy shape (D23)

- The **scoring math is synchronous** (CPU only) — keep it free of Capy so it's trivially testable and reusable.
- The **I/O boundary is coroutine-native**: `search(query) -> capy::task<SearchResult>`; reading committed postings off mmap/disk and scatter/gathering across shards uses `co_await` + `when_all` (parallel per-term/per-shard). This matches the data-node's dispatch model and the platform-wide coroutine rule (D23).
- Results flow out as a `frameset` ([D12](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d12)); large result sets stream via cursors.

## Build-vs-adopt reminder

This module is the **port** path. The **adopt** alternative (ParadeDB/pg_search — Tantivy in Postgres, or a Xapian sidecar) is tracked in [`oramacore-what-to-keep.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md). pg_search is AGPL (license conflict) and lacks true per-field `b`; Xapian gives true BM25F but as a GPL/LGPL sidecar. We port here to keep full control + a clean license + the coroutine model. If the port's `oramacore-fields` equivalent proves too costly, revisit adopt.

## Open questions

- **OQ1 — tokenizer:** port `nlp` (Rust) via CXX ([D18](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d18)) vs. a native C++ stemmer (e.g. Snowball) vs. reuse Tantivy's tokenizer via FFI.
- **OQ2 — postings store:** bespoke mmap (port `oramacore-fields`) vs. LMDB vs. Arrow columnar (research §6). The concurrent update-while-search engine is the dominant cost either way.
- **OQ3 — score parity test:** build a golden-output fixture from OramaCore on a known corpus to verify our scores match within tolerance before trusting the port.
