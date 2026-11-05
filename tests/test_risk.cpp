#include "cppquant/engine.hpp"
#include "cppquant/risk.hpp"
#include "test_framework.hpp"

#include <optional>
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

Order limit_order(OrderId id, AccountId account, Side side, Price price,
                  Quantity quantity) {
  return Order{id, account, side, OrderType::Limit, price, quantity};
}

} // namespace

TEST(risk_gateway_checks_each_limit) {
  RiskLimits limits;
  limits.max_order_quantity = 10;
  limits.max_order_notional = 1'000.0;
  limits.max_position = 15;
  limits.price_collar_bps = 500.0;
  limits.max_open_orders = 2;

  RiskGateway gateway(limits);
  gateway.set_reference_price(100);

  const RiskContext context{0, 0, 0};
  const Order order = limit_order(1, 7, Side::Buy, 100, 10);

  CHECK(gateway.check(order, context).accepted);

  Order too_large = order;
  too_large.quantity = 11;
  auto decision = gateway.check(too_large, context);
  CHECK(!decision.accepted);
  CHECK(decision.reason == RejectReason::MaxOrderQuantity);

  Order too_notional = order;
  too_notional.price = 101;
  decision = gateway.check(too_notional, context);
  CHECK(!decision.accepted);
  CHECK(decision.reason == RejectReason::MaxOrderNotional);

  Order too_much_position = order;
  too_much_position.quantity = 6;
  decision = gateway.check(too_much_position, RiskContext{10, 0, 0});
  CHECK(!decision.accepted);
  CHECK(decision.reason == RejectReason::MaxPosition);

  Order outside_collar = order;
  outside_collar.price = 106;
  outside_collar.quantity = 5;
  decision = gateway.check(outside_collar, context);
  CHECK(!decision.accepted);
  CHECK(decision.reason == RejectReason::PriceCollar);

  decision = gateway.check(order, RiskContext{0, 0, 2});
  CHECK(!decision.accepted);
  CHECK(decision.reason == RejectReason::MaxOpenOrders);

  Order market = order;
  market.type = OrderType::Market;
  market.price = 0;
  market.quantity = 10;
  CHECK(gateway.check(market, RiskContext{0, 0, 0}).accepted);
}

TEST(risk_gateway_counts_accepts_and_rejects) {
  RiskLimits limits;
  limits.max_order_quantity = 5;
  RiskGateway gateway(limits);

  gateway.record_accept(7);
  gateway.record_reject(7, RejectReason::MaxOrderQuantity);
  gateway.record_reject(8, RejectReason::PriceCollar);
  gateway.record_reject(7, RejectReason::MaxOrderQuantity);

  CHECK_EQ(gateway.accepted(), 1u);
  CHECK_EQ(gateway.rejected(), 3u);
  CHECK_EQ(gateway.rejected(RejectReason::MaxOrderQuantity), 2u);
  CHECK_EQ(gateway.rejected(RejectReason::PriceCollar), 1u);
  CHECK_EQ(gateway.rejected(7), 2u);
  CHECK_EQ(gateway.rejected(8), 1u);

  gateway.reset_counters();
  CHECK_EQ(gateway.accepted(), 0u);
  CHECK_EQ(gateway.rejected(), 0u);
  CHECK_EQ(gateway.rejected(7), 0u);
}

TEST(risk_gateway_rejects_invalid_configuration) {
  RiskLimits limits;
  limits.max_order_quantity = -1;
  CHECK_THROW(RiskGateway{limits}, std::invalid_argument);

  limits = RiskLimits{};
  limits.max_order_notional = -1.0;
  CHECK_THROW(RiskGateway{limits}, std::invalid_argument);

  limits = RiskLimits{};
  limits.max_position = -1;
  CHECK_THROW(RiskGateway{limits}, std::invalid_argument);

  limits = RiskLimits{};
  limits.price_collar_bps = -1.0;
  CHECK_THROW(RiskGateway{limits}, std::invalid_argument);

  RiskGateway gateway;
  CHECK_THROW(gateway.set_reference_price(Price{0}), std::invalid_argument);
  gateway.set_reference_price(std::nullopt);
  CHECK(!gateway.reference_price().has_value());
}

TEST(engine_rejects_before_mutating_the_book) {
  RiskLimits limits;
  limits.max_order_quantity = 5;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);
  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 7, Side::Buy, 100, 6))));
  engine.process();

  const auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Rejected);
  CHECK(events[0].reason == RejectReason::MaxOrderQuantity);
  CHECK(engine.book().empty());
  CHECK(engine.book().statistics().orders_submitted == 0u);
  CHECK_EQ(engine.risk().rejected(), 1u);
  CHECK_EQ(engine.risk().rejected(7), 1u);
}

TEST(engine_position_limit_includes_working_orders) {
  RiskLimits limits;
  limits.max_position = 10;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 7, Side::Buy, 99, 8))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 7, Side::Buy, 98, 5))));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Rejected);
  CHECK(events[0].reason == RejectReason::MaxPosition);

  CHECK(engine.submit(EngineCommand::cancel(1)));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 7, Side::Buy, 98, 5))));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Accepted);
}

TEST(engine_position_limit_tracks_fills) {
  RiskLimits limits;
  limits.max_position = 10;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 8, Side::Sell, 100, 8))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(Order{2, 7, Side::Buy, OrderType::Market, 0, 8})));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(3, 7, Side::Buy, 99, 5))));
  engine.process();
  const auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Rejected);
  CHECK(events[0].reason == RejectReason::MaxPosition);

  CHECK(engine.submit(
      EngineCommand::submit(Order{4, 7, Side::Sell, OrderType::Market, 0, 3})));
  engine.process();
  const auto sell_events = drain(engine);
  CHECK_EQ(sell_events.size(), 1u);
  CHECK(sell_events[0].type == EngineEventType::Accepted);
}

TEST(engine_open_order_limit_is_released_by_cancel) {
  RiskLimits limits;
  limits.max_open_orders = 1;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 7, Side::Buy, 99, 1))));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 7, Side::Sell, 101, 1))));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].reason == RejectReason::MaxOpenOrders);

  CHECK(engine.submit(EngineCommand::cancel(1)));
  engine.process();
  drain(engine);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 7, Side::Sell, 101, 1))));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Accepted);
}

TEST(engine_price_collar_and_market_notional) {
  RiskLimits limits;
  limits.price_collar_bps = 100.0;
  limits.max_order_notional = 1'000.0;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);
  engine.risk().set_reference_price(100);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 7, Side::Buy, 101, 5))));
  engine.process();
  auto events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Accepted);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 7, Side::Buy, 102, 5))));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].reason == RejectReason::PriceCollar);

  CHECK(engine.submit(
      EngineCommand::submit(Order{3, 7, Side::Buy, OrderType::Market, 0, 11})));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].reason == RejectReason::MaxOrderNotional);

  CHECK(engine.submit(
      EngineCommand::submit(Order{4, 7, Side::Buy, OrderType::Market, 0, 10})));
  engine.process();
  events = drain(engine);
  CHECK_EQ(events.size(), 1u);
  CHECK(events[0].type == EngineEventType::Accepted);
}

TEST(engine_risk_counters_by_reason_and_account) {
  RiskLimits limits;
  limits.max_order_quantity = 5;

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);

  CHECK(engine.submit(
      EngineCommand::submit(limit_order(1, 7, Side::Buy, 100, 6))));
  CHECK(engine.submit(
      EngineCommand::submit(limit_order(2, 8, Side::Sell, 101, 7))));
  engine.process();
  drain(engine);

  CHECK_EQ(engine.risk().rejected(), 2u);
  CHECK_EQ(engine.risk().rejected(RejectReason::MaxOrderQuantity), 2u);
  CHECK_EQ(engine.risk().rejected(7), 1u);
  CHECK_EQ(engine.risk().rejected(8), 1u);
}
