# oramacore-cpp

A **C++23 port of the [OramaCore](https://github.com/oramasearch/oramacore) search engine**: a
smaller engine that ports OramaCore's full-text core first and adds other index types one at a
time.

**First index type: BM25F** full-text, the subsystem most search workloads depend on. Other index
types (prefix/FST, geo, filters, vector) follow the same pattern. Async index/search is
**coroutine-native via Boost.Capy**, and the module is a **library** that a data node or service
links in.

> **Port notes (start here):** [`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md) — the BM25F math, the OramaCore source map, the milestone order, and the Capy shape.

## Build

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

> **Field limits:** `MemoryIndex` auto-registers the fields documents carry, so it bounds how many and how long (`Limits`: 1,000,000 fields, 1,000,000 per document, 1024-byte names by default), and `add()` returns `Result<DocId>`, refusing a document over a limit with the index unchanged ([port notes → Field limits](docs/bm25f-port-notes.md#field-limits-oramacore-cpp4)).

> Scaffold: the BM25F **scoring core** is implemented + unit-tested (`idf` / `pseudo_tf` / `score`). Tokenization, posting lists, the two-index pattern, and async search land per the milestone order in the port notes.

## Stack

- **C++23** (moving to C++26), CMake ≥ 3.28 + Ninja, FetchContent, Catch2.
- Coroutines via **Boost.Capy** — behind `-DORAMACORE_CPP_CAPY=ON` when async search lands; the scoring math stays synchronous.
- `Result<T> = std::expected<T, Error>`; no exceptions across the public API. Links as `orama::oramacore-cpp`.

## Where the design lives

The design of the port, the source map from OramaCore and the open questions are in
[`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md). Benchmarks against OramaCore are in
[`bench/`](bench/README.md).

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licenses are in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES). Contributions are welcome under the Developer
Certificate of Origin; see [CONTRIBUTING.md](CONTRIBUTING.md).
