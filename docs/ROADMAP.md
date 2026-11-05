# cppquant-orderbook roadmap

8-week plan: **2026-11-09 – 2027-01-03**. Goal: turn the single-threaded
matching library into an embeddable engine with predictable allocation,
pre-trade risk, market data, multi-symbol support and recovery.

## Milestones

| Milestone | Date | Scope |
| --- | --- | --- |
| v0.2 | 2026-11-22 | allocation-free book, SPSC queue, event loop |
| v0.3 | 2026-12-06 | risk gateway, market data codec |
| v0.4 | 2026-12-20 | multi-symbol engine, journal/recovery |
| v0.5 | 2027-01-03 | fuzz/soak, docs, release |

## Weekly plan

### W1 (11-09 – 11-15): pool allocation and baseline

- Implement `PoolAllocator<T>` for `std::list<RestingOrder>` with a fixed
  capacity and free list.
- Add an allocation-counting test allocator.
- Extend the benchmark to passive, crossing and mixed workloads.
- Record before/after ns/op and peak memory in `docs/BENCHMARKS.md`.

Acceptance: existing tests pass; steady-state add/cancel allocates nothing;
throughput improves by at least 15% or the result is explained.

### W2 (11-16 – 11-22): SPSC queue and engine loop

- Add `SpscQueue<T, N>`: power-of-two capacity, cache-line padding,
  acquire/release.
- Test FIFO, wraparound and a two-thread 1M-item producer/consumer run.
- Add `MatchingEngine`: inbound commands to outbound accepted/rejected/trade
  events.
- Add a TSan CI job (Linux Clang).

Acceptance: no lost or duplicated events; TSan is clean; event streams replay
deterministically.

### W3 (11-23 – 11-29): pre-trade risk

- `RiskLimits`: max order quantity, max notional, max position per account,
  price collar in bps, max open orders.
- Extend rejection reasons; rejected orders must not mutate the book.
- Add per-reason and per-account counters.

Acceptance: every limit has positive and negative tests; rejected orders do not
trade or change the top of book.

### W4 (11-30 – 12-06): market data

- Binary encoder for snapshots and deltas: version, message type, symbol id,
  sequence, price, quantity.
- Round-trip tests and malformed-input tests.
- Rebuild a book from snapshot plus deltas and compare with direct matching.

Acceptance: depth, best prices and statistics match the source book; decode
failures throw; wire format is documented.

### W5 (12-07 – 12-13): multi-symbol engine

- `Engine` owns `symbol -> OrderBook`; intern symbols to ids.
- Commands and events carry a symbol id with per-symbol sequences.
- CSV replay accepts a `symbol` column; add a multi-symbol sample.

Acceptance: books are fully isolated; single-symbol behaviour is unchanged.

### W6 (12-14 – 12-20): journal and recovery

- Binary journal of accepted commands, including sequence numbers.
- `replay_journal()` rebuilds state; compare checksums with live state.
- Snapshot save/load and truncated-journal recovery tests.

Acceptance: normal, truncated and empty journals all recover; `validate()`
passes after recovery; recovery cost is benchmarked.

### W7 (12-21 – 12-27): fuzz and soak

- Add libFuzzer targets for the decoder and replay parser behind
  `CPPQUANT_FUZZ=ON`.
- Run a 10-minute local fuzz session and a 10M-event soak with periodic
  `validate()`.

Acceptance: no crashes or sanitizer reports; memory does not grow without
bound; every fix gets a regression test.

### W8 (12-28 – 01-03): release

- Update API docs, `CHANGELOG.md`, `docs/BENCHMARKS.md`.
- Tag `v0.5.0` with release notes.
- Run Linux/macOS/ASan/TSan/package-consumer CI.

## Cut lines

If a week slips, keep in this order:

1. Pool allocator, SPSC queue, risk gateway.
2. Market data and multi-symbol engine.
3. Journal/recovery and fuzzing.

## Working agreement

- No backdated commits or empty changes.
- Each feature commit includes tests and a benchmark when performance-related.
- Open a PR per weekly milestone; merge only with green Linux CI.
- Update `CHANGELOG.md` at every milestone.