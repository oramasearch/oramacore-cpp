# oramacore-cpp

The **C++23 port of OramaCore's engine** for the [Orama Platform](https://github.com/oramasearch/orama-platform) — a leaner successor that keeps the crown jewels and drops the cruft (per the [keep/cut analysis](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md)).

**First index type: BM25F** full-text — the highest-value subsystem (~90% of real queries; Tommaso: *"keep as is"*). Other index types (prefix/FST, geo, filters, vector-if-kept) follow the same pattern. Async index/search is **coroutine-native via Boost.Capy** ([D23](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d23)); the module is a **library** ([D16](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d16)) that lives under the [data node](https://github.com/oramasearch/data-node).

> **Port notes (start here):** [`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md) — the BM25F math, the OramaCore source map, the milestone order, and the Capy shape.

## Build

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

> **Field limits:** `MemoryIndex` auto-registers the fields documents carry, so it bounds how many and how long (`Limits`: 1024 fields, 512 per document, 256-byte names by default), and `add()` returns `Result<DocId>`, refusing a document over a limit with the index unchanged ([port notes → Field limits](docs/bm25f-port-notes.md#field-limits-oramacore-cpp4)).

> Scaffold: the BM25F **scoring core** is implemented + unit-tested (`idf` / `pseudo_tf` / `score`). Tokenization, posting lists, the two-index pattern, and async search land per the milestone order in the port notes.

## Stack

- **C++23** (→ C++26 per [D7](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/architecture.md#d7)), CMake ≥ 3.28 + Ninja, FetchContent, Catch2.
- Coroutines via **Boost.Capy** (D23) — behind `-DORAMACORE_CPP_CAPY=ON` when async search lands; the scoring math stays synchronous.
- `Result<T> = std::expected<T, Error>`; no exceptions across the public API. Links as `orama::oramacore-cpp`.

## Where the design lives

Code only here. Strategy + architecture + the keep/cut + library-vs-adopt analysis live in [`oramasearch/orama-platform`](https://github.com/oramasearch/orama-platform):
[`oramacore-what-to-keep.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/oramacore-what-to-keep.md) · [`research/oramacore-cpp-rebuild-alternatives.md`](https://github.com/oramasearch/orama-platform/blob/main/docs/projects/orama-platform/research/oramacore-cpp-rebuild-alternatives.md). Tracking: data-node [`#7`](https://github.com/oramasearch/orama-platform/issues/7) / engine interface [`#28`](https://github.com/oramasearch/orama-platform/issues/28).
