#include "cppquant/engine.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace cppquant;

namespace {

std::vector<EngineEvent> drain(MatchingEngine& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

std::vector<EngineEvent> drain(BasicMatchingEngine<4, 16>& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

std::vector<EngineEvent> drain(BasicMatchingEngine<16, 2>& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

Order limit_order(OrderId id, Side side, Price price, Quantity quantity,
                  AccountId account = 0) {
  return Order{id, account, side, OrderType::Limit, price, quantity};
}

} // namespace

TEST(engine_accepts_orders_and_emits_trades) {
  MatchingEngine engine;

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Sell, 100, 5))));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Accepted);
  CHECK_EQ(events[0].order_id, 1u);
  CHECK(events[0].rested);

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(2, Side::Buy, 100, 3))));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 2u);
  CHECK(events[0].type == EngineEventType::Accepted);
  CHECK_EQ(events[0].order_id, 2u);
  CHECK(!events[0].rested);
  CHECK(events[1].type == EngineEventType::Trade);
  CHECK_EQ(events[1].trade.maker_id, 1u);
  CHECK_EQ(events[1].trade.taker_id, 2u);
  CHECK_EQ(events[1].trade.price, 100);
  CHECK_EQ(events[1].trade.quantity, 3);
  CHECK_EQ(engine.book().quantity_of(1), std::optional<Quantity>{2});
}

TEST(engine_emits_rejections) {
  MatchingEngine engine;

  CHECK(engine.submit(EngineCommand::submit(limit_order(1, Side::Buy, 99, 5))));
  engine.process();
  drain(engine);

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Sell, 101, 5))));
  engine.process();
  const auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Rejected);
  CHECK(events[0].reason == RejectReason::DuplicateOrderId);
  CHECK_EQ(engine.book().resting_order_count(), 1u);
}

TEST(engine_handles_cancel_and_modify) {
  MatchingEngine engine;

  CHECK(engine.submit(EngineCommand::submit(limit_order(1, Side::Buy, 99, 5))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(EngineCommand::cancel(1)));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Cancelled);
  CHECK(engine.book().empty());

  CHECK(engine.submit(EngineCommand::cancel(1)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::CancelRejected);
  CHECK(events[0].reason == RejectReason::UnknownOrder);

  CHECK(engine.submit(EngineCommand::submit(limit_order(2, Side::Buy, 98, 5))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(EngineCommand::modify(2, 99, 3)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Modified);
  CHECK_EQ(engine.book().best_bid(), Price{99});
  CHECK_EQ(engine.book().quantity_of(2), std::optional<Quantity>{3});

  CHECK(engine.submit(EngineCommand::modify(2, 0, 3)));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events[0].type, EngineEventType::ModifyRejected);
  CHECK(events[0].reason == RejectReason::InvalidPrice);
}

TEST(engine_reports_self_trade_cancellations) {
  MatchingEngine engine(SelfTradePrevention::CancelResting);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, Side::Sell, 100, 5, 7))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, Side::Buy, 100, 3, 7))));
  engine.process();
  const auto events = drain(engine);

  CHECK_EQ(events.size(), 2u);
  CHECK(events[0].type == EngineEventType::Accepted);
  CHECK(events[1].type == EngineEventType::SelfTradeCancelled);
  CHECK_EQ(events[1].order_id, 1u);
  CHECK(!engine.book().contains(1));
  CHECK_EQ(engine.book().best_bid(), Price{100});
}

TEST(engine_command_queue_reports_full) {
  BasicMatchingEngine<4, 16> engine;

  for (int index = 0; index < 4; ++index) {
    CHECK(engine.submit(EngineCommand::cancel(100 + index)));
  }
  CHECK_EQ(engine.pending_commands(), 4u);
  CHECK(!engine.submit(EngineCommand::cancel(999)));

  engine.process();
  CHECK_EQ(engine.pending_commands(), 0u);
  const auto events = drain(engine);
  CHECK_EQ(events.size(), 4u);
  for (const auto& event : events) {
    CHECK(event.type == EngineEventType::CancelRejected);
  }
}

TEST(engine_counts_dropped_events_when_outbound_is_full) {
  BasicMatchingEngine<16, 2> engine;

  for (OrderId id = 1; id <= 3; ++id) {
    CHECK(engine.submit(
        EngineCommand::submit(limit_order(id, Side::Sell, 100 + id, 1))));
    engine.process();
    drain(engine);
  }

  CHECK(engine.submit(
      EngineCommand::submit(Order{4, 0, Side::Buy, OrderType::Market, 0, 3})));
  engine.process();

  // Accepted + three trades need four slots, but only two are available.
  CHECK_EQ(engine.dropped_events(), 2u);
  CHECK_EQ(engine.pending_events(), 2u);
  CHECK(engine.book().empty());
}

TEST(engine_pending_counts_track_the_queues) {
  MatchingEngine engine;
  CHECK_EQ(engine.pending_commands(), 0u);
  CHECK_EQ(engine.pending_events(), 0u);

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Buy, 100, 1))));
  CHECK_EQ(engine.pending_commands(), 1u);
  CHECK_EQ(engine.pending_events(), 0u);

  engine.process();
  CHECK_EQ(engine.pending_commands(), 0u);
  CHECK_EQ(engine.pending_events(), 1u);
}
