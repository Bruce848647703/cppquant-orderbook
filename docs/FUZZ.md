# Fuzzing and soak testing

## Targets

Two libFuzzer entry points exercise the parsers that consume untrusted input:

- `fuzz/fuzz_market_data.cpp` decodes snapshots and deltas, restores a snapshot
  into a book, applies a delta and checks `validate()`.
- `fuzz/fuzz_replay.cpp` splits CSV-like input into lines, parses replay
  commands, applies them to a bounded book and checks `validate()`.

Both targets catch expected parse errors. A failed book invariant aborts the
process, so the fuzzer records it as a crash.

## Seed corpus

```text
fuzz/corpus/market_data/   valid snapshots, deltas and malformed headers
fuzz/corpus/replay/        valid commands, comments and malformed lines
```

The corpus is small and deterministic; libFuzzer grows it in the build
directory.

## libFuzzer

Requires Clang, a libFuzzer runtime and the sanitizer configuration:

```bash
cmake -S . -B build-fuzz -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCPPQUANT_FUZZ=ON \
  -DCPPQUANT_SANITIZE=ON \
  -DCPPQUANT_BUILD_TESTS=OFF \
  -DCPPQUANT_BUILD_BENCHMARKS=OFF \
  -DCPPQUANT_BUILD_EXAMPLES=OFF
cmake --build build-fuzz --target fuzz_market_data fuzz_replay

./build-fuzz/fuzz_market_data fuzz/corpus/market_data \
  -max_total_time=600 -rss_limit_mb=2048
./build-fuzz/fuzz_replay fuzz/corpus/replay \
  -max_total_time=600 -rss_limit_mb=2048
```

The fuzz executables are excluded from the default build so an ordinary
`cmake --build` does not require a libFuzzer runtime. CI builds both targets and
runs each for 60 seconds on every push.

## Portable smoke driver

Apple's Command Line Tools ship the fuzzer headers but not
`libclang_rt.fuzzer_osx.a`. The same entry points are also linked with
`fuzz/smoke_main.cpp`, which reads the corpus, mutates seeds and generates random
buffers without a fuzzer runtime:

```bash
./build-fuzz/fuzz_market_data_smoke fuzz/corpus/market_data --seconds 300
./build-fuzz/fuzz_replay_smoke fuzz/corpus/replay --seconds 300
```

Both drivers are ASan/UBSan-instrumented when the project is configured with
`CPPQUANT_SANITIZE=ON`. They are not coverage-guided, but they give a repeatable
local crash check on platforms without libFuzzer.

## Soak test

`orderbook_soak` mixes limit submissions, cancels and modifies, prunes stale
ids, and calls `validate()` plus a crossed-book check every 100,000 events:

```bash
./build/orderbook_soak --events 10000000 --seed 42
```

A 10M-event run on the reference machine takes about 0.8 s, ends with about
2,000 live orders, and performs three pool chunk allocations. CTest runs a
100,000-event smoke version.

## Findings

The initial runs found no crashes in the market-data decoder or replay parser,
but the CI libFuzzer job found a signed integer overflow when restoring a
snapshot whose same-price quantities exceeded `int64`. Limit orders now reject
a level-total overflow, aggregate sums saturate, and the triggering input is in
`fuzz/corpus/market_data/overflow_snapshot`. Subsequent smoke runs completed
without a sanitizer report:

| Driver | Inputs | Corpus seeds |
| --- | ---: | ---: |
| `fuzz_market_data_smoke` | 4,124,482 | 9 |
| `fuzz_replay_smoke` | 143,531 | 4 |
| `orderbook_soak --events 10000000` | 10,000,000 events | — |

Regression tests cover the overflow rejection directly and through snapshot
restore.

Wall-clock times include host sleep and are not meaningful as throughput
numbers. The replay parser refactor added regression tests for the boundary
cases the corpus targets: missing fields, unknown event names, non-numeric ids
and out-of-range integers.

## Limitations

- The soak uses deterministic synthetic prices, not recorded exchange data.
- It does not run under TSan; the queue and engine concurrency tests do.
- libFuzzer is not run for an unbounded time in CI.