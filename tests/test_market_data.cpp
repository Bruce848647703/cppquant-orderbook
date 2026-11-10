#include "cppquant/market_data.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

void seed_book(OrderBook& book) {
  book.add_limit_order(1, Side::Buy, 100, 10, 7);
  book.add_limit_order(2, Side::Buy, 100, 5, 8);
  book.add_limit_order(3, Side::Buy, 99, 7, 9);
  book.add_limit_order(4, Side::Sell, 101, 8, 10);
  book.add_limit_order(5, Side::Sell, 102, 12, 11);
}

RestingOrderView order_view(OrderId id, AccountId account, Side side,
                            Price price, Quantity quantity) {
  return RestingOrderView{id, account, side, price, quantity};
}

} // namespace

TEST(snapshot_round_trips_and_restores_exactly) {
  OrderBook book(SelfTradePrevention::Allow, 64);
  seed_book(book);

  const MarketDataSnapshot snapshot = make_snapshot(book, 7, 42);
  const auto message = encode_snapshot(snapshot);
  const MarketDataSnapshot decoded = decode_snapshot(message);

  CHECK_EQ(decoded.symbol_id, 7u);
  CHECK_EQ(decoded.sequence, 42u);
  CHECK_EQ(decoded.orders.size(), snapshot.orders.size());
  CHECK(decoded.orders == snapshot.orders);

  OrderBook restored(SelfTradePrevention::Allow, 64);
  restore_snapshot(restored, decoded);
  CHECK(restored.resting_orders() == book.resting_orders());
  CHECK_EQ(restored.best_bid(), book.best_bid());
  CHECK_EQ(restored.best_ask(), book.best_ask());
  CHECK(restored.validate());
}

TEST(snapshot_round_trips_an_empty_book) {
  OrderBook book;
  const auto message = encode_snapshot(make_snapshot(book, 1, 1));
  const auto decoded = decode_snapshot(message);
  CHECK(decoded.orders.empty());

  OrderBook restored;
  restore_snapshot(restored, decoded);
  CHECK(restored.empty());
  CHECK(restored.validate());
}

TEST(delta_messages_round_trip) {
  const RestingOrderView add = order_view(11, 3, Side::Buy, 99, 4);
  const BookDelta add_delta{
      MarketDataMessageType::AddOrder, 2, 10, add, {}, {}, {}};
  const BookDelta decoded_add = decode_delta(encode_delta(add_delta));
  CHECK(decoded_add.type == MarketDataMessageType::AddOrder);
  CHECK_EQ(decoded_add.symbol_id, 2u);
  CHECK_EQ(decoded_add.sequence, 10u);
  CHECK(decoded_add.order == add);

  const BookDelta remove_delta{
      MarketDataMessageType::RemoveOrder, 2, 11, {}, 7, {}, {}};
  const BookDelta decoded_remove = decode_delta(encode_delta(remove_delta));
  CHECK(decoded_remove.type == MarketDataMessageType::RemoveOrder);
  CHECK_EQ(decoded_remove.order_id, 7u);

  const BookDelta reduce_delta{
      MarketDataMessageType::ReduceOrder, 2, 12, {}, 7, {}, 3};
  const BookDelta decoded_reduce = decode_delta(encode_delta(reduce_delta));
  CHECK_EQ(decoded_reduce.new_quantity, 3);

  const BookDelta modify_delta{
      MarketDataMessageType::ModifyOrder, 2, 13, {}, 7, 98, 5};
  const BookDelta decoded_modify = decode_delta(encode_delta(modify_delta));
  CHECK_EQ(decoded_modify.new_price, 98);
  CHECK_EQ(decoded_modify.new_quantity, 5);
}

TEST(snapshot_plus_deltas_rebuilds_the_book) {
  OrderBook book(SelfTradePrevention::Allow, 64);
  seed_book(book);

  MarketDataPublisher publisher(3, 100);
  const auto snapshot_message = publisher.snapshot(book);
  const auto snapshot = decode_snapshot(snapshot_message);

  const RestingOrderView added = order_view(6, 12, Side::Buy, 98, 3);
  const auto add_message = publisher.add(added);
  const auto reduce_message = publisher.reduce(1, 4);
  const auto modify_message = publisher.modify(3, 100, 2);
  const auto remove_message = publisher.remove(2);

  CHECK(book.add_limit_order(6, Side::Buy, 98, 3, 12).accepted);
  CHECK(book.reduce(1, 4));
  CHECK(book.modify(3, 100, 2));
  CHECK(book.cancel(2));

  OrderBook rebuilt(SelfTradePrevention::Allow, 64);
  restore_snapshot(rebuilt, snapshot);
  apply_delta(rebuilt, decode_delta(add_message));
  apply_delta(rebuilt, decode_delta(reduce_message));
  apply_delta(rebuilt, decode_delta(modify_message));
  apply_delta(rebuilt, decode_delta(remove_message));

  CHECK(rebuilt.resting_orders() == book.resting_orders());
  CHECK_EQ(rebuilt.best_bid(), book.best_bid());
  CHECK(rebuilt.validate());
}

TEST(publisher_assigns_monotonic_sequences) {
  OrderBook book;
  MarketDataPublisher publisher(9, 100);

  const auto snapshot = decode_snapshot(publisher.snapshot(book));
  CHECK_EQ(snapshot.sequence, 100u);

  const auto add =
      decode_delta(publisher.add(order_view(1, 0, Side::Buy, 100, 1)));
  CHECK_EQ(add.sequence, 101u);

  const auto remove = decode_delta(publisher.remove(1));
  CHECK_EQ(remove.sequence, 102u);
  CHECK_EQ(publisher.next_sequence(), 103u);
}

TEST(reduce_keeps_time_priority) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Buy, 100, 5);

  CHECK(book.reduce(1, 4));
  auto result = book.add_limit_order(3, Side::Sell, 100, 5);
  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 1u);
  CHECK_EQ(result.trades[0].quantity, 4);
  CHECK_EQ(result.trades[1].maker_id, 2u);
  CHECK_EQ(result.trades[1].quantity, 1);
  CHECK(book.validate());
}

TEST(wire_sizes_are_stable) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 1);
  book.add_limit_order(2, Side::Sell, 101, 1);

  const auto snapshot = encode_snapshot(make_snapshot(book, 1, 1));
  CHECK_EQ(snapshot.size(), 24u + 4u + 2u * 29u);

  const auto add = encode_delta(BookDelta{MarketDataMessageType::AddOrder,
                                          1,
                                          2,
                                          order_view(3, 0, Side::Buy, 99, 1),
                                          {},
                                          {},
                                          {}});
  CHECK_EQ(add.size(), 24u + 29u);

  CHECK_EQ(
      encode_delta(
          BookDelta{MarketDataMessageType::RemoveOrder, 1, 3, {}, 3, {}, {}})
          .size(),
      32u);
  CHECK_EQ(
      encode_delta(
          BookDelta{MarketDataMessageType::ReduceOrder, 1, 4, {}, 3, {}, 1})
          .size(),
      40u);
  CHECK_EQ(
      encode_delta(
          BookDelta{MarketDataMessageType::ModifyOrder, 1, 5, {}, 3, 98, 1})
          .size(),
      48u);
}

TEST(malformed_messages_are_rejected) {
  const RestingOrderView view = order_view(1, 0, Side::Buy, 100, 1);
  const auto valid = encode_delta(
      BookDelta{MarketDataMessageType::AddOrder, 1, 1, view, {}, {}, {}});

  auto bad = valid;
  bad[0] = std::byte{0};
  CHECK_THROW(decode_delta(bad), std::runtime_error);

  bad = valid;
  bad[4] = std::byte{2};
  CHECK_THROW(decode_delta(bad), std::runtime_error);

  bad = valid;
  bad[6] = std::byte{99};
  CHECK_THROW(decode_delta(bad), std::runtime_error);

  bad = valid;
  bad.resize(bad.size() - 1);
  CHECK_THROW(decode_delta(bad), std::runtime_error);

  bad = valid;
  bad[20] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bad[20]) + 1);
  bad.push_back(std::byte{0});
  CHECK_THROW(decode_delta(bad), std::runtime_error);

  OrderBook book;
  const auto snapshot_message = encode_snapshot(make_snapshot(book, 1, 1));
  CHECK_THROW(decode_delta(snapshot_message), std::runtime_error);
  CHECK_THROW(decode_snapshot(valid), std::runtime_error);

  auto oversized = encode_snapshot(make_snapshot(book, 1, 1));
  oversized[24] = std::byte{100};
  CHECK_THROW(decode_snapshot(oversized), std::runtime_error);
}

TEST(restore_and_delta_reject_inconsistent_state) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 1);

  MarketDataSnapshot empty;
  CHECK_THROW(restore_snapshot(book, empty), std::runtime_error);

  OrderBook empty_book;
  CHECK_THROW(
      apply_delta(
          empty_book,
          BookDelta{MarketDataMessageType::RemoveOrder, 1, 1, {}, 99, {}, {}}),
      std::runtime_error);
  CHECK_THROW(
      apply_delta(
          empty_book,
          BookDelta{MarketDataMessageType::ReduceOrder, 1, 1, {}, 99, {}, 1}),
      std::runtime_error);
  CHECK_THROW(
      apply_delta(
          empty_book,
          BookDelta{MarketDataMessageType::ModifyOrder, 1, 1, {}, 99, 100, 1}),
      std::runtime_error);

  CHECK(empty_book.add_limit_order(1, Side::Buy, 100, 1).accepted);
  CHECK_THROW(
      apply_delta(empty_book, BookDelta{MarketDataMessageType::AddOrder,
                                        1,
                                        1,
                                        order_view(1, 0, Side::Buy, 99, 1),
                                        {},
                                        {},
                                        {}}),
      std::runtime_error);
}

TEST(help_text_covers_message_types) {
  CHECK_EQ(std::string(to_string(MarketDataMessageType::Snapshot)),
           std::string("snapshot"));
  CHECK_EQ(std::string(to_string(MarketDataMessageType::AddOrder)),
           std::string("add-order"));
  CHECK_EQ(std::string(to_string(MarketDataMessageType::RemoveOrder)),
           std::string("remove-order"));
  CHECK_EQ(std::string(to_string(MarketDataMessageType::ReduceOrder)),
           std::string("reduce-order"));
  CHECK_EQ(std::string(to_string(MarketDataMessageType::ModifyOrder)),
           std::string("modify-order"));
}

TEST(decoder_rejects_a_bad_side_byte) {
  const auto valid = encode_delta(BookDelta{MarketDataMessageType::AddOrder,
                                            1,
                                            1,
                                            order_view(1, 0, Side::Buy, 100, 1),
                                            {},
                                            {},
                                            {}});
  auto bad = valid;
  // Header 24 + id 8 + account 4 = side byte at offset 36.
  bad[36] = std::byte{2};
  CHECK_THROW(decode_delta(bad), std::runtime_error);
}
