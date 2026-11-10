#pragma once

#include "cppquant/orderbook.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cppquant {

enum class MarketDataMessageType : std::uint8_t {
  Snapshot = 1,
  AddOrder = 2,
  RemoveOrder = 3,
  ReduceOrder = 4,
  ModifyOrder = 5,
};

const char* to_string(MarketDataMessageType type) noexcept;

struct MarketDataSnapshot {
  std::uint32_t symbol_id{};
  std::uint64_t sequence{};
  std::vector<RestingOrderView> orders;
};

struct BookDelta {
  MarketDataMessageType type{MarketDataMessageType::AddOrder};
  std::uint32_t symbol_id{};
  std::uint64_t sequence{};
  RestingOrderView order{};
  OrderId order_id{};
  Price new_price{};
  Quantity new_quantity{};
};

/// Versioned little-endian wire format. See docs/MARKET_DATA.md.
std::vector<std::byte> encode_snapshot(const MarketDataSnapshot& snapshot);
MarketDataSnapshot decode_snapshot(std::span<const std::byte> message);

std::vector<std::byte> encode_delta(const BookDelta& delta);
BookDelta decode_delta(std::span<const std::byte> message);

MarketDataSnapshot make_snapshot(const OrderBook& book, std::uint32_t symbol_id,
                                 std::uint64_t sequence);

/// Restore a snapshot into an empty book. The caller is responsible for
/// constructing the book with enough order capacity.
void restore_snapshot(OrderBook& book, const MarketDataSnapshot& snapshot);

/// Apply an add/remove/reduce/modify delta. Throws std::runtime_error when the
/// delta is inconsistent with the book.
void apply_delta(OrderBook& book, const BookDelta& delta);

/// Convenience encoder that assigns monotonically increasing sequences.
class MarketDataPublisher {
public:
  explicit MarketDataPublisher(std::uint32_t symbol_id,
                               std::uint64_t first_sequence = 1) noexcept;

  std::vector<std::byte> snapshot(const OrderBook& book);
  std::vector<std::byte> add(const RestingOrderView& order);
  std::vector<std::byte> remove(OrderId id);
  std::vector<std::byte> reduce(OrderId id, Quantity new_quantity);
  std::vector<std::byte> modify(OrderId id, Price new_price,
                                Quantity new_quantity);

  std::uint32_t symbol_id() const noexcept {
    return symbol_id_;
  }
  std::uint64_t next_sequence() const noexcept {
    return sequence_;
  }

private:
  std::uint64_t take_sequence() noexcept {
    return sequence_++;
  }

  std::uint32_t symbol_id_{};
  std::uint64_t sequence_{1};
};

} // namespace cppquant
