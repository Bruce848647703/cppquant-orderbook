# cppquant-orderbook

A price-time-priority limit order book and matching engine written in modern
C++20, with no third-party dependencies.

[![CI](https://github.com/Bruce848647703/cppquant-orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/Bruce848647703/cppquant-orderbook/actions/workflows/ci.yml)

The project is deliberately small enough to read in one sitting, but it models
the parts that matter in a real matching engine: integer tick prices, strict
price-time priority, partial fills, market/IOC/FOK orders, cancel/replace,
self-trade prevention, execution reports and book invariants.

## Features

- **Price-time priority** via an ordered price map plus a FIFO list per level.
- **Order types**: limit, market, immediate-or-cancel (IOC), fill-or-kill
  (FOK).
- **O(1) cancel by id** using a hash map of order locations.
- **Pooled resting orders**: a chunked fixed-block allocator keeps steady-state
  add/cancel off the global heap, with pool usage exposed through `pool_stats()`.
- **Lock-free ingress**: a bounded SPSC ring buffer feeds an event-driven
  `MatchingEngine` that emits accepted/rejected/trade/cancel events.
- **Pre-trade risk**: order quantity, notional, net position, price collar and
  open-order limits, with rejection counters by reason and account.
- **Binary market data**: versioned order-by-order snapshots and add/remove/
  reduce/modify deltas, with exact book rebuild.
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
- CMake package export, CI on Linux/macOS with GCC, Clang, ASan/UBSan and TSan.

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

Reference numbers from an Apple M-series laptop (Apple clang 21, `-O3`,
median of five warmed runs):

| Scenario | Orders | ns/order | Orders/s | Peak book |
| --- | ---: | ---: | ---: | ---: |
| Passive add/cancel | 500,000 | 78.2 | 12.8 M | 1,000 |
| Crossing flow | 500,000 | 60.6 | 16.5 M | 1,159 |

The two-thread SPSC queue moves 10,000,000 messages at 28.7 ns/message
(34.9 M messages/s) on the same machine. The event engine processes 1,000,000
market commands at about 12.3 ns/command (81.5 M commands/s); enabling the risk
gateway stays within measurement noise (under 2%). A 200-order snapshot encodes
and decodes in about 16.8 µs; an add-order delta decodes in about 24 ns.

These are indicative only; run the benchmark on your own hardware. See
[docs/BENCHMARKS.md](docs/BENCHMARKS.md) for the W1 pooled-allocator
comparison, the W2 queue baseline and the W3 risk overhead.

## Design

```text
bids: std::map<Price, Level, std::greater<Price>>   best bid at begin()
asks: std::map<Price, Level, std::less<Price>>      best ask at begin()

Level:
  total quantity
  std::list<RestingOrder, PoolAllocator<RestingOrder>>   FIFO, stable iterators

locations: unordered_map<OrderId, {side, price, level pointer, list iterator}>
```

Matching walks the opposite book from the best price outwards. Within a level
it consumes orders from the front, so arrival order decides priority. Each
execution removes quantity from the maker; fully filled makers are erased from
the list, the location map and, when empty, the price level.

### Event-driven engine

```text
producer thread:  SpscQueue<EngineCommand>  --->  MatchingEngine::process()
                                                        |
                                                        v
consumer thread:  SpscQueue<EngineEvent>   <---  accepted / rejected / trade
```

`BasicMatchingEngine` owns the book and two bounded single-producer/single-
consumer queues. The producer only enqueues commands; the engine thread is the
only one that touches the book. Events that do not fit in the outbound queue
are counted in `dropped_events()` rather than blocking the engine.

### Pre-trade risk

```cpp
RiskLimits limits;
limits.max_order_quantity = 1'000;
limits.max_order_notional = 1'000'000;
limits.max_position = 5'000;
limits.price_collar_bps = 500;   // 5%
limits.max_open_orders = 100;

MatchingEngine engine(SelfTradePrevention::Allow,
                      PoolResource::kDefaultCapacity, limits);
engine.risk().set_reference_price(10'000);

engine.submit(EngineCommand::submit(order));
// rejected orders emit Rejected and never reach the book
```

`RiskGateway` is a stateless checker over a `RiskContext` (position, working
quantity, open orders). The engine maintains that context from accepted orders
and fills, so the position limit includes working-order exposure. Accepted and
rejected counts are available globally, by reason and by account.

See [docs/RISK.md](docs/RISK.md) for limit semantics and counter details.

### Binary market data

```cpp
#include "cppquant/market_data.hpp"

MarketDataPublisher publisher(/*symbol_id=*/7);
const auto snapshot_message = publisher.snapshot(book);
const auto add_message = publisher.add(resting_order);

// Consumer
OrderBook restored(SelfTradePrevention::Allow, 1024);
restore_snapshot(restored, decode_snapshot(snapshot_message));
apply_delta(restored, decode_delta(add_message));
```

The wire format is little-endian and versioned. Snapshots are order-by-order in
price-time order; deltas are add, remove, reduce and modify. Restoring a
snapshot and applying deltas reproduces the source book exactly.

See [docs/MARKET_DATA.md](docs/MARKET_DATA.md) for the byte layout and error
rules.

See [docs/DESIGN.md](docs/DESIGN.md) for the matching pseudocode, the queue
memory ordering, invariants and the reasoning behind the data structures.

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
- SPSC FIFO, wraparound, move-only elements, destruction and a 1M-message
  two-thread producer/consumer run;
- engine accept/reject/trade/cancel/modify events, self-trade cancellation,
  queue-full handling and dropped-event counting;
- risk limits (quantity, notional, position, collar, open orders), working
  exposure, counter accounting and no-mutation-on-reject;
- market data snapshot/delta round-trips, exact rebuild, sequence assignment,
  wire sizes and malformed-input rejection;
- two randomized flows (20,000 and 5,000 events) that call `validate()` after
  every event to check internal invariants and that the book never crosses.

```bash
./build/orderbook_tests            # run everything
./build/orderbook_tests priority   # filter by substring
```

## Limitations and roadmap

- Single instrument. The ingress queue is SPSC; there is no symbol sharding.
- Market data is single-symbol per message; the symbol id is reserved for the
  multi-symbol milestone.
- No hidden/iceberg orders, stop orders, auctions or pro-rata matching.
- No persistence or market-data feed; replay is CSV only.
- `modify` does not execute a crossing replacement.

Natural next steps are a lock-free ingress queue, order gateway with risk
checks, and a binary market-data encoder for a feed handler.

## Roadmap

The 8-week plan (2026-11-09 to 2027-01-03), milestones and acceptance criteria
live in [docs/ROADMAP.md](docs/ROADMAP.md). Current and historical numbers are
in [docs/BENCHMARKS.md](docs/BENCHMARKS.md); user-visible changes are tracked in
[CHANGELOG.md](CHANGELOG.md).

## License

MIT. See [LICENSE](LICENSE).
