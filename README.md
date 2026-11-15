# oramacore-cpp

A C++23 rewrite of the [OramaCore](https://github.com/oramasearch/oramacore) search engine, built to
be faster and to use less memory. It is a library: you link it into your program and index and search
documents in process.

Today it provides **BM25F full-text search** over an in-memory index. Other index types (prefix,
geo, filters, vector) are planned, one at a time.

## What you get

- **BM25F scoring** across the fields of a document, with a weight and a length normalization per
  field. Field contributions are summed before saturation, as BM25F defines it.
- **Any document shape.** A document is a map of field name to text. Fields you declare up front
  keep their own weight; any other field is registered on first sight with defaults.
- **Sharded search.** Each shard reports its corpus statistics (`stats()`), you merge them, and every
  shard then scores with the merged view, so results match one index over the whole corpus.
- **Bounded input.** `Limits` caps the fields an index registers: 1,000,000 fields, 1,000,000 per
  document and 1 KiB names by default, all configurable. A document over a limit is refused and the
  index is unchanged. Adding a document costs time in proportion to its own fields.
- **No exceptions across the API.** Calls that can fail return `Result<T>` (`std::expected<T, Error>`).

## Performance

Measured against OramaCore's BM25 index on the same machine and data (2 million rows), both built at
full optimization:

| | OramaCore (Rust) | oramacore-cpp |
|---|---|---|
| Index build | 16.1 s | 4.63 s |
| Search throughput | 13.2k queries/s | 119k queries/s |
| Search latency, p99 | 1,004 µs | 97 µs |
| Peak memory | 9.4 GB | 2.0 GB |

The comparison is not exactly like for like: OramaCore runs a fuller language pipeline (a Snowball
stemmer) and persists its index to disk, while this library keeps the index in memory with a simpler
stemmer. [`bench/`](bench/README.md) has the protocol, the full results and how to run both.

## Use it

```cpp
#include "oramacore/index.hpp"

namespace ix = oramacore::index;

ix::MemoryIndex index({{.name = "title", .params = {.weight = 2.0}}});

if (auto added = index.add({{"title", "Postgres tuning guide"},
                            {"body", "how to tune a postgres database"}}); !added) {
    // added.error().message says which limit the document broke
}

const auto result = index.search("postgres tuning", /*top_k=*/10);
for (const auto& hit : result.hits) {
    // hit.doc_id, hit.score
}
```

In CMake, add the repository (for example with `FetchContent`) and link the target:

```cmake
target_link_libraries(your_target PRIVATE orama::oramacore-cpp)
```

## Build and test

Needs a C++23 compiler, CMake 3.28 or later and Ninja. Catch2 is fetched for the tests.

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

## Status

Early. The index is in memory and append-only: deleting or updating a document is not supported
yet, and nothing is persisted. The tokenizer handles English, with a minimal stemmer. Asynchronous
search on Boost.Capy coroutines is planned behind `-DORAMACORE_CPP_CAPY=ON`; scoring stays
synchronous.

How the engine works, and how it maps to OramaCore's source, is in
[`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md).

## License

MIT, see [LICENSE](LICENSE). Third-party components and their licenses are in
[THIRD_PARTY_LICENSES](THIRD_PARTY_LICENSES). Contributions are welcome under the Developer
Certificate of Origin; see [CONTRIBUTING.md](CONTRIBUTING.md). Security reports: [SECURITY.md](SECURITY.md).
