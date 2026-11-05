# Changelog

All notable changes to this project are documented here.

## [Unreleased]

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