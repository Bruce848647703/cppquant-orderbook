#pragma once

#include "cppquant/orderbook.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace cppquant {

/// Pre-trade limits. A value of zero disables that limit.
struct RiskLimits {
  Quantity max_order_quantity{};
  double max_order_notional{};
  /// Absolute net position limit, including working-order exposure.
  Quantity max_position{};
  /// Maximum distance of a limit price from the reference price.
  double price_collar_bps{};
  std::size_t max_open_orders{};
};

/// Current risk exposure supplied by the caller. `working_quantity` is the
/// signed quantity of orders already resting for the account.
struct RiskContext {
  Quantity position{};
  Quantity working_quantity{};
  std::size_t open_orders{};
};

struct RiskDecision {
  bool accepted{};
  RejectReason reason{RejectReason::None};
};

/// Stateless limit checker plus accepted/rejected counters.
///
/// `check` does not mutate any state, so a rejected order can be dropped
/// before it reaches the matching engine. `record_accept` and `record_reject`
/// are called by the engine after the decision is applied.
class RiskGateway {
public:
  RiskGateway() = default;
  explicit RiskGateway(RiskLimits limits);

  void set_reference_price(std::optional<Price> reference_price);
  std::optional<Price> reference_price() const noexcept {
    return reference_price_;
  }

  const RiskLimits& limits() const noexcept {
    return limits_;
  }

  RiskDecision check(const Order& order,
                     const RiskContext& context) const noexcept;

  void record_accept(AccountId account) noexcept;
  void record_reject(AccountId account, RejectReason reason) noexcept;

  std::uint64_t accepted() const noexcept {
    return accepted_;
  }
  std::uint64_t rejected() const noexcept {
    return rejected_;
  }
  std::uint64_t rejected(RejectReason reason) const noexcept;
  std::uint64_t rejected(AccountId account) const noexcept;

  const std::unordered_map<AccountId, std::uint64_t>&
  rejections_by_account() const noexcept {
    return rejections_by_account_;
  }

  void reset_counters() noexcept;

private:
  RiskLimits limits_{};
  std::optional<Price> reference_price_;
  std::array<std::uint64_t, static_cast<std::size_t>(RejectReason::Count)>
      rejections_{};
  std::unordered_map<AccountId, std::uint64_t> rejections_by_account_;
  std::uint64_t accepted_{};
  std::uint64_t rejected_{};
};

} // namespace cppquant
