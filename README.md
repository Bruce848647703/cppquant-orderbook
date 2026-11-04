# cppquant-orderbook

A price-time-priority limit order book and matching engine written in modern
C++20, with no third-party dependencies.

[![CI](https://github.com/Bruce848647703/cppquant-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/Bruce848647703/cppquant-orderbook/actions/workflows/ci.yml)

The project is deliberately small enough to read in one sitting, but it models
the parts that matter in a real matching engine: integer tick prices, strict
price-time priority, partial fills, market/IOC/FOK orders, cancel/replace,
self-trade prevention, execution reports and book invariants.

## Features

- **Price-time priority** via an ordered price map plus an intrusive FIFO list
  per level.
- **Order types**: limit, market, immediate-or-cancel (IOC), fill-or-kill
  (FOK).
- **O(1) cancel by id** using a hash map of order locations.
- **Cancel/replace** with exchange-style priority rules: reducing quantity at
  the same price keeps priority, a price change or size increase loses it.
- **Self-trade prevention**: allow, cancel resting, or cancel aggressor.
- **Execution reports** with monotonic trade sequence numbers; trades print at
  the resting (maker) price.
- **Statistics** for throughput and replay: submitted, rested, cancelled,
  modified, rejected, trades, quantity and notional.
- **Invariant checker** (`validate()`) used by the tests and safe to call from
  debug builds.
- **CSV replay tool** and a deterministic benchmark.
- CMake package export, CI on Linux/macOS with GCC, Clang and sanitizers.

## Quick start

Requirements: a C++20 compiler and CMake 3.20+. Ninja is optional.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

If Ninja is not installed, omit `-G Ninja`.

Build the sanitizer configuration:

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug -DCPPQUANT_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Install the library and CMake package:

```bash
cmake --install build --prefix /tmp/cppquant
```

Downstream projects can then use:

```cmake
find_package(cppquant CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE cppquant::orderbook)
```

## Usage

```cpp
#include "cppquant/orderbook.hpp"

using namespace cppquant;

OrderBook book;

// Resting asks.
book.add_limit_order(1, Side::Sell, 10'030, 100);
book.add_limit_order(2, Side::Sell, 10'035, 50);

// Aggressive buy: fills 100 at 10'030 and 20 at 10'035.
auto result = book.add_limit_order(3, Side::Buy, 10'035, 120);

for (const auto& trade : result.trades) {
  // trade.maker_id, trade.price, trade.quantity, trade.sequence ...
}

assert(result.filled == 120);
assert(book.best_ask() == Price{10'035});
assert(book.quantity_of(2) == std::optional<Quantity>{30});
```

Prices are integer ticks and quantities are integer lots. The book does not know
about tick sizes, currencies or contract multipliers; those belong in the
caller's instrument definition.

### Order lifecycle

| Type | Executes | Remainder |
| --- | --- | --- |
| `Limit` | Against crossing levels | Rests on the book |
| `Market` | Against any level | Cancelled |
| `ImmediateOrCancel` | Against crossing levels | Cancelled |
| `FillOrKill` | Only if the full quantity is available | Rejected if not |

A rejected order returns `accepted == false` with a `RejectReason`; it never
mutates the book. `SubmitResult::remaining` is the unfilled quantity: it rests
when `rested == true`, otherwise it was cancelled or the order was rejected.

### Cancel and replace

```cpp
book.modify(order_id, new_price, new_quantity);
```

- Same price, quantity not increased: priority is preserved.
- Price change or quantity increase: the order goes to the back of the new
  level.
- A price change that would cross the opposite side is rejected. Cancel and
  resubmit if you want it to execute.

## CSV replay

`orderbook_replay` reads events from a file or stdin and prints executions,
the final book and statistics.

```bash
./build/orderbook_replay data/sample_session.csv
```

Format (one event per line, `#` starts a comment):

```text
timestamp,event,id,side,price,quantity,account
1,limit,1,S,10030,10,11
2,market,2,B,0,7,12
3,cancel,1
4,modify,2,10025,5
```

Events: `limit`/`add`, `market`, `ioc`, `fok`, `cancel`, `modify`. `account`
is optional. For `modify` the columns are `event,id,price,quantity`.

## Benchmark

```bash
./build/orderbook_bench          # full run
./build/orderbook_bench --quick  # CI smoke test
```

The workload is deterministic (fixed seed) and reports ns/order and orders per
second for two scenarios: passive add/cancel with a bounded book, and a
continuous crossing flow with marketable limit orders.

Reference numbers from an Apple M-series laptop (Apple clang 21, `-O3`):

| Scenario | Orders | ns/order | Orders/s | Peak book |
| --- | ---: | ---: | ---: | ---: |
| Passive add/cancel | 500,000 | 160 | 6.3 M | 1,000 |
| Crossing flow | 500,000 | 70 | 14.2 M | 1,159 |

These are indicative only; run the benchmark on your own hardware.

## Design

```text
bids: std::map<Price, Level, std::greater<Price>>   best bid at begin()
asks: std::map<Price, Level, std::less<Price>>      best ask at begin()

Level:
  total quantity
  std::list<RestingOrder>   FIFO, stable iterators for O(1) erase

locations: unordered_map<OrderId, {side, price, list iterator}>
```

Matching walks the opposite book from the best price outwards. Within a level
it consumes orders from the front, so arrival order decides priority. Each
execution removes quantity from the maker; fully filled makers are erased from
the list, the location map and, when empty, the price level.

See [docs/DESIGN.md](docs/DESIGN.md) for the matching pseudocode, invariants
and the reasoning behind the data structures.

### Complexity

| Operation | Time |
| --- | --- |
| Submit (no fills) | `O(log L)` |
| Submit (F fills) | `O(F + log L)` amortized |
| Cancel | `O(1)` average |
| Modify (priority kept) | `O(1)` |
| Modify (reinsert) | `O(log L)` |
| Best bid/ask | `O(1)` |
| Depth (N levels) | `O(N)` |

`L` is the number of price levels and `F` the number of fills. Memory is
`O(orders + levels)`.

## Tests

The repository uses a tiny header-only test harness (`tests/test_framework.hpp`)
instead of an external framework, so the build has zero dependencies. The suite
covers:

- price and time priority across and within levels;
- partial fills, market/IOC/FOK behaviour and rejection reasons;
- cancel, modify priority rules and crossing-replace rejection;
- self-trade prevention policies;
- depth aggregation, snapshots, statistics and clearing;
- two randomized flows (20,000 and 5,000 events) that call `validate()` after
  every event to check internal invariants and that the book never crosses.

```bash
./build/orderbook_tests            # run everything
./build/orderbook_tests priority   # filter by substring
```

## Limitations and roadmap

- Single instrument, single-threaded. No lock-free queue or symbol sharding.
- No hidden/iceberg orders, stop orders, auctions or pro-rata matching.
- No persistence or market-data feed; replay is CSV only.
- `modify` does not execute a crossing replacement.

Natural next steps are a lock-free ingress queue, order gateway with risk
checks, and a binary market-data encoder for a feed handler.

## License

MIT. See [LICENSE](LICENSE).