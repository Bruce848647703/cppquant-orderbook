# Benchmarks

Reference environment: Apple M-series (arm64), Apple clang 21.0.0,
`-O3 -DNDEBUG`, 2026-11-04.

| Workload | Volume | Latency | Throughput | Peak book |
| --- | ---: | ---: | ---: | ---: |
| Passive add/cancel | 500,000 orders | 159.9 ns/order | 6.25 M orders/s | 1,000 |
| Crossing flow | 500,000 orders | 70.4 ns/order | 14.21 M orders/s | 1,159 |
| Crossing trades | 500,000 orders | — | 493,482 trades | — |

Commands:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/orderbook_bench
```

The workload is deterministic (seed 42). Numbers are indicative; run the
benchmark on your own hardware before comparing.

## Planned baselines

- W1: allocations per passive add/cancel after warm-up, pool versus list.
- W2: SPSC queue throughput and p99 latency, matching-engine events/s.
- W4: market data encode/decode throughput.
- W6: journal replay and snapshot restore time.