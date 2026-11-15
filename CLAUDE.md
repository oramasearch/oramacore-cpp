# oramacore-cpp: rules for coding agents

A **C++23 port of OramaCore's search engine** (OramaCore is written in Rust). Early stage. This repository
holds the code only.

## What this is

A leaner C++23 successor to OramaCore. It keeps the core of the engine (BM25F full-text scoring, the
write side / read side split with two indexes, a store that can be updated while it is searched) and leaves
out what this port does not need (vector embeddings and the Python and GPU stack they bring, e-commerce
features, sorting, the message queue, JavaScript hooks).

**First index type: BM25F.** Start with [`docs/bm25f-port-notes.md`](docs/bm25f-port-notes.md). Other index
types follow the same milestone pattern.

## Build and test

```sh
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
```

## Load-bearing rules

- **BM25F sums before it saturates.** Per-field contributions (`w_f`, `b_f`) accumulate into one pseudo term
  frequency *before* the `k1` saturation. Scoring each field with BM25 and adding the results is different
  math and is wrong. `pseudo_tf()` enforces this; don't "simplify" it. Collect every contribution first, and
  finalize the score last.
- **Coroutines via Boost.Capy.** Async index and search operations return a Capy `task<T>`; scatter/gather
  uses `co_await` and `when_all`. Keep the **scoring math synchronous** (pure and testable): only the I/O
  boundary is coroutine-shaped.
- **Library first; C++23 today, C++26 as the target; `Result<T> = std::expected`.** No exceptions cross the
  public API.
- **Single responsibility:** this is the search and index *engine*. It does not own an HTTP surface, a
  payload format or orchestration; the program that embeds it does.
- **Port parity:** build golden-output fixtures from OramaCore and check that our scores match within a
  tolerance before trusting the port.

## Layout

```
include/oramacore/   public headers (error, version, bm25f, ...)
src/                 compiled implementation (bm25f.cpp, tokenize.cpp, index.cpp)
tests/               Catch2 tests (test_bm25f.cpp pins the BM25F math)
bench/               benchmarks
docs/                bm25f-port-notes.md (and per-index-type notes as they are added)
```

## Conventions

- **Conventional Commits** for commit messages and pull request titles.
- **No AI attribution:** never credit an AI or coding assistant as an author or co-author, and never mention
  one in commit messages, pull request titles or descriptions, tags or release notes.
- **Documentation in the same commit:** when you change behaviour, update the docs and the folder's
  `README.md` in the same commit.
- **Banned word: "seam".** It is ambiguous. Name the mechanism instead: an interface, an extension point, an
  injected dependency, a boundary, or a test double point.
- Pull requests target the `dev` branch.
