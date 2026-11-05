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

## Planned baselines

- W2: SPSC queue throughput and p99 latency, matching-engine events/s.
- W4: market data encode/decode throughput.
- W6: journal replay and snapshot restore time.
