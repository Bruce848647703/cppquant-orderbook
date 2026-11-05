# Pre-trade risk

`RiskGateway` checks an incoming order against a set of limits before it reaches
the order book. A rejected order is dropped before `OrderBook::submit`, so it
cannot trade, rest, or change the top of book.

## Limits

A limit value of zero disables that check.

| Limit | Field | Check |
| --- | --- | --- |
| Order quantity | `max_order_quantity` | `quantity <= limit` |
| Order notional | `max_order_notional` | `quantity * price <= limit` |
| Net position | `max_position` | `abs(position + working + signed_qty) <= limit` |
| Price collar | `price_collar_bps` | `abs(price - reference) / reference * 10000 <= limit` |
| Open orders | `max_open_orders` | `open_orders < limit` |

For market orders the notional check uses the configured reference price when
one is available; otherwise it is skipped because a market order has no price.
The price collar also applies only to priced orders. Market orders can still be
bounded by quantity, position and open-order limits.

Checks run in this order: quantity, notional, position, collar, open orders.
The first failure determines the `RejectReason`:

```text
MaxOrderQuantity
MaxOrderNotional
MaxPosition
PriceCollar
MaxOpenOrders
```

## Risk context

`check` is stateless and takes the current exposure from the caller:

```cpp
struct RiskContext {
  Quantity position;          // filled net position
  Quantity working_quantity;  // signed quantity of resting orders
  std::size_t open_orders;    // resting order count
};
```

`BasicMatchingEngine` maintains these values:

- `position` is updated from every trade, for both taker and maker accounts.
- `working_quantity` is the signed remaining quantity of accepted orders that
  rest on the book.
- `open_orders` increments when an order rests and decrements when it is fully
  filled, cancelled or removed by self-trade prevention.

Trades carry `taker_account` and `maker_account`, which makes this bookkeeping
possible without exposing private book internals.

## Counters

`RiskGateway` records:

- `accepted()` and `rejected()` totals;
- `rejected(RejectReason)` per reason;
- `rejected(AccountId)` per account;
- `rejections_by_account()` for iteration.

The engine records an acceptance only after both the risk check and
`OrderBook::submit` accept the order. A duplicate order id is therefore a
rejection attributed to the submitting account, not an acceptance.

`reset_counters()` clears the counters but leaves limits and the reference
price unchanged.

## Reference price

```cpp
engine.risk().set_reference_price(10'000);
engine.risk().set_reference_price(std::nullopt);  // disable collar/notional ref
```

The reference price is a single scalar, not a per-symbol curve. A multi-symbol
engine would supply a reference per symbol; that is part of the later
multi-symbol milestone.

## Example

```cpp
RiskLimits limits;
limits.max_order_quantity = 1'000;
limits.max_order_notional = 1'000'000;
limits.max_position = 5'000;
limits.price_collar_bps = 500;  // 5%
limits.max_open_orders = 100;

MatchingEngine engine(SelfTradePrevention::Allow,
                      PoolResource::kDefaultCapacity, limits);
engine.risk().set_reference_price(10'000);

engine.submit(EngineCommand::submit(order));
engine.process();

EngineEvent event;
while (engine.poll(event)) {
  if (event.type == EngineEventType::Rejected) {
    // event.reason is one of the risk reasons above
  }
}
```

## Interaction with order book rejections

The book still validates ids, prices, quantities and duplicate ids. If the risk
gateway accepts but the book rejects, the engine emits the book's reason and
counts it as a risk rejection for accounting purposes. This keeps one rejection
stream for callers.

## Limitations

- State is in memory and resets when the engine is recreated.
- No margin, buying-power, credit or per-symbol limits.
- No resting-order persistence or recovery; the journal milestone will need to
  replay risk state as well.
- The price collar uses one scalar reference price.
- The gateway is single-threaded with the engine thread; it is not atomic.

## Tests

- Every limit has positive and negative cases, including boundary values.
- Working orders count toward the position limit; cancelling releases the
  exposure.
- Fills update positions for taker and maker accounts.
- Cancelling the last open order releases the open-order slot.
- Market-order notional uses the reference price.
- Rejected orders leave `orders_submitted` in the book unchanged and never
  fill.
- Per-reason and per-account counters are checked directly and through the
  engine.
- Invalid limits and non-positive reference prices are rejected.
