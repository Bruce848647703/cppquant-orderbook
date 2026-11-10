# Design notes

## Goals

`cppquant-orderbook` is a single-instrument matching engine that is small,
deterministic and easy to embed. It favours explicit data structures and clear
semantics over micro-optimization, while still using the structures a
production book needs: an ordered price index and stable FIFO queues.

Non-goals: multi-threading, symbol sharding, networking, persistence, auctions
and exotic order types.

## Data model

```text
                    bids_                                  asks_
        std::map<Price, Level, greater<>>        std::map<Price, Level, less<>>
                    |                                       |
                    v                                       v
        +-------------------------+             +-------------------------+
        | price 10'020            |             | price 10'025            |
        | total 30                |             | total 12                |
        | [id 4 qty 12] -> ...    |             | [id 7 qty 7] -> ...     |
        +-------------------------+             +-------------------------+
```

- Prices are `std::int64_t` ticks; quantities are `std::int64_t` lots.
- `bids_` is ordered descending and `asks_` ascending, so `begin()` is always
  the best price on either side.
- Each `Level` owns a `std::list<RestingOrder, PoolAllocator<RestingOrder>>`.
  Appending is O(1) and erasing
  an arbitrary node is O(1) once its iterator is known.
- `locations_` maps an order id to `{side, price, Level*, list iterator}`.
  `std::map` nodes and `std::list` nodes are stable, so the pointers remain
  valid while the level exists. That makes cancel O(1) on average, with an
  O(log L) map erase only when the last order at a price leaves.

There is one `Level` object per non-empty price. Empty levels are erased
immediately so the best price never points at a dead level.

## Ingress and events

`SpscQueue<T, Capacity>` is a bounded ring buffer with a power-of-two capacity.
The producer owns `tail_`, the consumer owns `head_`, and publication uses
release stores with acquire loads. Head and tail sit on separate cache lines to
avoid false sharing. Elements are constructed in place and destroyed on pop or
queue destruction; move-only types are supported.

`BasicMatchingEngine` owns the book and two queues:

```text
producer thread                   engine thread
---------------                   -------------
submit(EngineCommand)  ------->   process()
                                    |
                                    +--> OrderBook::submit/cancel/modify
                                    |
poll(EngineEvent)      <-------   accepted / rejected / trade / ...
```

Only the engine thread touches the book; the producer only publishes commands.
The command queue returns `false` when full, so the producer can apply its own
backpressure policy. Event-queue overflow is counted in `dropped_events()` and
never blocks matching.

Event order for a submission is: accepted (or rejected), then one event per
trade, then one event per resting order cancelled by self-trade prevention.
Cancel and modify emit exactly one event each.

## Pre-trade risk

`RiskGateway` checks quantity, notional, net position, price collar and
open-order limits before `OrderBook::submit`. It is stateless: the engine builds
a `RiskContext` from filled positions, signed working quantity and open-order
count. The engine updates that context from trades and from orders that rest,
fill, cancel or are removed by self-trade prevention.

Trades carry taker and maker account ids specifically so position accounting
works without exposing book internals. Rejections increment global, per-reason
and per-account counters. See [RISK.md](RISK.md) for the exact semantics.

## Market data

`market_data.hpp` defines a versioned little-endian format with a 24-byte
header, a 29-byte order record, order-by-order snapshots and four delta types
(add, remove, reduce, modify). Snapshots list bids best-to-worst, then asks
best-to-worst, FIFO inside each level.

`restore_snapshot` requires an empty book with enough capacity and rejects any
order that would trade. `apply_delta` mirrors book operations exactly;
`reduce()` preserves time priority. The decoder rejects bad magic/version,
unknown types, payload-size mismatches, truncation, trailing bytes, invalid
sides and non-positive prices or quantities.

`MarketDataPublisher` assigns monotonically increasing sequences. The codec
does not enforce sequence continuity; consumers should request a new snapshot
when they observe a gap. See [MARKET_DATA.md](MARKET_DATA.md) for the wire
layout.

## Multi-symbol engine

`MultiSymbolEngine` interns symbol names to `SymbolId` values starting at 1 and
owns one `OrderBook` per symbol. The same `EngineCommand` and `EngineEvent`
types are used by the single-book engine; the multi-symbol engine sets
`symbol_id` on every command/event and assigns a per-symbol sequence.

- `add_symbol` is idempotent and rejects empty names.
- Commands with an unknown or zero symbol id are rejected with
  `UnknownSymbol`; no book is touched.
- Order ids are scoped per book, so the same id can exist in two symbols.
- Cancel and modify target the book named by the command's symbol id.
- The replay CLI reads `timestamp,symbol,event,...` and prints per-symbol books
  and trades.

The engine still uses one SPSC pair and one engine thread. Symbol sharding
across threads, per-symbol risk contexts and a combined market-data stream are
future work.

## Journal and recovery

`CommandJournal` records accepted submit/cancel/modify commands with a
monotonic sequence. Risk-rejected and book-rejected commands are not recorded.
The binary file has an 8-byte `CQJ1` header and 16-byte record headers; payloads
are 30 bytes for submit, 8 for cancel and 24 for modify (see
[RECOVERY.md](RECOVERY.md)).

`read_journal_file` returns the complete prefix and sets `truncated` when the
last record is incomplete. Replay applies records in order and rejects
non-increasing sequences or state transitions that cannot be applied.
`book_checksum` hashes resting orders in price-time order so a replayed book can
be compared bit-for-bit with the live book.

Snapshot files reuse the market-data codec; a snapshot plus journal prefix
bounds restart time. The journal is not fsynced per record and has no
compaction.

## Matching

```text
submit(order):
  validate id, quantity, price, duplicate id
  if FOK and available_quantity(order) < order.quantity:
      reject InsufficientLiquidity
  remaining = order.quantity

  while remaining > 0 and opposite book is not empty:
      level = opposite.begin()
      if order is not Market and level.price does not cross order.price:
          break
      while remaining > 0 and level has orders:
          maker = level.front()          # oldest order at this price
          if same account and STP enabled:
              CancelResting -> remove maker and continue
              CancelAggressor -> cancel the taker remainder; stop
          executed = min(remaining, maker.quantity)
          emit trade at level.price
          remaining -= executed
          decrement maker; erase it if empty
      erase level if empty

  if remaining > 0 and order is Limit and not cancelled by STP:
      append to the back of its price level
  else:
      remaining is cancelled (market, IOC, FOK cannot rest)
```

Key properties:

- Trades print at the **resting** price. A buy crossing a 10'025 ask pays
  10'025 even if its limit is 10'030.
- Maker orders are consumed FIFO within a price level.
- The aggressor never rests before all crossing liquidity is consumed.
- Rejections happen before any mutation, except explicit STP cancellations
  which are part of matching.

`available_quantity` mirrors the matching walk for FOK orders, including the
STP policy, so a FOK is never partially filled.

## Cancel/replace priority

`modify(id, new_price, new_quantity)` implements exchange-style rules:

| Change | Result |
| --- | --- |
| Same price, quantity decreases | Priority kept; level total adjusted |
| Same price, quantity increases | Removed and appended to its level |
| Price changes (non-crossing) | Removed and appended to the new level |
| Price change would cross | Rejected, order unchanged |

A crossing replacement would have to execute against the opposite book and
produce trades, which does not fit the boolean `modify` API. Callers that want
that behaviour should `cancel` and `submit`.

## Self-trade prevention

An account id is carried on every order. Three policies are supported:

- `Allow`: trade with your own resting orders.
- `CancelResting`: remove the resting order that would have matched and keep
  matching. Removed ids are reported in `SubmitResult::cancelled`.
- `CancelAggressor`: stop matching and cancel the incoming remainder.

The policy is fixed at construction so the matching path stays branch-predictable
for the common `Allow` case.

## Rejections

| Reason | Trigger |
| --- | --- |
| `InvalidOrderId` | id 0 |
| `InvalidQuantity` | quantity <= 0 |
| `InvalidPrice` | non-market order with price <= 0 |
| `DuplicateOrderId` | id already resting |
| `InsufficientLiquidity` | FOK cannot fill completely |

Rejected submissions increment `orders_rejected` but do not otherwise touch the
book.

## Invariants

`OrderBook::validate()` checks the following and is called after every event in
the randomized tests:

1. Every level is non-empty and has a positive total.
2. Every resting order has positive quantity and matches its level price.
3. Every resting order has a location entry with the same side, price and
   level pointer.
4. The number of orders counted in levels equals `locations_.size()`.
5. Each level total equals the sum of its order quantities.
6. The best bid is strictly below the best ask.

The engine preserves these invariants by construction; the checker is there to
catch regressions in future changes.

## Numeric choices

- Integer ticks avoid floating-point equality problems in price comparisons.
- `long double` accumulates notional in statistics. Quantity × price can
  exceed 64 bits on large sessions, and `long double` keeps the benchmark and
  replay totals sane. Execution prices and quantities remain integers.
- Trade sequence numbers are monotonic per book and reset on `clear()`.

## Performance notes

The benchmark has three deterministic workloads:

1. **Passive add/cancel** keeps about 1,000 resting orders and measures the
   cost of resting, cancelling and maintaining levels.
2. **Crossing flow** alternates marketable buys and sells around a 20-tick
   band, producing roughly one trade per order.
3. **SPSC queue** runs one producer thread and one consumer thread over
   10 million messages.

The price maps give O(log L) level lookup; the matching loop is linear in the
number of fills. The dominant costs on a modern laptop are cache misses on the
hash map and list nodes, not comparisons. If the book needed millions of
orders per symbol, the next step would be a bounded tick array for levels, an
intrusive doubly-linked list embedded in the order object, and a flat hash map
for ids.

## Testing strategy

- Deterministic unit tests for every order type, priority rule, rejection path
  and STP policy.
- Queue tests for FIFO, wraparound, move-only elements, destruction, queue-full
  behaviour and a 1M-message two-thread producer/consumer run.
- Engine tests for accepted/rejected/trade/cancel/modify events, self-trade
  cancellations, command-queue pressure and dropped-event accounting.
- Risk tests for every limit, working-order exposure, fill-driven position
  updates, counter accounting and no-mutation-on-reject.
- Market data round-trip, snapshot rebuild, delta application, wire sizes and
  malformed-message tests.
- Journal replay/checksum, rejected-command omission, truncated prefixes,
  snapshot files and sequence validation.
- Multi-symbol interning, book isolation, duplicate ids across books,
  per-symbol event sequences and unknown-symbol rejection.
- A reusable replay-line parser shared by the CLI and fuzz targets, with tests
  for comments, malformed fields and direct book application.
- A CSV replay that exercises a mixed session end-to-end and is wired into
  CTest.
- libFuzzer targets for the market-data decoder and replay execution, a
  committed seed corpus and a portable mutation driver for macOS.
- A 10M-event soak executable that validates the book every 100,000 events and
  reports pool usage.
- Two randomized flows with a fixed seed. After every submit/cancel/modify the
  test calls `validate()` and checks that the top of book is not crossed. The
  first flow mixes all order types and STP; the second stresses plain limit
  orders.
- CI runs GCC, Clang and Apple clang, plus ASan/UBSan, TSan and libFuzzer
  jobs.

## Possible extensions

- Multi-producer ingress or a dispatcher feeding several engine threads.
- Stop and stop-limit orders with a trigger book.
- Iceberg orders with displayed and hidden quantities.
- Pro-rata or allocation-scheme matching per instrument.
- A sequenced egress ring for market data instead of the generic event queue.
- Binary snapshot/restore for crash recovery.
- Property tests with a reference model implemented as a simple vector scan.
