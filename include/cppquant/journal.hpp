#pragma once

#include "cppquant/market_data.hpp"
#include "cppquant/orderbook.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cppquant {

enum class JournalRecordType : std::uint8_t {
  Submit = 1,
  Cancel = 2,
  Modify = 3,
};

struct JournalRecord {
  std::uint64_t sequence{};
  JournalRecordType type{JournalRecordType::Submit};
  Order order{};
  OrderId order_id{};
  Price new_price{};
  Quantity new_quantity{};
};

/// In-memory journal of accepted, state-changing commands.
class CommandJournal {
public:
  void append_submit(const Order& order);
  void append_cancel(OrderId id);
  void append_modify(OrderId id, Price price, Quantity quantity);

  const std::vector<JournalRecord>& records() const noexcept {
    return records_;
  }
  void clear() noexcept;

private:
  std::uint64_t append(JournalRecord record);

  std::vector<JournalRecord> records_;
  std::uint64_t next_sequence_{1};
};

struct JournalReadResult {
  std::vector<JournalRecord> records;
  /// True when the file ended in the middle of a record. The complete prefix
  /// is still returned and can be replayed.
  bool truncated{};
};

/// Versioned binary journal file. See docs/RECOVERY.md.
void write_journal_file(const std::vector<JournalRecord>& records,
                        const std::string& path);
JournalReadResult read_journal_file(const std::string& path);

/// Apply journal records to a restored/empty book. Throws std::runtime_error
/// when a record cannot be applied or sequences are not strictly increasing.
void replay_journal(const std::vector<JournalRecord>& records, OrderBook& book);

/// FNV-1a checksum over resting orders in price-time order.
std::uint64_t book_checksum(const OrderBook& book) noexcept;

/// Persist a market-data snapshot as a standalone file and read it back.
void write_snapshot_file(const MarketDataSnapshot& snapshot,
                         const std::string& path);
MarketDataSnapshot read_snapshot_file(const std::string& path);

} // namespace cppquant
