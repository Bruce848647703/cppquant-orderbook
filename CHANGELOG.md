# Changelog

All notable changes to this project are documented here.

## [0.5.0] - 2026-11-11

### Added

- Chunked fixed-block pool for resting order nodes, with `pool_stats()` and a
  configurable order capacity. Steady-state add/cancel no longer touches the
  global allocator.
- Tests for pool reuse, capacity exhaustion, clear/reset and non-copyability.
- Bounded lock-free SPSC ring buffer with release/acquire publication.
- Event-driven `BasicMatchingEngine`: one thread enqueues submit/cancel/modify
  commands, the engine thread drains them and emits accepted, rejected, trade,
  cancel, modify and self-trade-cancellation events.
- Queue and engine tests, including a 1M-message concurrent producer/consumer
  run, queue-full handling and dropped-event accounting.
- ThreadSanitizer build option and CI job.
- `RiskGateway` with order quantity, notional, position, price-collar and
  open-order limits.
- Engine-side position/working-order tracking, per-reason and per-account
  rejection counters, and no-mutation-on-reject semantics.
- Risk tests and a benchmark comparing engine throughput with and without the
  gateway.
- Versioned binary market data codec: order-by-order snapshots and
  add/remove/reduce/modify deltas.
- `MarketDataPublisher`, snapshot restore and delta application helpers.
- `OrderBook::resting_orders()` and priority-preserving `reduce()`.
- Market data round-trip, rebuild, wire-size and malformed-input tests.
- Market data encode/decode benchmark and `docs/MARKET_DATA.md`.
- `MultiSymbolEngine` with interned symbol ids, one order book per symbol and
  per-symbol event sequences.
- Symbol-aware submit/cancel/modify commands and `UnknownSymbol` rejection.
- Multi-symbol replay CLI and sample session.
- Multi-symbol tests for isolation, duplicate ids and sequence assignment.
- Binary command journal for accepted submit/cancel/modify commands.
- Journal write/read with truncated-tail recovery and checksum verification.
- Snapshot file persistence and restore.
- Journal and snapshot tests plus `docs/RECOVERY.md`.
- Reusable replay-line parser (`parse_replay_line`) shared by the CLI and fuzz
  targets.
- Replay parser tests for submit/cancel/modify, comments, malformed input and
  direct book application.
- libFuzzer targets for market-data decoding and replay execution, with a seed
  corpus and a portable mutation driver for macOS.
- 10M-event deterministic soak executable with periodic `validate()` and pool
  diagnostics.
- CI libFuzzer job plus `docs/FUZZ.md`.
- Final API/documentation index, release benchmark refresh and package version
  0.5.0.

### Changed

- Passive add/cancel throughput improved by about 30% on the reference machine;
  crossing flow by about 10%. See `docs/BENCHMARKS.md`.
- The benchmark reports two-thread SPSC queue throughput.
- Trades now carry taker and maker account ids.
- The benchmark also reports engine command throughput and risk overhead.

### Planned

- **0.2** — allocation-free resting orders, bounded SPSC queue, event-driven
  matching engine.
- **0.3** — pre-trade risk gateway, binary market data snapshots and deltas.
- **0.4** — multi-symbol engine, command journal and snapshot recovery.
- **0.5** — fuzzing, soak tests, documentation and release.

## [0.1.0] - 2026-11-04

### Added

- Price-time-priority order book with integer tick prices and integer
  quantities.
- Limit, market, IOC and FOK orders; partial fills and cancel/replace.
- Self-trade prevention: allow, cancel resting, cancel aggressor.
- O(1) cancel via order locations and an invariant checker.
- Execution reports, statistics and depth snapshots.
- CSV replay tool and deterministic benchmark.
- CMake package export, unit tests, documentation and CI.
