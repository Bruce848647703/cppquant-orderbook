#include "cppquant/engine.hpp"
#include "test_framework.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

std::vector<EngineEvent> drain(MultiSymbolEngine& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

std::vector<EngineEvent> drain(MatchingEngine& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

Order order(OrderId id, AccountId account, Side side, Price price,
            Quantity quantity) {
  return Order{id, account, side, OrderType::Limit, price, quantity};
}

} // namespace

TEST(multi_symbol_engine_interns_symbols) {
  MultiSymbolEngine engine;
  CHECK_EQ(engine.symbol_count(), 0u);

  const SymbolId first = engine.add_symbol("AAA");
  const SymbolId second = engine.add_symbol("BBB");
  const SymbolId again = engine.add_symbol("AAA");

  CHECK_EQ(first, 1u);
  CHECK_EQ(second, 2u);
  CHECK_EQ(again, first);
  CHECK_EQ(engine.symbol_count(), 2u);
  CHECK_EQ(engine.find_symbol("AAA"), first);
  CHECK_EQ(engine.find_symbol("BBB"), second);
  CHECK_EQ(engine.find_symbol("CCC"), 0u);
  CHECK_EQ(engine.symbol_name(first), std::string("AAA"));
  CHECK(engine.book(first) != nullptr);
  CHECK(engine.book(second) != nullptr);
  CHECK(engine.book(99) == nullptr);
  CHECK_THROW(engine.add_symbol(""), std::invalid_argument);
  CHECK_THROW(engine.symbol_name(99), std::out_of_range);
}

TEST(multi_symbol_engine_keeps_books_isolated) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");
  const SymbolId bbb = engine.add_symbol("BBB");

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(1, 7, Side::Sell, 100, 5))));
  CHECK(engine.submit(
      EngineCommand::submit(bbb, order(1, 8, Side::Buy, 100, 5))));
  engine.process();

  const auto events = drain(engine);
  CHECK_EQ(events.size(), 2u);
  CHECK_EQ(events[0].symbol_id, aaa);
  CHECK_EQ(events[1].symbol_id, bbb);
  CHECK(engine.book(aaa)->best_ask() == std::optional<Price>{100});
  CHECK(!engine.book(aaa)->best_bid().has_value());
  CHECK(engine.book(bbb)->best_bid() == std::optional<Price>{100});
  CHECK(!engine.book(bbb)->best_ask().has_value());
  CHECK_EQ(engine.book(aaa)->statistics().trades_executed, 0u);
  CHECK_EQ(engine.book(bbb)->statistics().trades_executed, 0u);
}

TEST(multi_symbol_engine_matches_within_a_symbol) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");
  const SymbolId bbb = engine.add_symbol("BBB");

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(1, 0, Side::Sell, 100, 5))));
  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(2, 0, Side::Buy, 100, 3))));
  engine.process();

  const auto events = drain(engine);
  CHECK_EQ(events.size(), 3u);
  CHECK(events[0].type == EngineEventType::Accepted);
  CHECK(events[1].type == EngineEventType::Accepted);
  CHECK(events[2].type == EngineEventType::Trade);
  CHECK_EQ(events[2].symbol_id, aaa);
  CHECK_EQ(events[2].trade.price, 100);
  CHECK_EQ(events[2].trade.quantity, 3);
  CHECK_EQ(engine.book(aaa)->quantity_of(1), std::optional<Quantity>{2});
  CHECK(engine.book(bbb)->empty());
}

TEST(multi_symbol_order_ids_are_independent) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");
  const SymbolId bbb = engine.add_symbol("BBB");

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(1, 0, Side::Sell, 100, 1))));
  CHECK(engine.submit(
      EngineCommand::submit(bbb, order(1, 0, Side::Buy, 100, 1))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(EngineCommand::cancel(aaa, 1)));
  engine.process();
  const auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Cancelled);
  CHECK(engine.book(aaa)->empty());
  CHECK(engine.book(bbb)->contains(1));
}

TEST(multi_symbol_events_have_per_symbol_sequences) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");
  const SymbolId bbb = engine.add_symbol("BBB");

  CHECK(
      engine.submit(EngineCommand::submit(aaa, order(1, 0, Side::Buy, 99, 1))));
  CHECK(
      engine.submit(EngineCommand::submit(aaa, order(2, 0, Side::Buy, 98, 1))));
  CHECK(engine.submit(
      EngineCommand::submit(bbb, order(1, 0, Side::Sell, 101, 1))));
  engine.process();

  const auto events = drain(engine);
  CHECK_EQ(events.size(), 3u);
  CHECK_EQ(events[0].symbol_id, aaa);
  CHECK_EQ(events[0].sequence, 1u);
  CHECK_EQ(events[1].symbol_id, aaa);
  CHECK_EQ(events[1].sequence, 2u);
  CHECK_EQ(events[2].symbol_id, bbb);
  CHECK_EQ(events[2].sequence, 1u);
}

TEST(multi_symbol_engine_rejects_unknown_symbols) {
  MultiSymbolEngine engine;

  CHECK(
      engine.submit(EngineCommand::submit(99, order(1, 0, Side::Buy, 100, 1))));
  engine.process();

  const auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Rejected);
  CHECK(events[0].reason == RejectReason::UnknownSymbol);
  CHECK_EQ(events[0].symbol_id, 99u);
  CHECK_EQ(events[0].sequence, 0u);
  CHECK(engine.book(99) == nullptr);
}

TEST(multi_symbol_engine_matches_the_single_book_engine) {
  MatchingEngine basic;
  MultiSymbolEngine multi;
  const SymbolId aaa = multi.add_symbol("AAA");

  CHECK(basic.submit(EngineCommand::submit(order(1, 0, Side::Sell, 100, 5))));
  CHECK(multi.submit(
      EngineCommand::submit(aaa, order(1, 0, Side::Sell, 100, 5))));
  CHECK(basic.submit(EngineCommand::submit(order(2, 0, Side::Buy, 100, 3))));
  CHECK(
      multi.submit(EngineCommand::submit(aaa, order(2, 0, Side::Buy, 100, 3))));

  basic.process();
  multi.process();

  CHECK(basic.book().resting_orders() == multi.book(aaa)->resting_orders());
  CHECK(basic.book().best_bid() == multi.book(aaa)->best_bid());
  const auto basic_events = drain(basic);
  const auto multi_events = drain(multi);
  CHECK_EQ(basic_events.size(), multi_events.size());
  for (std::size_t index = 0; index < basic_events.size(); ++index) {
    CHECK(basic_events[index].type == multi_events[index].type);
    CHECK_EQ(basic_events[index].order_id, multi_events[index].order_id);
  }
}

TEST(multi_symbol_engine_emits_modify_and_reject_events) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(1, 0, Side::Sell, 100, 5))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(EngineCommand::modify(aaa, 1, 99, 3)));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Modified);
  CHECK_EQ(engine.book(aaa)->quantity_of(1), std::optional<Quantity>{3});
  CHECK(engine.book(aaa)->best_ask() == std::optional<Price>{99});

  CHECK(engine.submit(EngineCommand::modify(aaa, 1, 0, 3)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::ModifyRejected);
  CHECK(events[0].reason == RejectReason::InvalidPrice);

  CHECK(engine.submit(EngineCommand::modify(aaa, 99, 100, 1)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::ModifyRejected);
  CHECK(events[0].reason == RejectReason::UnknownOrder);

  CHECK(engine.submit(EngineCommand::cancel(aaa, 99)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::CancelRejected);
  CHECK(events[0].reason == RejectReason::UnknownOrder);
}

TEST(multi_symbol_engine_emits_self_trade_cancellations) {
  MultiSymbolEngine engine(SelfTradePrevention::CancelResting);
  const SymbolId aaa = engine.add_symbol("AAA");

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(1, 7, Side::Sell, 100, 5))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(aaa, order(2, 7, Side::Buy, 100, 3))));
  engine.process();
  const auto events = drain(engine);
  CHECK_EQ(events.size(), 2u);
  CHECK(events[0].type == EngineEventType::Accepted);
  CHECK(events[1].type == EngineEventType::SelfTradeCancelled);
  CHECK_EQ(events[1].order_id, 1u);
  CHECK(engine.book(aaa)->contains(2));
  CHECK(!engine.book(aaa)->contains(1));
}

TEST(multi_symbol_engine_reports_queue_and_event_counts) {
  MultiSymbolEngine engine;
  const SymbolId aaa = engine.add_symbol("AAA");
  const MultiSymbolEngine& const_engine = engine;
  CHECK(const_engine.book(aaa) != nullptr);
  CHECK(const_engine.book(0) == nullptr);

  CHECK(
      engine.submit(EngineCommand::submit(aaa, order(1, 0, Side::Buy, 99, 1))));
  CHECK(
      engine.submit(EngineCommand::submit(aaa, order(2, 0, Side::Buy, 98, 1))));
  CHECK(engine.pending_commands() >= 2u);
  CHECK_EQ(engine.dropped_events(), 0u);

  engine.process();
  CHECK_EQ(engine.pending_commands(), 0u);
  CHECK(engine.pending_events() >= 2u);
  drain(engine);
  CHECK_EQ(engine.pending_events(), 0u);
}
