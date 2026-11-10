#pragma once

#include "cppquant/orderbook.hpp"

#include <optional>
#include <string_view>

namespace cppquant {

enum class ReplayCommandType : std::uint8_t {
  Submit = 1,
  Cancel = 2,
  Modify = 3,
};

struct ReplayCommand {
  ReplayCommandType type{ReplayCommandType::Submit};
  /// Set for Submit.
  Order order{};
  /// Set for Cancel and Modify.
  OrderId order_id{};
  /// Set for Modify.
  Price new_price{};
  Quantity new_quantity{};
};

/// Parse one CSV replay line.
///
/// Format: `timestamp,event,id,...` where event is `add`/`limit`, `market`,
/// `ioc`, `fok`, `cancel` or `modify`. Blank lines and lines whose first
/// non-space character is `#` return `std::nullopt`. Malformed input throws
/// `std::runtime_error`.
std::optional<ReplayCommand> parse_replay_line(std::string_view line);

} // namespace cppquant