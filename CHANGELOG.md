# Changelog

All notable changes to this project are documented here.

## [Unreleased]

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

### Changed

- Passive add/cancel throughput improved by about 30% on the reference machine;
  crossing flow by about 10%. See `docs/BENCHMARKS.md`.
- The benchmark now also reports two-thread SPSC queue throughput.

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
