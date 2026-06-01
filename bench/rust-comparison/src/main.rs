//! Standalone microbenchmark of OramaCore's real on-disk BM25 string index
//! (`oramacore_fields::string`) with the real `oramacore_lib` EN tokenizer/stemmer.
//!
//! Apples-to-apples with the C++ single-field BM25 harness:
//!   - one text field per doc = First Name + Last Name + Company + City + Country
//!   - index build timed separately from CSV parsing
//!   - query pool = distinct tokens (len >= 3) over the same 5 columns
//!   - deterministic LCG seeded at 42; each query is 1 or 2 terms (50/50)
//!   - top_k = 100, fresh scorer per query, warmup before the timed run

use std::collections::HashSet;
use std::path::PathBuf;
use std::time::Instant;

use oramacore_fields::string::{
    BM25u64Scorer, IndexedValue, SearchParams, StringIndexer, StringStorage, Tokenizer,
};
use oramacore_lib::nlp::locales::Locale;
use oramacore_lib::nlp::TextParser;

/// Wrap the real OramaCore EN TextParser so it satisfies the field-index `Tokenizer` trait.
struct ParserTokenizer {
    parser: TextParser,
}

impl Tokenizer for ParserTokenizer {
    fn tokenize_and_stem(&self, input: &str) -> Vec<(String, Option<String>)> {
        self.parser.tokenize_and_stem(input)
    }
}

/// Minimal deterministic LCG (Numerical Recipes constants) so query generation
/// is reproducible and matches the C++ harness's `rng % N` pattern exactly.
struct Lcg {
    state: u64,
}
impl Lcg {
    fn new(seed: u64) -> Self {
        Lcg { state: seed }
    }
    fn next_u64(&mut self) -> u64 {
        // x = a*x + c (mod 2^64)
        self.state = self
            .state
            .wrapping_mul(6364136223846793005)
            .wrapping_add(1442695040888963407);
        self.state
    }
}

struct Row {
    first: String,
    last: String,
    company: String,
    city: String,
    country: String,
}

impl Row {
    /// Single concatenated text field — matches the C++ `--single-field` mode.
    fn field_text(&self) -> String {
        format!(
            "{} {} {} {} {}",
            self.first, self.last, self.company, self.city, self.country
        )
    }
}

fn parse_csv(path: &str) -> Vec<Row> {
    let mut rdr = csv::ReaderBuilder::new()
        .has_headers(true)
        .from_path(path)
        .unwrap_or_else(|e| panic!("open {path}: {e}"));

    // Header: Index,Customer Id,First Name,Last Name,Company,City,Country,Phone 1,Phone 2,Email,Subscription Date,Website
    // indices:   0       1          2          3        4     5     6
    let mut rows = Vec::new();
    for rec in rdr.records() {
        let rec = rec.expect("csv record");
        rows.push(Row {
            first: rec.get(2).unwrap_or("").to_string(),
            last: rec.get(3).unwrap_or("").to_string(),
            company: rec.get(4).unwrap_or("").to_string(),
            city: rec.get(5).unwrap_or("").to_string(),
            country: rec.get(6).unwrap_or("").to_string(),
        });
    }
    rows
}

/// Peak resident-set high-water mark in MB. macOS reports ru_maxrss in BYTES
/// (Linux uses KB) — we measure on macOS, so divide by 1e6.
fn peak_rss_mb() -> f64 {
    let mut ru: libc::rusage = unsafe { std::mem::zeroed() };
    unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut ru) };
    ru.ru_maxrss as f64 / 1e6
}

/// Total size in bytes of everything under `path` (recursive) — the on-disk
/// index segment footprint (OramaCore persists an FST + mmap segment).
fn dir_size_bytes(path: &std::path::Path) -> u64 {
    let mut total = 0u64;
    if let Ok(entries) = std::fs::read_dir(path) {
        for e in entries.flatten() {
            if let Ok(md) = e.metadata() {
                if md.is_dir() {
                    total += dir_size_bytes(&e.path());
                } else {
                    total += md.len();
                }
            }
        }
    }
    total
}

/// nearest-rank percentile over a sorted slice of nanosecond latencies; returns microseconds.
fn percentile_us(sorted_ns: &[u64], p: f64) -> f64 {
    if sorted_ns.is_empty() {
        return 0.0;
    }
    let rank = (p / 100.0 * (sorted_ns.len() as f64)).ceil() as usize;
    let idx = rank.saturating_sub(1).min(sorted_ns.len() - 1);
    sorted_ns[idx] as f64 / 1000.0
}

fn run_dataset(label: &str, path: &str, n_queries: usize, warmup: usize) {
    println!("\n========================================================");
    println!("DATASET: {label}  ({path})");
    println!("========================================================");

    // ---- Parse CSV (NOT timed) ----
    let rows = parse_csv(path);
    let n = rows.len();
    println!("parsed {n} rows");

    // Pre-materialize the per-doc field text (NOT timed — matches C++ which
    // builds the row strings before the timed index loop).
    let texts: Vec<String> = rows.iter().map(|r| r.field_text()).collect();
    let rss_after_parse = peak_rss_mb();  // rows + texts resident, before the index

    // A single shared parser for indexing + query-pool building + query tokenization.
    let index_parser = TextParser::from_locale(Locale::EN);
    let indexer = StringIndexer::new(
        false,
        ParserTokenizer {
            parser: TextParser::from_locale(Locale::EN),
        },
    );

    // Pre-build the IndexedValue for every doc OUTSIDE the timed region? No.
    // The C++ harness times tokenization + posting construction + commit as the
    // "index build". To be apples-to-apples we time index_json (tokenize + build
    // IndexedValue) + insert + compact. CSV parsing and field concatenation are
    // the only excluded steps.

    // ---- Build query pool (NOT timed): distinct tokens, len >= 3 ----
    let mut pool_set: HashSet<String> = HashSet::new();
    for t in &texts {
        for tok in index_parser.tokenize(t) {
            if tok.chars().count() >= 3 {
                pool_set.insert(tok);
            }
        }
    }
    let pool: Vec<String> = pool_set.into_iter().collect();
    println!("query pool: {} distinct terms (len >= 3)", pool.len());
    assert!(!pool.is_empty(), "empty query pool");

    // ---- INDEX BUILD (TIMED) ----
    let tmp = tempfile::tempdir().expect("tempdir");
    let index_path: PathBuf = tmp.path().join("string_index");

    let build_start = Instant::now();
    let storage = StringStorage::new(index_path.clone(), oramacore_fields::string::SegmentConfig::default())
        .expect("create StringStorage");
    for (doc_id, text) in texts.iter().enumerate() {
        let value: IndexedValue = indexer
            .index_json(&serde_json::Value::String(text.clone()))
            .expect("index_json string");
        storage.insert(doc_id as u64, value);
    }
    storage.compact(1).expect("compact");
    let build_elapsed = build_start.elapsed();

    // Memory footprint of the index: in-RAM growth (small — it's mmap'd from
    // disk) and the actual on-disk segment size (where OramaCore's index lives).
    let rss_after_build = peak_rss_mb();
    let on_disk_mb = dir_size_bytes(&index_path) as f64 / 1e6;

    let build_ms = build_elapsed.as_secs_f64() * 1000.0;
    let build_docs_per_sec = n as f64 / build_elapsed.as_secs_f64();
    println!(
        "INDEX BUILD: {:.1} ms  ({:.0} docs/sec)",
        build_ms, build_docs_per_sec
    );

    // ---- Build-phase breakdown (diagnostic, not the headline number) ----
    // Phase A: tokenize + build IndexedValue (index_json). Phase B: insert + compact.
    // Gated: building a second full index pollutes the search measurement's
    // memory/cache state, so only run when BENCH_BREAKDOWN=1.
    if std::env::var("BENCH_BREAKDOWN").as_deref() == Ok("1") {
        let tmp2 = tempfile::tempdir().expect("tempdir2");
        let storage2 =
            StringStorage::new(tmp2.path().join("idx2"), oramacore_fields::string::SegmentConfig::default())
                .expect("create StringStorage2");
        let a_start = Instant::now();
        let mut values: Vec<IndexedValue> = Vec::with_capacity(n);
        for text in &texts {
            values.push(
                indexer
                    .index_json(&serde_json::Value::String(text.clone()))
                    .expect("index_json string"),
            );
        }
        let a_ms = a_start.elapsed().as_secs_f64() * 1000.0;
        let b_start = Instant::now();
        for (doc_id, v) in values.into_iter().enumerate() {
            storage2.insert(doc_id as u64, v);
        }
        storage2.compact(1).expect("compact2");
        let b_ms = b_start.elapsed().as_secs_f64() * 1000.0;
        println!(
            "  breakdown: tokenize+build {:.1} ms ({:.0}%), insert+compact {:.1} ms ({:.0}%)",
            a_ms,
            100.0 * a_ms / (a_ms + b_ms),
            b_ms,
            100.0 * b_ms / (a_ms + b_ms)
        );
    }

    // ---- Pre-generate query strings (NOT timed) ----
    // Mirror C++: term = pool[rng % len]; if (rng % 2 == 1) term += " " + pool[rng2 % len]
    let mut rng = Lcg::new(42);
    let total = warmup + n_queries;
    let mut queries: Vec<String> = Vec::with_capacity(total);
    for _ in 0..total {
        let r1 = rng.next_u64();
        let mut q = pool[(r1 as usize) % pool.len()].clone();
        let coin = rng.next_u64();
        if coin % 2 == 1 {
            let r2 = rng.next_u64();
            q.push(' ');
            q.push_str(&pool[(r2 as usize) % pool.len()]);
        }
        queries.push(q);
    }

    // Pre-tokenize is NOT done — the C++ harness tokenizes inside the timed loop
    // (search = tokenize + index lookup + score + top-k). We do the same: each
    // timed query tokenizes via TextParser then searches.
    let query_parser = TextParser::from_locale(Locale::EN);

    // ---- WARMUP ----
    let mut sink: u64 = 0;
    for q in &queries[..warmup] {
        let toks = query_parser.tokenize(q);
        let mut scorer = BM25u64Scorer::new();
        let params = SearchParams {
            tokens: &toks,
            ..Default::default()
        };
        storage.search(&params, &mut scorer).expect("search");
        let res = scorer.into_search_result_top_k(100);
        sink = sink.wrapping_add(res.docs.len() as u64);
    }

    // ---- SEARCH (TIMED) ----
    // Per-query latency captured in nanoseconds for sub-microsecond resolution.
    let mut latencies_ns: Vec<u64> = Vec::with_capacity(n_queries);
    let search_start = Instant::now();
    for q in &queries[warmup..] {
        let q_start = Instant::now();
        let toks = query_parser.tokenize(q);
        let mut scorer = BM25u64Scorer::new();
        let params = SearchParams {
            tokens: &toks,
            ..Default::default()
        };
        storage.search(&params, &mut scorer).expect("search");
        let res = scorer.into_search_result_top_k(100);
        let q_elapsed = q_start.elapsed();
        latencies_ns.push(q_elapsed.as_nanos() as u64);
        sink = sink.wrapping_add(res.docs.len() as u64);
    }
    let search_elapsed = search_start.elapsed();

    let total_search_s = search_elapsed.as_secs_f64();
    let qps = n_queries as f64 / total_search_s;

    let mut sorted = latencies_ns.clone();
    sorted.sort_unstable();
    let mean_us =
        (latencies_ns.iter().sum::<u64>() as f64 / latencies_ns.len() as f64) / 1000.0;
    let p50 = percentile_us(&sorted, 50.0);
    let p90 = percentile_us(&sorted, 90.0);
    let p99 = percentile_us(&sorted, 99.0);
    let max = *sorted.last().unwrap() as f64 / 1000.0;

    println!("SEARCH: {n_queries} queries in {:.1} ms", total_search_s * 1000.0);
    println!("  throughput: {:.0} queries/sec", qps);
    println!(
        "  latency (us): mean {:.2}  p50 {:.2}  p90 {:.2}  p99 {:.2}  max {:.1}",
        mean_us, p50, p90, p99, max
    );
    println!("  (sink={sink})"); // prevent dead-code elimination

    // ---- Clean summary block ----
    println!("\n--- SUMMARY [{label}] ---");
    println!("docs                : {n}");
    println!("query pool terms    : {}", pool.len());
    println!("index build ms      : {:.1}", build_ms);
    println!("index build docs/s  : {:.0}", build_docs_per_sec);
    println!("search queries      : {n_queries}");
    println!("search q/s          : {:.0}", qps);
    println!("latency mean us     : {:.2}", mean_us);
    println!("latency p50 us      : {:.2}", p50);
    println!("latency p90 us      : {:.2}", p90);
    println!("latency p99 us      : {:.2}", p99);
    println!("latency max us      : {:.1}", max);
    // NOTE on the metrics: OramaCore builds the index in an in-RAM LiveLayer and
    // then `compact`s it to an on-disk segment, so `build peak RAM` is a transient
    // spike during build — the index itself ends up *on disk* (`on-disk`), not
    // resident. (The C++ engine, by contrast, holds the index in RAM, so its
    // "in-RAM index" delta is the steady-state resident footprint.)
    println!("peak RSS mb         : {:.0}", peak_rss_mb());
    println!("build peak RAM mb   : {:.0}", rss_after_build - rss_after_parse);
    println!("index on-disk mb    : {:.0}", on_disk_mb);
}

fn main() {
    let base_10k = "/Users/stevesperandeo/Downloads/customers-10000.csv";
    let base_2m = "/Users/stevesperandeo/Downloads/customers-2000000.csv";

    // protocol: 30000 queries each, 1000 warmup
    run_dataset("10k", base_10k, 30_000, 1_000);
    run_dataset("2M", base_2m, 30_000, 1_000);
}
