#include "cppquant/orderbook.hpp"

#include <algorithm>
#include <utility>

namespace cppquant {
namespace {

SubmitResult rejected(const Order& order, RejectReason reason,
                      Statistics& stats) {
  SubmitResult result;
  result.accepted = false;
  result.reason = reason;
  result.remaining = std::max<Quantity>(order.quantity, 0);
  ++stats.orders_rejected;
  return result;
}

} // namespace

const char* to_string(Side side) noexcept {
  return side == Side::Buy ? "buy" : "sell";
}

const char* to_string(OrderType type) noexcept {
  switch (type) {
  case OrderType::Limit:
    return "limit";
  case OrderType::Market:
    return "market";
  case OrderType::ImmediateOrCancel:
    return "ioc";
  case OrderType::FillOrKill:
    return "fok";
  }
  return "unknown";
}

const char* to_string(SelfTradePrevention policy) noexcept {
  switch (policy) {
  case SelfTradePrevention::Allow:
    return "allow";
  case SelfTradePrevention::CancelResting:
    return "cancel-resting";
  case SelfTradePrevention::CancelAggressor:
    return "cancel-aggressor";
  }
  return "unknown";
}

const char* to_string(RejectReason reason) noexcept {
  switch (reason) {
  case RejectReason::None:
    return "none";
  case RejectReason::InvalidOrderId:
    return "invalid-order-id";
  case RejectReason::InvalidPrice:
    return "invalid-price";
  case RejectReason::InvalidQuantity:
    return "invalid-quantity";
  case RejectReason::DuplicateOrderId:
    return "duplicate-order-id";
  case RejectReason::InsufficientLiquidity:
    return "insufficient-liquidity";
  case RejectReason::UnknownOrder:
    return "unknown-order";
  }
  return "unknown";
}

OrderBook::OrderBook(SelfTradePrevention stp, std::size_t order_capacity)
    : pool_(order_capacity), stp_(stp) {}

bool OrderBook::crosses(Side taker_side, Price limit_price,
                        Price book_price) noexcept {
  return taker_side == Side::Buy ? limit_price >= book_price
                                 : limit_price <= book_price;
}

OrderBook::Level& OrderBook::level_for(Side side, Price price) {
  if (side == Side::Buy) {
    return bids_.try_emplace(price, &pool_).first->second;
  }
  return asks_.try_emplace(price, &pool_).first->second;
}

void OrderBook::rest_order(const Order& order, bool count_stats) {
  Level* level = &level_for(order.side, order.price);

  level->orders.push_back(RestingOrder{order.id, order.account, order.price,
                                       order.quantity, next_arrival_++});
  level->total += order.quantity;

  locations_.emplace(order.id, Location{order.side, order.price, level,
                                        std::prev(level->orders.end())});

  if (count_stats) {
    ++stats_.orders_rested;
  }
}

bool OrderBook::remove_resting(OrderId id) {
  auto location_it = locations_.find(id);
  if (location_it == locations_.end()) {
    return false;
  }

  const Location location = location_it->second;
  Level* level = location.level;

  level->total -= location.it->quantity;
  level->orders.erase(location.it);
  if (level->orders.empty()) {
    if (location.side == Side::Buy) {
      bids_.erase(location.price);
    } else {
      asks_.erase(location.price);
    }
  }

  locations_.erase(location_it);
  return true;
}

Quantity OrderBook::available_quantity(const Order& order) const {
  Quantity available = 0;

  auto scan = [&](const auto& book) {
    for (const auto& [price, level] : book) {
      if (order.type != OrderType::Market &&
          !crosses(order.side, order.price, price)) {
        return;
      }
      for (const auto& maker : level.orders) {
        if (stp_ != SelfTradePrevention::Allow &&
            maker.account == order.account) {
          if (stp_ == SelfTradePrevention::CancelAggressor) {
            return;
          }
          continue;
        }
        available += maker.quantity;
        if (available >= order.quantity) {
          return;
        }
      }
    }
  };

  if (order.side == Side::Buy) {
    scan(asks_);
  } else {
    scan(bids_);
  }
  return available;
}

SubmitResult OrderBook::submit(Order order) {
  ++stats_.orders_submitted;

  if (order.id == 0) {
    return rejected(order, RejectReason::InvalidOrderId, stats_);
  }
  if (order.quantity <= 0) {
    return rejected(order, RejectReason::InvalidQuantity, stats_);
  }
  if (order.type != OrderType::Market && order.price <= 0) {
    return rejected(order, RejectReason::InvalidPrice, stats_);
  }
  if (locations_.contains(order.id)) {
    return rejected(order, RejectReason::DuplicateOrderId, stats_);
  }

  if (order.type == OrderType::FillOrKill &&
      available_quantity(order) < order.quantity) {
    return rejected(order, RejectReason::InsufficientLiquidity, stats_);
  }

  SubmitResult result;
  result.accepted = true;
  Quantity remaining = order.quantity;
  bool aggressor_cancelled = false;

  auto match = [&](auto& book) {
    while (remaining > 0 && !book.empty()) {
      auto level_it = book.begin();
      if (order.type != OrderType::Market &&
          !crosses(order.side, order.price, level_it->first)) {
        break;
      }

      Level& level = level_it->second;
      while (remaining > 0 && !level.orders.empty()) {
        auto maker_it = level.orders.begin();
        RestingOrder& maker = *maker_it;

        if (stp_ != SelfTradePrevention::Allow &&
            maker.account == order.account) {
          if (stp_ == SelfTradePrevention::CancelAggressor) {
            aggressor_cancelled = true;
            break;
          }
          result.cancelled.push_back(maker.id);
          locations_.erase(maker.id);
          level.total -= maker.quantity;
          level.orders.erase(maker_it);
          continue;
        }

        const Quantity executed = std::min(remaining, maker.quantity);
        result.trades.push_back(Trade{next_trade_sequence_++, order.id,
                                      maker.id, level_it->first, executed,
                                      order.side});

        remaining -= executed;
        maker.quantity -= executed;
        level.total -= executed;
        stats_.quantity_traded += executed;
        stats_.notional_traded += static_cast<long double>(executed) *
                                  static_cast<long double>(level_it->first);
        last_trade_price_ = level_it->first;

        if (maker.quantity == 0) {
          locations_.erase(maker.id);
          level.orders.erase(maker_it);
        }
      }

      if (aggressor_cancelled) {
        break;
      }
      if (level.orders.empty()) {
        book.erase(level_it);
      } else {
        break;
      }
    }
  };

  if (order.side == Side::Buy) {
    match(asks_);
  } else {
    match(bids_);
  }

  result.filled = order.quantity - remaining;
  result.remaining = remaining;
  stats_.trades_executed += result.trades.size();

  if (remaining > 0 && order.type == OrderType::Limit && !aggressor_cancelled) {
    Order rest = order;
    rest.quantity = remaining;
    rest_order(rest);
    result.rested = true;
  }

  return result;
}

SubmitResult OrderBook::add_limit_order(OrderId id, Side side, Price price,
                                        Quantity quantity, AccountId account) {
  return submit(Order{id, account, side, OrderType::Limit, price, quantity});
}

SubmitResult OrderBook::add_market_order(OrderId id, Side side,
                                         Quantity quantity, AccountId account) {
  return submit(Order{id, account, side, OrderType::Market, 0, quantity});
}

SubmitResult OrderBook::add_ioc_order(OrderId id, Side side, Price price,
                                      Quantity quantity, AccountId account) {
  return submit(
      Order{id, account, side, OrderType::ImmediateOrCancel, price, quantity});
}

SubmitResult OrderBook::add_fok_order(OrderId id, Side side, Price price,
                                      Quantity quantity, AccountId account) {
  return submit(
      Order{id, account, side, OrderType::FillOrKill, price, quantity});
}

bool OrderBook::cancel(OrderId id) {
  if (!remove_resting(id)) {
    return false;
  }
  ++stats_.orders_cancelled;
  return true;
}

bool OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
  auto location_it = locations_.find(id);
  if (location_it == locations_.end() || new_price <= 0 || new_quantity <= 0) {
    return false;
  }

  const Location location = location_it->second;
  const RestingOrder current = *location.it;

  // A price change must not cross the opposite side. Supporting a crossing
  // replace would require matching the replacement, so it is rejected here
  // and callers can cancel and submit instead.
  if (new_price != current.price) {
    const auto opposite = location.side == Side::Buy ? best_ask() : best_bid();
    if (opposite && crosses(location.side, new_price, *opposite)) {
      return false;
    }
  }

  const bool keeps_priority =
      current.price == new_price && new_quantity <= current.quantity;

  if (keeps_priority) {
    location.level->total += new_quantity - current.quantity;
    location.it->quantity = new_quantity;
  } else {
    remove_resting(id);
    Order replacement{
        id,        current.account, location.side, OrderType::Limit,
        new_price, new_quantity};
    rest_order(replacement, false);
  }

  ++stats_.orders_modified;
  return true;
}

std::optional<Price> OrderBook::best_bid() const noexcept {
  if (bids_.empty()) {
    return std::nullopt;
  }
  return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const noexcept {
  if (asks_.empty()) {
    return std::nullopt;
  }
  return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const noexcept {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return *ask - *bid;
}

std::optional<double> OrderBook::mid_price() const noexcept {
  const auto bid = best_bid();
  const auto ask = best_ask();
  if (!bid || !ask) {
    return std::nullopt;
  }
  return (static_cast<double>(*bid) + static_cast<double>(*ask)) / 2.0;
}

std::optional<Price> OrderBook::last_trade_price() const noexcept {
  return last_trade_price_;
}

std::vector<LevelView> OrderBook::depth(Side side, std::size_t levels) const {
  std::vector<LevelView> out;
  out.reserve(std::min<std::size_t>(levels, locations_.size()));

  auto collect = [&](const auto& book) {
    for (const auto& [price, level] : book) {
      if (out.size() >= levels) {
        return;
      }
      out.push_back(LevelView{price, level.total, level.orders.size()});
    }
  };

  if (side == Side::Buy) {
    collect(bids_);
  } else {
    collect(asks_);
  }
  return out;
}

BookSnapshot OrderBook::snapshot(std::size_t levels) const {
  BookSnapshot snap;
  snap.bids = depth(Side::Buy, levels);
  snap.asks = depth(Side::Sell, levels);
  snap.best_bid = best_bid();
  snap.best_ask = best_ask();
  snap.last_trade_price = last_trade_price_;
  snap.resting_orders = locations_.size();
  snap.bid_quantity = total_quantity(Side::Buy);
  snap.ask_quantity = total_quantity(Side::Sell);
  snap.stats = stats_;
  return snap;
}

bool OrderBook::contains(OrderId id) const noexcept {
  return locations_.contains(id);
}

std::optional<Quantity> OrderBook::quantity_of(OrderId id) const noexcept {
  auto it = locations_.find(id);
  if (it == locations_.end()) {
    return std::nullopt;
  }
  return it->second.it->quantity;
}

std::size_t OrderBook::resting_order_count() const noexcept {
  return locations_.size();
}

Quantity OrderBook::total_quantity(Side side) const noexcept {
  Quantity total = 0;
  if (side == Side::Buy) {
    for (const auto& [price, level] : bids_) {
      total += level.total;
    }
  } else {
    for (const auto& [price, level] : asks_) {
      total += level.total;
    }
  }
  return total;
}

bool OrderBook::empty() const noexcept {
  return locations_.empty();
}

void OrderBook::clear() noexcept {
  bids_.clear();
  asks_.clear();
  locations_.clear();
  pool_.reset();
  last_trade_price_.reset();
  next_arrival_ = 1;
  next_trade_sequence_ = 1;
  stats_ = Statistics{};
}

bool OrderBook::validate() const noexcept {
  std::size_t counted = 0;

  auto check = [&](const auto& book, Side side) {
    for (const auto& [price, level] : book) {
      if (level.orders.empty() || level.total <= 0) {
        return false;
      }
      Quantity sum = 0;
      for (const auto& order : level.orders) {
        if (order.quantity <= 0 || order.price != price) {
          return false;
        }
        auto location_it = locations_.find(order.id);
        if (location_it == locations_.end() ||
            location_it->second.side != side ||
            location_it->second.price != price ||
            location_it->second.level != &level) {
          return false;
        }
        sum += order.quantity;
        ++counted;
      }
      if (sum != level.total) {
        return false;
      }
    }
    return true;
  };

  if (!check(bids_, Side::Buy) || !check(asks_, Side::Sell)) {
    return false;
  }
  if (counted != locations_.size()) {
    return false;
  }
  if (!bids_.empty() && !asks_.empty() &&
      bids_.begin()->first >= asks_.begin()->first) {
    return false;
  }
  return true;
}

} // namespace cppquant
