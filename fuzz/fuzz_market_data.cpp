#include "cppquant/market_data.hpp"
#include "cppquant/orderbook.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

using namespace cppquant;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const std::span<const std::byte> message(
      reinterpret_cast<const std::byte*>(data), size);

  try {
    const MarketDataSnapshot snapshot = decode_snapshot(message);
    OrderBook book(SelfTradePrevention::Allow, snapshot.orders.size() + 1);
    restore_snapshot(book, snapshot);
    if (!book.validate()) {
      __builtin_trap();
    }
  } catch (...) {
  }

  try {
    const BookDelta delta = decode_delta(message);
    OrderBook book(SelfTradePrevention::Allow, 16);
    apply_delta(book, delta);
    if (!book.validate()) {
      __builtin_trap();
    }
  } catch (...) {
  }

  return 0;
}