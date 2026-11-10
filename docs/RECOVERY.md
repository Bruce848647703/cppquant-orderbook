# Journal and recovery

## Command journal

`CommandJournal` records accepted, state-changing engine commands:

- submit;
- cancel;
- modify.

Risk-rejected orders, duplicate ids, unknown cancels and failed modifies are
not journaled. Every record gets a monotonically increasing sequence starting
at 1.

```cpp
CommandJournal journal;
MatchingEngine engine(SelfTradePrevention::Allow,
                      PoolResource::kDefaultCapacity, {}, &journal);

engine.submit(EngineCommand::submit(order));
engine.process();
// journal.records() now contains one submit
```

Cancel and modify records are appended only after the book accepts them.

## Journal file format

All integers are little-endian.

File header (8 bytes):

| Size | Field |
| ---: | --- |
| 4 | Magic `CQJ1` (`0x314A5143`) |
| 2 | Version, currently 1 |
| 2 | Reserved, zero |

Each record has a 16-byte header:

| Size | Field |
| ---: | --- |
| 1 | Type: 1 submit, 2 cancel, 3 modify |
| 1 | Reserved, zero |
| 2 | Reserved, zero |
| 4 | Payload size |
| 8 | Sequence |

Payloads:

```text
submit (30 bytes):
  uint64 order_id
  uint32 account
  uint8  side (0 buy, 1 sell)
  uint8  order type (0 limit, 1 market, 2 IOC, 3 FOK)
  int64  price
  int64  quantity

cancel (8 bytes):
  uint64 order_id

modify (24 bytes):
  uint64 order_id
  int64  new_price
  int64  new_quantity
```

## Truncated files

A crash can leave a partial final record. The reader returns all complete
records and sets `JournalReadResult::truncated = true` when the remaining bytes
are shorter than a record header or payload. A bad magic, version, reserved
field, unknown type or payload-size mismatch is a hard error.

The complete prefix can be replayed; it represents the state as of the last
durable record.

## Replay

```cpp
auto read = read_journal_file("session.journal");
OrderBook book(SelfTradePrevention::Allow, 200'000);
replay_journal(read.records, book);
```

`replay_journal` requires strictly increasing sequences and throws if any
submit, cancel or modify cannot be applied. Construct the book with enough order
capacity for the expected snapshot/prefix.

## Checksums

`book_checksum` hashes resting orders in price-time order with FNV-1a over id,
account, side, price and quantity. Compare the checksum of a replayed book with
the live book to verify recovery.

## Snapshots

`write_snapshot_file` stores the same order-by-order market-data snapshot used
by the feed codec. `read_snapshot_file` decodes it; `restore_snapshot` requires
an empty book with enough capacity. A truncated snapshot is rejected.
Snapshots and journals are complementary: a snapshot bounds recovery time, and
the journal brings the book forward.

## API

```cpp
CommandJournal journal;
// attach to a BasicMatchingEngine, then:
write_journal_file(journal.records(), "session.journal");

auto read = read_journal_file("session.journal");
orderbook.restore_snapshot(book, read_snapshot_file("session.snapshot"));
replay_journal(read.records, book);
```

## Limitations

- The journal is not fsynced after every record; durability depends on the
  caller's flush policy.
- No journal compaction or rotation.
- The journal records engine commands, not risk-gateway state or multi-symbol
  engine commands.
- Replay does not enforce sequence continuity beyond strictly increasing
  order. A missing record produces a different checksum rather than an
  immediate gap error.
- Snapshot files have no checksum or compression; the market-data decoder
  validates structure but not integrity against corruption.

## Tests

- A mixed submit/trade/modify/cancel session journals, writes, reads and
  replays to the same checksum.
- Rejected commands are absent from the journal.
- A truncated tail replays the complete prefix.
- An empty journal round-trips.
- Bad magic and non-increasing sequences are rejected.
- Snapshot files round-trip and truncated snapshots are rejected.
- Checksums change with book state and reset sequences are tested.
