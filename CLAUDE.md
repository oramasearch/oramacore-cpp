# oramacore-cpp — Claude rules

The **C++23 port of OramaCore's engine** for the [Orama Platform](https://github.com/oramasearch/orama-platform). Scaffold stage. **Code only** — design + keep/cut analysis live in the platform repo.

## What this is

A leaner C++23 successor to OramaCore (Rust). We **keep the crown jewels** (BM25F full-text, the WriteSide/ReadSide + two-index pattern, the concurrent update-while-search store) and **cut the cruft** (vector/embeddings → drops Python+GPU, e-commerce, sort, RabbitMQ→PG journal, JS hooks). Full analysis: [`oramacore-what-to-keep.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md).

**First index type: BM25F.** Start here — [`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md). Other index types follow the same milestone pattern.

## Load-bearing rules

- **BM25F sum-before-saturate.** Per-field contributions (`w_f`, `b_f`) accumulate into one pseudo-tf *before* the `k1` saturation — you cannot per-field-BM25-and-add (the math differs). The scaffold's `pseudo_tf()` enforces this; don't "simplify" it. (Tommaso: *"collect all contributions first, finalize the score later."*)
- **Coroutines via Boost.Capy (D23).** Async index/search returns Capy `task<T>`; scatter/gather uses `co_await` + `when_all`. Keep the **scoring math synchronous** (pure, testable) — only the I/O boundary is coroutine-shaped.
- **Library-first (D16); C++23 (→C++26, D7); `Result<T> = std::expected`** — no exceptions across the API.
- **Single responsibility (D21):** this is the search/index *engine*. It does not own the HTTP surface (that's [`component`](https://github.com/oramasearch/component)), the payload type (that's [`frameset`](https://github.com/oramasearch/frameset)), or orchestration (that's the [data-node](https://github.com/oramasearch/data-node), which embeds this).
- **Port parity (OQ3):** build golden-output fixtures from OramaCore and verify our scores match within tolerance before trusting the port.

## Where the design lives

[`oramacore-what-to-keep.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md) (keep/cut + port effort) · [`research/oramacore-cpp-rebuild-alternatives.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/research/oramacore-cpp-rebuild-alternatives.md) (library/PG-extension menu) · architecture [D7](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d7)/[D12](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d12)/[D18](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d18)/[D23](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d23). Tracking: [`#7`](https://github.com/oramasearch/orama-platform/issues/7)/[`#28`](https://github.com/oramasearch/orama-platform/issues/28).

## Layout

```
include/oramacore/   public headers (error, version, bm25f, …)
src/                 compiled impl (bm25f.cpp; tokenize/, index/ later)
tests/               Catch2 (test_bm25f.cpp pins the BM25F math)
docs/                bm25f-port-notes.md (+ per-index-type notes later)
```

## Conventions

- **Conventional Commits**; **never mention "Claude"/"AI"/"Codex"** in commits/PRs (org policy). Private repo — push to `main` directly unless told otherwise.
