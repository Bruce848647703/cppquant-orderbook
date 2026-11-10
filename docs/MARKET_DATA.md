# Binary market data

`cppquant-orderbook` publishes a versioned, little-endian wire format for
order-by-order book snapshots and deltas. It is intentionally small: enough to
rebuild a book and resume matching, without compression or a checksum layer.

## Header

Every message starts with a 24-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 4 | Magic `CQMD` (`0x444D5143` little-endian) |
| 4 | 2 | Version, currently `1` |
| 6 | 1 | Message type |
| 7 | 1 | Reserved, must be `0` |
| 8 | 4 | Symbol id |
| 12 | 8 | Sequence |
| 20 | 4 | Payload size in bytes |

All integers are little-endian. The header size plus the payload size must
equal the message size exactly; trailing bytes are rejected.

Message types:

```text
1  snapshot
2  add order
3  remove order
4  reduce order
5  modify order
```

## Order record

Snapshots and add-order deltas share a 29-byte order record:

| Size | Field |
| ---: | --- |
| 8 | Order id |
| 4 | Account id |
| 1 | Side, `0` = buy, `1` = sell |
| 8 | Price, signed 64-bit ticks |
| 8 | Quantity, signed 64-bit lots |

Prices and quantities must be positive. Unknown side bytes are rejected.

## Snapshot payload

```text
uint32 order_count
order_record[order_count]
```

Orders are emitted in price-time order: bids from best to worst, then asks from
best to worst, FIFO inside each level. Rebuilding the book in that order
preserves price-time priority.

A snapshot is a full replacement of the current book. The decoder rejects a
count that cannot fit in the payload and any trailing bytes.

## Delta payloads

**Add order** (type 2): one order record.

**Remove order** (type 3):

```text
uint64 order_id
```

**Reduce order** (type 4):

```text
uint64 order_id
int64  new_quantity
```

The new quantity must be positive and no greater than the current quantity.
Reducing preserves time priority.

**Modify order** (type 5):

```text
uint64 order_id
int64  new_price
int64  new_quantity
```

A price change or size increase moves the order to the back of its level, as
with `OrderBook::modify`.

## Sequence numbers

The sequence is assigned by the publisher and increases by one for every
message. `MarketDataPublisher` starts at a caller-provided value. A consumer can
use gaps to detect loss, but the codec itself does not enforce continuity
because snapshots can legitimately reset a stream.

## Rebuilding

```cpp
// Producer
MarketDataPublisher publisher(/*symbol_id=*/7);
auto snapshot_message = publisher.snapshot(book);
auto add_message = publisher.add(resting_order_view);

// Consumer
auto snapshot = decode_snapshot(snapshot_message);
OrderBook restored(SelfTradePrevention::Allow, snapshot.orders.size());
restore_snapshot(restored, snapshot);

auto delta = decode_delta(add_message);
apply_delta(restored, delta);
```

`restore_snapshot` requires an empty book and enough order capacity. Every
snapshot order must rest without matching; otherwise the restore fails.
`apply_delta` throws when an add is a duplicate or would cross, when a remove
references an unknown id, or when a reduce/modify cannot be applied.

## Validation

The decoder rejects:

- messages shorter than the header;
- bad magic, version or reserved byte;
- unknown message type;
- payload-size mismatch or trailing payload bytes;
- truncated order records;
- invalid side bytes, non-positive prices or quantities;
- snapshot counts that exceed the payload;
- a delta decoder receiving a snapshot and vice versa.

Errors are reported as `std::runtime_error` (invalid arguments use
`std::invalid_argument` when encoding a snapshot as a delta).

## Benchmark

On the reference machine, a 200-order snapshot (5,828 bytes) encodes and decodes
in about 16.8 µs including vector allocations. A 53-byte add-order delta decodes
in about 24 ns (41 M messages/s). Snapshot throughput is dominated by
allocation and record count, not by the price/quantity conversion.

## Limitations

- Single symbol per message; the symbol id is already present for the future
  multi-symbol engine.
- Order-by-order snapshots only; no price-level-only snapshot variant.
- No compression, CRC or authentication.
- The codec does not implement gap recovery; consumers must request a new
  snapshot when a sequence gap is observed.
- No fragmented or streaming frame decoder yet; each message is a complete
  byte span.
