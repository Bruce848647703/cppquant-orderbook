#include "cppquant/risk.hpp"

#include <cmath>
#include <stdexcept>

namespace cppquant {

RiskGateway::RiskGateway(RiskLimits limits) : limits_(limits) {
  if (limits_.max_order_quantity < 0) {
    throw std::invalid_argument("max order quantity must be non-negative");
  }
  if (!std::isfinite(limits_.max_order_notional) ||
      limits_.max_order_notional < 0.0) {
    throw std::invalid_argument("max order notional must be non-negative");
  }
  if (limits_.max_position < 0) {
    throw std::invalid_argument("max position must be non-negative");
  }
  if (!std::isfinite(limits_.price_collar_bps) ||
      limits_.price_collar_bps < 0.0) {
    throw std::invalid_argument("price collar must be non-negative");
  }
}

void RiskGateway::set_reference_price(std::optional<Price> reference_price) {
  if (reference_price && *reference_price <= 0) {
    throw std::invalid_argument("reference price must be positive");
  }
  reference_price_ = reference_price;
}

RiskDecision RiskGateway::check(const Order& order,
                                const RiskContext& context) const noexcept {
  if (!std::isfinite(order.quantity) || order.quantity <= 0) {
    return RiskDecision{false, RejectReason::InvalidQuantity};
  }

  if (limits_.max_order_quantity > 0 &&
      order.quantity > limits_.max_order_quantity) {
    return RiskDecision{false, RejectReason::MaxOrderQuantity};
  }

  if (limits_.max_order_notional > 0.0) {
    double price = static_cast<double>(order.price);
    if (order.type == OrderType::Market) {
      price = reference_price_ ? static_cast<double>(*reference_price_) : 0.0;
    }
    if (price > 0.0 && static_cast<double>(order.quantity) * price >
                           limits_.max_order_notional) {
      return RiskDecision{false, RejectReason::MaxOrderNotional};
    }
  }

  if (limits_.max_position > 0) {
    const Quantity signed_quantity =
        order.side == Side::Buy ? order.quantity : -order.quantity;
    const Quantity projected =
        context.position + context.working_quantity + signed_quantity;
    const Quantity magnitude = projected < 0 ? -projected : projected;
    if (magnitude > limits_.max_position) {
      return RiskDecision{false, RejectReason::MaxPosition};
    }
  }

  if (limits_.price_collar_bps > 0.0 && reference_price_ &&
      order.type != OrderType::Market) {
    const Price difference = order.price > *reference_price_
                                 ? order.price - *reference_price_
                                 : *reference_price_ - order.price;
    const double distance_bps = static_cast<double>(difference) /
                                static_cast<double>(*reference_price_) *
                                10'000.0;
    if (distance_bps > limits_.price_collar_bps) {
      return RiskDecision{false, RejectReason::PriceCollar};
    }
  }

  if (limits_.max_open_orders > 0 &&
      context.open_orders >= limits_.max_open_orders) {
    return RiskDecision{false, RejectReason::MaxOpenOrders};
  }

  return RiskDecision{true, RejectReason::None};
}

void RiskGateway::record_accept(AccountId account) noexcept {
  (void)account;
  ++accepted_;
}

void RiskGateway::record_reject(AccountId account,
                                RejectReason reason) noexcept {
  ++rejected_;
  const auto index = static_cast<std::size_t>(reason);
  if (index < rejections_.size()) {
    ++rejections_[index];
  }
  ++rejections_by_account_[account];
}

std::uint64_t RiskGateway::rejected(RejectReason reason) const noexcept {
  const auto index = static_cast<std::size_t>(reason);
  return index < rejections_.size() ? rejections_[index] : 0;
}

std::uint64_t RiskGateway::rejected(AccountId account) const noexcept {
  auto iterator = rejections_by_account_.find(account);
  return iterator == rejections_by_account_.end() ? 0 : iterator->second;
}

void RiskGateway::reset_counters() noexcept {
  rejections_.fill(0);
  rejections_by_account_.clear();
  accepted_ = 0;
  rejected_ = 0;
}

} // namespace cppquant
