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

The benchmark has two deterministic workloads:

1. **Passive add/cancel** keeps about 1,000 resting orders and measures the
   cost of resting, cancelling and maintaining levels.
2. **Crossing flow** alternates marketable buys and sells around a 20-tick
   band, producing roughly one trade per order.

The price maps give O(log L) level lookup; the matching loop is linear in the
number of fills. The dominant costs on a modern laptop are cache misses on the
hash map and list nodes, not comparisons. If the book needed millions of
orders per symbol, the next step would be a bounded tick array for levels, an
intrusive doubly-linked list embedded in the order object, and a flat hash map
for ids.

## Testing strategy

- Deterministic unit tests for every order type, priority rule, rejection path
  and STP policy.
- A CSV replay that exercises a mixed session end-to-end and is wired into
  CTest.
- Two randomized flows with a fixed seed. After every submit/cancel/modify the
  test calls `validate()` and checks that the top of book is not crossed. The
  first flow mixes all order types and STP; the second stresses plain limit
  orders.
- CI runs GCC, Clang and Apple clang, plus an ASan/UBSan job.

## Possible extensions

- Stop and stop-limit orders with a trigger book.
- Iceberg orders with displayed and hidden quantities.
- Pro-rata or allocation-scheme matching per instrument.
- A lock-free SPSC ingress queue and a sequenced egress ring for market data.
- Binary snapshot/restore for crash recovery.
- Property tests with a reference model implemented as a simple vector scan.
