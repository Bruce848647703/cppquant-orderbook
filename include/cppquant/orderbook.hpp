#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cppquant/pool.hpp"

namespace cppquant {

using Price = std::int64_t;
using Quantity = std::int64_t;
using OrderId = std::uint64_t;
using AccountId = std::uint32_t;
using Sequence = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

enum class OrderType : std::uint8_t {
  Limit,
  Market,
  ImmediateOrCancel,
  FillOrKill,
};

/// Controls what happens when an incoming order would trade against a resting
/// order owned by the same account.
enum class SelfTradePrevention : std::uint8_t {
  Allow,
  CancelResting,
  CancelAggressor,
};

enum class RejectReason : std::uint8_t {
  None,
  InvalidOrderId,
  InvalidPrice,
  InvalidQuantity,
  DuplicateOrderId,
  InsufficientLiquidity,
  UnknownOrder,
  MaxOrderQuantity,
  MaxOrderNotional,
  MaxPosition,
  PriceCollar,
  MaxOpenOrders,
  UnknownSymbol,
  Count,
};

const char* to_string(Side side) noexcept;
const char* to_string(OrderType type) noexcept;
const char* to_string(SelfTradePrevention policy) noexcept;
const char* to_string(RejectReason reason) noexcept;

/// A request to trade. For Market orders the price is ignored; for all other
/// types it is the limit price and must be positive.
struct Order {
  OrderId id{};
  AccountId account{};
  Side side{};
  OrderType type{OrderType::Limit};
  Price price{};
  Quantity quantity{};
};

/// An execution. Trades always print at the resting (maker) price.
struct Trade {
  Sequence sequence{};
  OrderId taker_id{};
  OrderId maker_id{};
  Price price{};
  Quantity quantity{};
  Side taker_side{};
  AccountId taker_account{};
  AccountId maker_account{};
};

struct SubmitResult {
  bool accepted{};
  RejectReason reason{RejectReason::None};
  Quantity filled{};
  /// Quantity that did not execute. If `rested` is true this quantity is on
  /// the book; otherwise it was cancelled or the order was rejected.
  Quantity remaining{};
  bool rested{};
  std::vector<Trade> trades;
  /// Resting orders removed because of self-trade prevention.
  std::vector<OrderId> cancelled;
};

/// One aggregated price level.
struct LevelView {
  Price price{};
  Quantity quantity{};
  std::size_t orders{};
};

struct Statistics {
  std::uint64_t orders_submitted{};
  std::uint64_t orders_rejected{};
  std::uint64_t orders_cancelled{};
  std::uint64_t orders_modified{};
  std::uint64_t orders_rested{};
  std::uint64_t trades_executed{};
  Quantity quantity_traded{};
  long double notional_traded{};
};

struct RestingOrderView {
  OrderId id{};
  AccountId account{};
  Side side{};
  Price price{};
  Quantity quantity{};

  bool operator==(const RestingOrderView&) const = default;
};

struct BookSnapshot {
  std::vector<LevelView> bids;
  std::vector<LevelView> asks;
  std::optional<Price> best_bid;
  std::optional<Price> best_ask;
  std::optional<Price> last_trade_price;
  std::size_t resting_orders{};
  Quantity bid_quantity{};
  Quantity ask_quantity{};
  Statistics stats;
};

/// A single-instrument limit order book with price-time priority.
///
/// Prices are integer ticks and quantities are integer lots. A limit order
/// that crosses the book executes against resting orders in price order, then
/// arrival order within a price level. The unfilled remainder rests on the
/// book. Market, IOC and FOK orders never rest.
///
/// The book is not thread-safe: callers must serialize access.
class OrderBook {
public:
  explicit OrderBook(
      SelfTradePrevention stp = SelfTradePrevention::Allow,
      std::size_t order_capacity = PoolResource::kDefaultCapacity);

  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;
  OrderBook(OrderBook&&) = delete;
  OrderBook& operator=(OrderBook&&) = delete;

  /// Submit an order. Returns the executions and the state of any remainder.
  SubmitResult submit(Order order);

  SubmitResult add_limit_order(OrderId id, Side side, Price price,
                               Quantity quantity, AccountId account = 0);
  SubmitResult add_market_order(OrderId id, Side side, Quantity quantity,
                                AccountId account = 0);
  SubmitResult add_ioc_order(OrderId id, Side side, Price price,
                             Quantity quantity, AccountId account = 0);
  SubmitResult add_fok_order(OrderId id, Side side, Price price,
                             Quantity quantity, AccountId account = 0);

  /// Remove a resting order. Returns false when the id is unknown.
  bool cancel(OrderId id);

  /// Change price and/or quantity of a resting order. Reducing quantity at the
  /// same price keeps time priority; changing price or increasing quantity
  /// sends the order to the back of its new level. Returns false on unknown
  /// ids or invalid values.
  bool modify(OrderId id, Price new_price, Quantity new_quantity);

  std::optional<Price> best_bid() const noexcept;
  std::optional<Price> best_ask() const noexcept;
  std::optional<Price> spread() const noexcept;
  std::optional<double> mid_price() const noexcept;
  std::optional<Price> last_trade_price() const noexcept;

  /// Aggregated levels, best price first.
  std::vector<LevelView> depth(Side side, std::size_t levels = 5) const;

  BookSnapshot snapshot(std::size_t levels = 5) const;

  bool contains(OrderId id) const noexcept;
  std::optional<Quantity> quantity_of(OrderId id) const noexcept;
  std::size_t resting_order_count() const noexcept;
  Quantity total_quantity(Side side) const noexcept;
  bool empty() const noexcept;

  /// Remove all orders and reset statistics. Trade sequence numbers restart.
  void clear() noexcept;

  /// Check internal invariants. Intended for tests and debug builds.
  bool validate() const noexcept;

  /// Resting orders in price-time order: bids from best to worst, then asks
  /// from best to worst.
  std::vector<RestingOrderView> resting_orders() const;

  /// Reduce a resting order's quantity while keeping its time priority.
  /// Returns false for unknown ids, non-positive quantities or a quantity
  /// greater than the current one.
  bool reduce(OrderId id, Quantity new_quantity);

  SelfTradePrevention self_trade_prevention() const noexcept {
    return stp_;
  }
  const Statistics& statistics() const noexcept {
    return stats_;
  }
  PoolStats pool_stats() const noexcept {
    return pool_.stats();
  }

private:
  struct RestingOrder {
    OrderId id{};
    AccountId account{};
    Price price{};
    Quantity quantity{};
    std::uint64_t arrival{};
  };

  using OrderList = std::list<RestingOrder, PoolAllocator<RestingOrder>>;

  struct Level {
    Quantity total{};
    OrderList orders;

    Level() = default;
    explicit Level(PoolResource* resource)
        : orders(PoolAllocator<RestingOrder>(resource)) {}
  };

  struct Location {
    Side side{};
    Price price{};
    Level* level{};
    OrderList::iterator it{};
  };

  using BidBook = std::map<Price, Level, std::greater<Price>>;
  using AskBook = std::map<Price, Level, std::less<Price>>;

  void rest_order(const Order& order, bool count_stats = true);
  bool remove_resting(OrderId id);
  Quantity available_quantity(const Order& order) const;
  Level& level_for(Side side, Price price);
  bool can_absorb(Side side, Price price, Quantity quantity) const noexcept;

  static bool crosses(Side taker_side, Price limit_price,
                      Price book_price) noexcept;

  PoolResource pool_;
  SelfTradePrevention stp_{SelfTradePrevention::Allow};
  BidBook bids_;
  AskBook asks_;
  std::unordered_map<OrderId, Location> locations_;
  std::optional<Price> last_trade_price_;
  std::uint64_t next_arrival_{1};
  Sequence next_trade_sequence_{1};
  Statistics stats_;
};

} // namespace cppquant
