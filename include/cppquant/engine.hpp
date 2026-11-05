#pragma once

#include "cppquant/orderbook.hpp"
#include "cppquant/spsc_queue.hpp"

#include <cstddef>
#include <cstdint>

namespace cppquant {

enum class EngineCommandType : std::uint8_t {
  Submit,
  Cancel,
  Modify,
};

enum class EngineEventType : std::uint8_t {
  Accepted,
  Rejected,
  Trade,
  Cancelled,
  Modified,
  CancelRejected,
  ModifyRejected,
  SelfTradeCancelled,
};

struct EngineCommand {
  EngineCommandType type{EngineCommandType::Submit};
  Order order{};
  OrderId order_id{};
  Price new_price{};
  Quantity new_quantity{};

  static EngineCommand submit(Order order) {
    EngineCommand command;
    command.type = EngineCommandType::Submit;
    command.order = order;
    return command;
  }

  static EngineCommand cancel(OrderId id) {
    EngineCommand command;
    command.type = EngineCommandType::Cancel;
    command.order_id = id;
    return command;
  }

  static EngineCommand modify(OrderId id, Price price, Quantity quantity) {
    EngineCommand command;
    command.type = EngineCommandType::Modify;
    command.order_id = id;
    command.new_price = price;
    command.new_quantity = quantity;
    return command;
  }
};

struct EngineEvent {
  EngineEventType type{EngineEventType::Accepted};
  OrderId order_id{};
  RejectReason reason{RejectReason::None};
  Quantity remaining{};
  bool rested{};
  Trade trade{};
};

inline const char* to_string(EngineCommandType type) noexcept {
  switch (type) {
  case EngineCommandType::Submit:
    return "submit";
  case EngineCommandType::Cancel:
    return "cancel";
  case EngineCommandType::Modify:
    return "modify";
  }
  return "unknown";
}

inline const char* to_string(EngineEventType type) noexcept {
  switch (type) {
  case EngineEventType::Accepted:
    return "accepted";
  case EngineEventType::Rejected:
    return "rejected";
  case EngineEventType::Trade:
    return "trade";
  case EngineEventType::Cancelled:
    return "cancelled";
  case EngineEventType::Modified:
    return "modified";
  case EngineEventType::CancelRejected:
    return "cancel-rejected";
  case EngineEventType::ModifyRejected:
    return "modify-rejected";
  case EngineEventType::SelfTradeCancelled:
    return "self-trade-cancelled";
  }
  return "unknown";
}

/// Event-driven wrapper around `OrderBook`.
///
/// One thread produces commands with `submit`; the engine thread drains them
/// with `process` and consumes events with `poll`. The order book itself is
/// only touched by the engine thread. Events that do not fit in the outbound
/// queue are counted in `dropped_events`.
template <std::size_t CommandCapacity = 1024, std::size_t EventCapacity = 4096>
class BasicMatchingEngine {
public:
  explicit BasicMatchingEngine(
      SelfTradePrevention stp = SelfTradePrevention::Allow,
      std::size_t order_capacity = PoolResource::kDefaultCapacity)
      : book_(stp, order_capacity) {}

  bool submit(const EngineCommand& command) {
    return inbound_.try_push(command);
  }

  bool poll(EngineEvent& event) {
    return outbound_.try_pop(event);
  }

  void process() {
    EngineCommand command;
    while (inbound_.try_pop(command)) {
      switch (command.type) {
      case EngineCommandType::Submit: {
        const SubmitResult result = book_.submit(command.order);
        EngineEvent event;
        event.order_id = command.order.id;
        if (!result.accepted) {
          event.type = EngineEventType::Rejected;
          event.reason = result.reason;
          emit(event);
          break;
        }

        event.type = EngineEventType::Accepted;
        event.remaining = result.remaining;
        event.rested = result.rested;
        emit(event);

        for (const auto& trade : result.trades) {
          EngineEvent trade_event;
          trade_event.type = EngineEventType::Trade;
          trade_event.order_id = trade.taker_id;
          trade_event.trade = trade;
          emit(trade_event);
        }
        for (const OrderId cancelled : result.cancelled) {
          EngineEvent cancel_event;
          cancel_event.type = EngineEventType::SelfTradeCancelled;
          cancel_event.order_id = cancelled;
          emit(cancel_event);
        }
        break;
      }
      case EngineCommandType::Cancel: {
        EngineEvent event;
        event.order_id = command.order_id;
        if (book_.cancel(command.order_id)) {
          event.type = EngineEventType::Cancelled;
        } else {
          event.type = EngineEventType::CancelRejected;
          event.reason = RejectReason::UnknownOrder;
        }
        emit(event);
        break;
      }
      case EngineCommandType::Modify: {
        EngineEvent event;
        event.order_id = command.order_id;
        if (book_.modify(command.order_id, command.new_price,
                         command.new_quantity)) {
          event.type = EngineEventType::Modified;
        } else {
          event.type = EngineEventType::ModifyRejected;
          event.reason = RejectReason::UnknownOrder;
          if (book_.contains(command.order_id)) {
            event.reason = command.new_price <= 0
                               ? RejectReason::InvalidPrice
                               : RejectReason::InvalidQuantity;
          }
        }
        emit(event);
        break;
      }
      }
    }
  }

  OrderBook& book() noexcept {
    return book_;
  }
  const OrderBook& book() const noexcept {
    return book_;
  }

  std::size_t pending_commands() const noexcept {
    return inbound_.size_approx();
  }
  std::size_t pending_events() const noexcept {
    return outbound_.size_approx();
  }
  std::size_t dropped_events() const noexcept {
    return dropped_events_;
  }

private:
  void emit(const EngineEvent& event) {
    if (!outbound_.try_push(event)) {
      ++dropped_events_;
    }
  }

  OrderBook book_;
  SpscQueue<EngineCommand, CommandCapacity> inbound_;
  SpscQueue<EngineEvent, EventCapacity> outbound_;
  std::size_t dropped_events_{};
};

using MatchingEngine = BasicMatchingEngine<>;

} // namespace cppquant
