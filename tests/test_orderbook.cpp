#include "cppquant/orderbook.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <random>
#include <vector>

using namespace cppquant;

namespace {

std::vector<Price> prices(const std::vector<LevelView>& levels) {
  std::vector<Price> out;
  out.reserve(levels.size());
  for (const auto& level : levels) {
    out.push_back(level.price);
  }
  return out;
}

std::vector<Quantity> quantities(const std::vector<LevelView>& levels) {
  std::vector<Quantity> out;
  out.reserve(levels.size());
  for (const auto& level : levels) {
    out.push_back(level.quantity);
  }
  return out;
}

} // namespace

TEST(empty_book_has_no_top_of_book) {
  OrderBook book;

  CHECK(!book.best_bid().has_value());
  CHECK(!book.best_ask().has_value());
  CHECK(!book.spread().has_value());
  CHECK(!book.mid_price().has_value());
  CHECK(!book.last_trade_price().has_value());
  CHECK(book.empty());
  CHECK_EQ(book.resting_order_count(), 0u);
  CHECK(book.validate());
}

TEST(non_crossing_limits_rest_and_set_top_of_book) {
  OrderBook book;

  auto bid = book.add_limit_order(1, Side::Buy, 10'000, 250);
  auto ask = book.add_limit_order(2, Side::Sell, 10'020, 400);

  CHECK(bid.accepted);
  CHECK(bid.rested);
  CHECK_EQ(bid.filled, 0);
  CHECK_EQ(bid.remaining, 250);
  CHECK(ask.accepted);
  CHECK(ask.rested);

  CHECK_EQ(book.best_bid(), Price{10'000});
  CHECK_EQ(book.best_ask(), Price{10'020});
  CHECK_EQ(book.spread(), Price{20});
  CHECK_EQ(book.mid_price(), 10'010.0);
  CHECK_EQ(book.total_quantity(Side::Buy), 250);
  CHECK_EQ(book.total_quantity(Side::Sell), 400);
  CHECK_EQ(book.resting_order_count(), 2u);
  CHECK(book.validate());
}

TEST(price_time_priority_within_a_level) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 101, 5);
  book.add_limit_order(2, Side::Sell, 101, 7);

  auto result = book.add_limit_order(3, Side::Buy, 101, 6);

  CHECK(result.accepted);
  CHECK(!result.rested);
  CHECK_EQ(result.filled, 6);
  CHECK_EQ(result.remaining, 0);
  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 1u);
  CHECK_EQ(result.trades[0].quantity, 5);
  CHECK_EQ(result.trades[1].maker_id, 2u);
  CHECK_EQ(result.trades[1].quantity, 1);
  CHECK_EQ(book.quantity_of(2), std::optional<Quantity>{6});
  CHECK(!book.contains(1));
  CHECK(book.validate());
}

TEST(price_priority_across_levels) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 102, 5);
  book.add_limit_order(2, Side::Sell, 101, 5);

  auto result = book.add_limit_order(3, Side::Buy, 102, 7);

  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 2u);
  CHECK_EQ(result.trades[0].price, 101);
  CHECK_EQ(result.trades[0].quantity, 5);
  CHECK_EQ(result.trades[1].maker_id, 1u);
  CHECK_EQ(result.trades[1].price, 102);
  CHECK_EQ(result.trades[1].quantity, 2);
  CHECK_EQ(book.last_trade_price(), Price{102});
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{3});
  CHECK(book.validate());
}

TEST(limit_order_partially_filled_rests_remainder) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 10);

  auto full = book.add_limit_order(2, Side::Buy, 100, 10);

  CHECK_EQ(full.filled, 10);
  CHECK(!full.rested);
  CHECK(book.empty());

  book.add_limit_order(3, Side::Sell, 100, 10);
  auto partial = book.add_limit_order(4, Side::Buy, 100, 14);

  CHECK_EQ(partial.filled, 10);
  CHECK_EQ(partial.remaining, 4);
  CHECK(partial.rested);
  CHECK(!book.contains(3));
  CHECK_EQ(book.quantity_of(4), std::optional<Quantity>{4});
  CHECK_EQ(book.best_bid(), Price{100});
  CHECK(!book.best_ask().has_value());
  CHECK(book.validate());
}

TEST(market_order_sweeps_multiple_levels) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 3);
  book.add_limit_order(2, Side::Sell, 101, 4);
  book.add_limit_order(3, Side::Sell, 102, 5);

  auto result = book.add_market_order(4, Side::Buy, 10);

  CHECK(result.accepted);
  CHECK_EQ(result.filled, 10);
  CHECK_EQ(result.remaining, 0);
  CHECK(!result.rested);
  CHECK_EQ(result.trades.size(), 3u);
  CHECK_EQ(result.trades[0].price, 100);
  CHECK_EQ(result.trades[0].quantity, 3);
  CHECK_EQ(result.trades[1].price, 101);
  CHECK_EQ(result.trades[1].quantity, 4);
  CHECK_EQ(result.trades[2].price, 102);
  CHECK_EQ(result.trades[2].quantity, 3);
  CHECK_EQ(book.quantity_of(3), std::optional<Quantity>{2});
  CHECK_EQ(book.best_ask(), Price{102});
  CHECK(book.validate());
}

TEST(market_order_with_empty_book_is_cancelled) {
  OrderBook book;

  auto result = book.add_market_order(1, Side::Buy, 25);

  CHECK(result.accepted);
  CHECK_EQ(result.filled, 0);
  CHECK_EQ(result.remaining, 25);
  CHECK(!result.rested);
  CHECK(result.trades.empty());
  CHECK(book.empty());
  CHECK(book.validate());
}

TEST(ioc_order_cancels_unfilled_remainder) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 3);

  auto result = book.add_ioc_order(2, Side::Buy, 101, 10);

  CHECK(result.accepted);
  CHECK_EQ(result.filled, 3);
  CHECK_EQ(result.remaining, 7);
  CHECK(!result.rested);
  CHECK(book.empty());
  CHECK(book.validate());
}

TEST(fok_order_rejected_when_liquidity_is_insufficient) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 3);
  book.add_limit_order(2, Side::Sell, 102, 10);

  auto result = book.add_fok_order(3, Side::Buy, 101, 5);

  CHECK(!result.accepted);
  CHECK(result.reason == RejectReason::InsufficientLiquidity);
  CHECK_EQ(result.filled, 0);
  CHECK_EQ(result.remaining, 5);
  CHECK(result.trades.empty());
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{3});
  CHECK_EQ(book.quantity_of(2), std::optional<Quantity>{10});
  CHECK(book.validate());
}

TEST(fok_order_fills_completely_when_liquidity_exists) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 3);
  book.add_limit_order(2, Side::Sell, 102, 10);

  auto result = book.add_fok_order(3, Side::Buy, 102, 8);

  CHECK(result.accepted);
  CHECK_EQ(result.filled, 8);
  CHECK_EQ(result.remaining, 0);
  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].price, 100);
  CHECK_EQ(result.trades[1].price, 102);
  CHECK_EQ(book.quantity_of(2), std::optional<Quantity>{5});
  CHECK(book.validate());
}

TEST(trades_execute_at_the_resting_price) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 5);

  auto result = book.add_limit_order(2, Side::Buy, 105, 5);

  CHECK_EQ(result.trades.size(), 1u);
  CHECK_EQ(result.trades[0].price, 100);
  CHECK_EQ(result.trades[0].taker_id, 2u);
  CHECK_EQ(result.trades[0].maker_id, 1u);
  CHECK_EQ(result.trades[0].taker_side, Side::Buy);
  CHECK_EQ(book.last_trade_price(), Price{100});
  CHECK(book.empty());
}

TEST(cancel_removes_order_and_updates_top_of_book) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Buy, 99, 20);

  CHECK(book.cancel(1));
  CHECK(!book.contains(1));
  CHECK_EQ(book.best_bid(), Price{99});
  CHECK_EQ(book.quantity_of(2), std::optional<Quantity>{20});
  CHECK(book.validate());

  CHECK(book.cancel(2));
  CHECK(!book.best_bid().has_value());
  CHECK(book.empty());
  CHECK(book.validate());
}

TEST(cancel_unknown_order_returns_false) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);

  CHECK(!book.cancel(999));
  CHECK_EQ(book.statistics().orders_cancelled, 0u);
  CHECK_EQ(book.resting_order_count(), 1u);
}

TEST(duplicate_order_id_is_rejected) {
  OrderBook book;
  CHECK(book.add_limit_order(1, Side::Buy, 100, 10).accepted);

  auto result = book.add_limit_order(1, Side::Sell, 200, 5);

  CHECK(!result.accepted);
  CHECK(result.reason == RejectReason::DuplicateOrderId);
  CHECK_EQ(book.resting_order_count(), 1u);
  CHECK_EQ(book.best_bid(), Price{100});
  CHECK(!book.best_ask().has_value());
}

TEST(invalid_id_price_and_quantity_are_rejected) {
  OrderBook book;

  auto invalid_id =
      book.submit(Order{0, 0, Side::Buy, OrderType::Limit, 100, 1});
  CHECK(!invalid_id.accepted);
  CHECK(invalid_id.reason == RejectReason::InvalidOrderId);

  auto invalid_quantity = book.add_limit_order(1, Side::Buy, 100, 0);
  CHECK(!invalid_quantity.accepted);
  CHECK(invalid_quantity.reason == RejectReason::InvalidQuantity);

  auto invalid_price = book.add_limit_order(1, Side::Buy, 0, 10);
  CHECK(!invalid_price.accepted);
  CHECK(invalid_price.reason == RejectReason::InvalidPrice);

  auto market = book.add_market_order(1, Side::Buy, 10);
  CHECK(market.accepted);

  CHECK_EQ(book.statistics().orders_rejected, 3u);
  CHECK(book.empty());
}

TEST(modify_quantity_down_keeps_time_priority) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Buy, 100, 10);

  CHECK(book.modify(1, 100, 5));
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{5});
  CHECK_EQ(book.total_quantity(Side::Buy), 15);

  auto result = book.add_limit_order(3, Side::Sell, 100, 8);

  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 1u);
  CHECK_EQ(result.trades[0].quantity, 5);
  CHECK_EQ(result.trades[1].maker_id, 2u);
  CHECK_EQ(result.trades[1].quantity, 3);
  CHECK(book.validate());
}

TEST(modify_quantity_up_loses_time_priority) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Buy, 100, 10);

  CHECK(book.modify(1, 100, 15));

  auto result = book.add_limit_order(3, Side::Sell, 100, 20);

  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 2u);
  CHECK_EQ(result.trades[0].quantity, 10);
  CHECK_EQ(result.trades[1].maker_id, 1u);
  CHECK_EQ(result.trades[1].quantity, 10);
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{5});
  CHECK(book.validate());
}

TEST(modify_price_moves_order_behind_the_new_level) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Buy, 99, 10);

  CHECK(book.modify(2, 100, 10));
  CHECK_EQ(book.resting_order_count(), 2u);
  CHECK_EQ(book.best_bid(), Price{100});

  auto result = book.add_limit_order(3, Side::Sell, 100, 15);

  CHECK_EQ(result.trades.size(), 2u);
  CHECK_EQ(result.trades[0].maker_id, 1u);
  CHECK_EQ(result.trades[1].maker_id, 2u);
  CHECK(book.validate());
}

TEST(modify_cannot_cross_the_book) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Sell, 105, 10);

  CHECK(!book.modify(1, 105, 10));
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{10});
  CHECK_EQ(book.best_ask(), Price{105});
  CHECK(book.validate());

  CHECK(!book.modify(2, 100, 10));
  CHECK_EQ(book.quantity_of(2), std::optional<Quantity>{10});
  CHECK_EQ(book.best_bid(), Price{100});
  CHECK(book.validate());
}

TEST(modify_rejects_unknown_ids_and_invalid_values) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);

  CHECK(!book.modify(999, 100, 5));
  CHECK(!book.modify(1, 0, 5));
  CHECK(!book.modify(1, 100, 0));
  CHECK(!book.modify(1, 100, -1));
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{10});
  CHECK_EQ(book.statistics().orders_modified, 0u);
  CHECK(book.validate());
}

TEST(self_trade_prevention_cancels_resting_order) {
  OrderBook book(SelfTradePrevention::CancelResting);
  book.add_limit_order(1, Side::Sell, 100, 5, 7);
  book.add_limit_order(2, Side::Sell, 100, 5, 8);

  auto result = book.add_limit_order(3, Side::Buy, 100, 8, 7);

  CHECK(result.accepted);
  CHECK_EQ(result.cancelled.size(), 1u);
  CHECK_EQ(result.cancelled[0], 1u);
  CHECK_EQ(result.trades.size(), 1u);
  CHECK_EQ(result.trades[0].maker_id, 2u);
  CHECK_EQ(result.trades[0].quantity, 5);
  CHECK_EQ(result.remaining, 3);
  CHECK(result.rested);
  CHECK(!book.contains(1));
  CHECK_EQ(book.best_bid(), Price{100});
  CHECK_EQ(book.total_quantity(Side::Buy), 3);
  CHECK(book.validate());
}

TEST(self_trade_prevention_cancels_aggressor) {
  OrderBook book(SelfTradePrevention::CancelAggressor);
  book.add_limit_order(1, Side::Sell, 100, 5, 7);
  book.add_limit_order(2, Side::Sell, 100, 5, 8);

  auto result = book.add_limit_order(3, Side::Buy, 100, 8, 7);

  CHECK(result.accepted);
  CHECK(result.trades.empty());
  CHECK(result.cancelled.empty());
  CHECK_EQ(result.filled, 0);
  CHECK_EQ(result.remaining, 8);
  CHECK(!result.rested);
  CHECK(book.contains(1));
  CHECK(book.contains(2));
  CHECK_EQ(book.total_quantity(Side::Sell), 10);
  CHECK(book.validate());
}

TEST(self_trade_prevention_allow_trades_with_the_same_account) {
  OrderBook book(SelfTradePrevention::Allow);
  book.add_limit_order(1, Side::Sell, 100, 5, 7);

  auto result = book.add_limit_order(2, Side::Buy, 100, 5, 7);

  CHECK_EQ(result.trades.size(), 1u);
  CHECK_EQ(result.filled, 5);
  CHECK_EQ(result.cancelled.size(), 0u);
  CHECK(book.empty());
}

TEST(depth_aggregates_orders_by_price) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 3);
  book.add_limit_order(2, Side::Buy, 100, 4);
  book.add_limit_order(3, Side::Buy, 99, 5);
  book.add_limit_order(4, Side::Sell, 101, 6);
  book.add_limit_order(5, Side::Sell, 102, 7);

  auto bids = book.depth(Side::Buy, 2);
  CHECK_EQ(bids.size(), 2u);
  CHECK_EQ(bids[0].price, 100);
  CHECK_EQ(bids[0].quantity, 7);
  CHECK_EQ(bids[0].orders, 2u);
  CHECK_EQ(bids[1].price, 99);
  CHECK_EQ(bids[1].quantity, 5);
  CHECK_EQ(bids[1].orders, 1u);

  auto asks = book.depth(Side::Sell, 5);
  CHECK_EQ(prices(asks), (std::vector<Price>{101, 102}));
  CHECK_EQ(quantities(asks), (std::vector<Quantity>{6, 7}));
  CHECK(book.validate());
}

TEST(snapshot_reports_both_sides_and_statistics) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Sell, 101, 12);
  book.add_limit_order(3, Side::Buy, 100, 5);
  book.add_limit_order(4, Side::Sell, 101, 8);

  auto snap = book.snapshot(1);

  CHECK_EQ(snap.bids.size(), 1u);
  CHECK_EQ(snap.asks.size(), 1u);
  CHECK_EQ(snap.best_bid, std::optional<Price>{100});
  CHECK_EQ(snap.best_ask, std::optional<Price>{101});
  CHECK_EQ(snap.bid_quantity, 15);
  CHECK_EQ(snap.ask_quantity, 20);
  CHECK_EQ(snap.resting_orders, 4u);
  CHECK_EQ(snap.stats.orders_rested, 4u);
  CHECK(!snap.last_trade_price.has_value());
}

TEST(statistics_track_activity) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 5);
  book.add_limit_order(2, Side::Buy, 99, 4);
  book.add_limit_order(3, Side::Buy, 100, 3);

  book.cancel(2);
  CHECK(book.modify(1, 100, 6));

  const auto& stats = book.statistics();
  CHECK_EQ(stats.orders_submitted, 3u);
  CHECK_EQ(stats.orders_rejected, 0u);
  CHECK_EQ(stats.orders_cancelled, 1u);
  CHECK_EQ(stats.orders_modified, 1u);
  CHECK_EQ(stats.orders_rested, 2u);
  CHECK_EQ(stats.trades_executed, 1u);
  CHECK_EQ(stats.quantity_traded, 3);
  CHECK_NEAR(stats.notional_traded, 300.0L, 1e-9L);
  CHECK_EQ(book.last_trade_price(), Price{100});
  CHECK(book.validate());
}

TEST(clear_resets_the_book_and_statistics) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 99, 5);
  book.add_limit_order(2, Side::Sell, 101, 5);

  CHECK(!book.empty());
  book.clear();

  CHECK(book.empty());
  CHECK(!book.best_bid().has_value());
  CHECK(!book.best_ask().has_value());
  CHECK(!book.last_trade_price().has_value());
  CHECK_EQ(book.resting_order_count(), 0u);
  CHECK_EQ(book.statistics().orders_submitted, 0u);
  CHECK(book.validate());
}

TEST(trade_sequence_numbers_are_monotonic) {
  OrderBook book;
  book.add_limit_order(1, Side::Sell, 100, 2);
  book.add_limit_order(2, Side::Sell, 101, 2);
  book.add_limit_order(3, Side::Sell, 102, 2);

  auto first = book.add_market_order(4, Side::Buy, 3);
  auto second = book.add_market_order(5, Side::Buy, 3);

  CHECK_EQ(first.trades.size(), 2u);
  CHECK_EQ(second.trades.size(), 2u);
  CHECK_EQ(first.trades[0].sequence, 1u);
  CHECK_EQ(first.trades[1].sequence, 2u);
  CHECK_EQ(second.trades[0].sequence, 3u);
  CHECK_EQ(second.trades[1].sequence, 4u);
  CHECK(book.validate());
}

TEST(buy_and_sell_cancels_are_independent) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 10);
  book.add_limit_order(2, Side::Sell, 105, 20);

  CHECK(book.cancel(1));
  CHECK(!book.best_bid().has_value());

  CHECK_EQ(book.best_ask(), Price{105});
  CHECK_EQ(book.total_quantity(Side::Sell), 20);
  CHECK(book.validate());
}

TEST(randomized_flow_keeps_internal_invariants) {
  OrderBook book(SelfTradePrevention::CancelResting);
  std::mt19937_64 rng(20261104);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  auto random_side = [&] { return (rng() & 1U) != 0 ? Side::Buy : Side::Sell; };

  for (int step = 0; step < 20'000; ++step) {
    live.erase(std::remove_if(live.begin(), live.end(),
                              [&](OrderId id) { return !book.contains(id); }),
               live.end());

    const auto action = rng() % 100;

    if (action < 65 || live.empty()) {
      const auto id = next_id++;
      const auto side = random_side();
      const auto price = static_cast<Price>(9'500 + rng() % 101);
      const auto quantity = static_cast<Quantity>(1 + rng() % 20);
      const auto account = static_cast<AccountId>(rng() % 4);
      const auto type = rng() % 10;

      SubmitResult result;
      if (type == 0) {
        result = book.add_market_order(id, side, quantity, account);
      } else if (type == 1) {
        result = book.add_ioc_order(id, side, price, quantity, account);
      } else if (type == 2) {
        result = book.add_fok_order(id, side, price, quantity, account);
      } else {
        result = book.add_limit_order(id, side, price, quantity, account);
      }
      if (type == 2 && !result.accepted) {
        CHECK(result.reason == RejectReason::InsufficientLiquidity);
      } else {
        CHECK(result.accepted);
      }
      if (result.rested) {
        live.push_back(id);
      }
    } else if (action < 85) {
      const auto index = rng() % live.size();
      CHECK(book.cancel(live[index]));
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      const auto index = rng() % live.size();
      const auto id = live[index];
      const auto price = static_cast<Price>(9'500 + rng() % 101);
      const auto quantity = static_cast<Quantity>(1 + rng() % 20);
      (void)book.modify(id, price, quantity);
    }

    CHECK(book.validate());
    if (book.best_bid() && book.best_ask()) {
      CHECK(*book.best_bid() < *book.best_ask());
    }
  }

  for (const auto id : live) {
    CHECK(book.cancel(id));
  }

  CHECK(book.empty());
  CHECK(book.validate());
}

TEST(randomized_flow_with_allow_policy_keeps_invariants) {
  OrderBook book(SelfTradePrevention::Allow);
  std::mt19937_64 rng(7);
  std::vector<OrderId> live;
  OrderId next_id = 1;

  for (int step = 0; step < 5'000; ++step) {
    live.erase(std::remove_if(live.begin(), live.end(),
                              [&](OrderId id) { return !book.contains(id); }),
               live.end());

    if (live.empty() || rng() % 3 != 0) {
      const auto id = next_id++;
      const auto side = (rng() & 1U) != 0 ? Side::Buy : Side::Sell;
      const auto price = static_cast<Price>(100 + rng() % 11);
      const auto quantity = static_cast<Quantity>(1 + rng() % 50);
      auto result = book.add_limit_order(id, side, price, quantity,
                                         static_cast<AccountId>(rng() % 2));
      if (result.rested) {
        live.push_back(id);
      }
    } else {
      const auto index = rng() % live.size();
      CHECK(book.cancel(live[index]));
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(index));
    }
    CHECK(book.validate());
  }
}
