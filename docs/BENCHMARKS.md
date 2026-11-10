# Benchmarks

Reference environment: Apple M-series (arm64), Apple clang 21.0.0,
`-O3 -DNDEBUG`, 2026-11-05.

All numbers are the median of five consecutive runs after a warm-up run. The
first run after a build is noticeably slower due to CPU frequency and cache
warm-up; use warmed medians when comparing changes.

## Current

| Workload | Volume | Latency | Throughput | Peak book |
| --- | ---: | ---: | ---: | ---: |
| Passive add/cancel | 500,000 orders | 78.2 ns/order | 12.78 M orders/s | 1,000 |
| Crossing flow | 500,000 orders | 60.6 ns/order | 16.51 M orders/s | 1,159 |
| Crossing trades | 500,000 orders | — | 493,482 trades | — |
| SPSC queue, two threads | 10,000,000 messages | 28.7 ns/message | 34.9 M messages/s | — |
| Engine, no risk | 1,000,000 commands | 12.3 ns/command | 81.5 M commands/s | — |
| Engine, risk enabled | 1,000,000 commands | 12.2 ns/command | 81.8 M commands/s | — |
| Snapshot encode+decode | 100,000 snapshots | 16.8 µs/snapshot | 0.06 M snapshots/s | 200 orders |
| Delta decode | 100,000 messages | 24.2 ns/message | 41.4 M messages/s | 1 order |

Command:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/orderbook_bench
```

## W1: pooled resting orders

Before W1, each resting order allocated a `std::list` node with the global
allocator. W1 routes those nodes through a chunked fixed-block pool. The pool
reserves 128-byte blocks in 1,024-block chunks, up to the book's configured
capacity (65,536 orders by default).

| Workload | Before | After | Change |
| --- | ---: | ---: | ---: |
| Passive add/cancel | 101.9 ns / 9.82 M/s | 78.2 ns / 12.78 M/s | **+30% throughput** |
| Crossing flow | 66.6 ns / 15.02 M/s | 60.6 ns / 16.51 M/s | **+10% throughput** |

Pool behaviour in the benchmark:

- Passive workload: 1 chunk, 1 global allocation, 1,000 peak orders.
- Crossing workload: 2 chunks, 2 global allocations, 1,159 peak orders.

After warm-up, adding and cancelling orders performs no global allocations; the
test `pool_reuses_blocks_without_heap_growth` asserts that the heap-allocation
count stays constant across 12,000 add/cancel cycles. The crossing workload
benefits less because fills remove orders, so allocation is a smaller share of
its cost.

## W2: SPSC queue

The ingress queue is a bounded single-producer/single-consumer ring buffer with
power-of-two capacity and release/acquire publication. The benchmark uses one
producer thread and one consumer thread, 10 million `uint64_t` messages and a
1,024-slot queue.

| Configuration | Messages | Latency | Throughput |
| --- | ---: | ---: | ---: |
| Two threads, 1,024 slots | 10,000,000 | 28.7 ns/message | 34.9 M messages/s |

The queue test runs the same pattern for 1 million messages under TSan and
checks the sum, count and final emptiness. The event queue uses the same
implementation with a larger capacity.

## W3: risk gateway overhead

The engine benchmark submits one million non-crossing market commands in
batches of 1,000 and drains the event queue after each batch. It runs once with
all risk limits disabled and once with quantity, notional, position, collar and
open-order limits enabled.

| Configuration | Commands | Latency | Throughput |
| --- | ---: | ---: | ---: |
| Engine, no risk | 1,000,000 | 12.3 ns/command | 81.5 M commands/s |
| Engine, risk enabled | 1,000,000 | 12.2 ns/command | 81.8 M commands/s |

The difference is within run-to-run noise (under 2%). The risk path is a few
integer comparisons and two hash-map lookups, so it does not move the dominant
cost of command processing, queue handoff and event emission.

## W4: binary market data

The snapshot benchmark builds a book with 100 bids and 100 asks, encodes the
order-by-order snapshot (5,828 bytes), decodes it, and repeats 100,000 times.
The delta benchmark decodes a 53-byte add-order message.

| Workload | Volume | Latency | Throughput |
| --- | ---: | ---: | ---: |
| Snapshot encode+decode | 100,000 snapshots | 16.8 µs/snapshot | 0.06 M snapshots/s |
| Delta decode | 100,000 messages | 24.2 ns/message | 41.4 M messages/s |

Snapshot cost is dominated by allocating and copying the output vector; the
codec itself only performs integer conversions. Delta decoding is a single
allocation-free pass over a 29-byte record.

## W5: multi-symbol engine

No separate throughput benchmark yet. `MultiSymbolEngine` adds one symbol-id
lookup per command and keeps one book per symbol; the single-book hot path and
benchmark numbers are unchanged. A sharded multi-symbol throughput baseline
will be added with the journal/recovery work.

## W6: journal recovery

The benchmark builds 100,000 submit records, writes the journal, reads it back
and replays every command into a fresh book.

| Workload | Records | Latency | Throughput |
| --- | ---: | ---: | ---: |
| Journal write+read+replay | 100,000 | 158 ns/record | 6.3 M records/s |

The run includes file I/O, decoding and a fresh order book. Replay checks that
all 100,000 orders rest and the checksum path is exercised.

## Planned baselines

- W7: fuzz/soak throughput and long-run memory.
