#include "cppquant/engine.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace cppquant {

MultiSymbolEngine::MultiSymbolEngine(SelfTradePrevention stp,
                                     std::size_t order_capacity)
    : stp_(stp), order_capacity_(order_capacity) {}

SymbolId MultiSymbolEngine::add_symbol(std::string symbol) {
  if (symbol.empty()) {
    throw std::invalid_argument("symbol must not be empty");
  }
  const auto existing = symbol_ids_.find(symbol);
  if (existing != symbol_ids_.end()) {
    return existing->second;
  }
  if (symbols_.size() >=
      static_cast<std::size_t>(std::numeric_limits<SymbolId>::max())) {
    throw std::length_error("too many symbols");
  }

  const auto id = static_cast<SymbolId>(symbols_.size() + 1);
  symbols_.push_back(
      SymbolInfo{id, std::move(symbol),
                 std::make_unique<OrderBook>(stp_, order_capacity_), 1});
  symbol_ids_.emplace(symbols_.back().name, id);
  return id;
}

SymbolId
MultiSymbolEngine::find_symbol(const std::string& symbol) const noexcept {
  const auto iterator = symbol_ids_.find(symbol);
  return iterator == symbol_ids_.end() ? 0 : iterator->second;
}

const std::string& MultiSymbolEngine::symbol_name(SymbolId symbol_id) const {
  if (symbol_id == 0 || symbol_id > symbols_.size()) {
    throw std::out_of_range("unknown symbol id");
  }
  return symbols_[symbol_id - 1].name;
}

std::size_t MultiSymbolEngine::symbol_count() const noexcept {
  return symbols_.size();
}

OrderBook* MultiSymbolEngine::book(SymbolId symbol_id) noexcept {
  SymbolInfo* info = find_info(symbol_id);
  return info == nullptr ? nullptr : info->book.get();
}

const OrderBook* MultiSymbolEngine::book(SymbolId symbol_id) const noexcept {
  const SymbolInfo* info = find_info(symbol_id);
  return info == nullptr ? nullptr : info->book.get();
}

MultiSymbolEngine::SymbolInfo*
MultiSymbolEngine::find_info(SymbolId symbol_id) noexcept {
  if (symbol_id == 0 || symbol_id > symbols_.size()) {
    return nullptr;
  }
  return &symbols_[symbol_id - 1];
}

const MultiSymbolEngine::SymbolInfo*
MultiSymbolEngine::find_info(SymbolId symbol_id) const noexcept {
  if (symbol_id == 0 || symbol_id > symbols_.size()) {
    return nullptr;
  }
  return &symbols_[symbol_id - 1];
}

bool MultiSymbolEngine::submit(const EngineCommand& command) {
  return inbound_.try_push(command);
}

bool MultiSymbolEngine::poll(EngineEvent& event) {
  return outbound_.try_pop(event);
}

std::size_t MultiSymbolEngine::pending_commands() const noexcept {
  return inbound_.size_approx();
}

std::size_t MultiSymbolEngine::pending_events() const noexcept {
  return outbound_.size_approx();
}

std::size_t MultiSymbolEngine::dropped_events() const noexcept {
  return dropped_events_;
}

void MultiSymbolEngine::emit(SymbolInfo& symbol, const EngineEvent& event) {
  EngineEvent copy = event;
  copy.symbol_id = symbol.id;
  copy.sequence = symbol.next_sequence++;
  if (!outbound_.try_push(copy)) {
    ++dropped_events_;
  }
}

void MultiSymbolEngine::emit_invalid(SymbolId symbol_id,
                                     const EngineEvent& event) {
  EngineEvent copy = event;
  copy.symbol_id = symbol_id;
  copy.sequence = 0;
  if (!outbound_.try_push(copy)) {
    ++dropped_events_;
  }
}

void MultiSymbolEngine::process() {
  EngineCommand command;
  while (inbound_.try_pop(command)) {
    SymbolInfo* info = find_info(command.symbol_id);
    if (info == nullptr) {
      EngineEvent event;
      event.type = EngineEventType::Rejected;
      event.reason = RejectReason::UnknownSymbol;
      event.order_id = command.type == EngineCommandType::Submit
                           ? command.order.id
                           : command.order_id;
      emit_invalid(command.symbol_id, event);
      continue;
    }

    switch (command.type) {
    case EngineCommandType::Submit: {
      const SubmitResult result = info->book->submit(command.order);
      EngineEvent event;
      event.order_id = command.order.id;
      if (!result.accepted) {
        event.type = EngineEventType::Rejected;
        event.reason = result.reason;
        emit(*info, event);
        break;
      }

      event.type = EngineEventType::Accepted;
      event.remaining = result.remaining;
      event.rested = result.rested;
      emit(*info, event);

      for (const auto& trade : result.trades) {
        EngineEvent trade_event;
        trade_event.type = EngineEventType::Trade;
        trade_event.order_id = trade.taker_id;
        trade_event.trade = trade;
        emit(*info, trade_event);
      }
      for (const OrderId cancelled : result.cancelled) {
        EngineEvent cancel_event;
        cancel_event.type = EngineEventType::SelfTradeCancelled;
        cancel_event.order_id = cancelled;
        emit(*info, cancel_event);
      }
      break;
    }
    case EngineCommandType::Cancel: {
      EngineEvent event;
      event.order_id = command.order_id;
      if (info->book->cancel(command.order_id)) {
        event.type = EngineEventType::Cancelled;
      } else {
        event.type = EngineEventType::CancelRejected;
        event.reason = RejectReason::UnknownOrder;
      }
      emit(*info, event);
      break;
    }
    case EngineCommandType::Modify: {
      EngineEvent event;
      event.order_id = command.order_id;
      if (info->book->modify(command.order_id, command.new_price,
                             command.new_quantity)) {
        event.type = EngineEventType::Modified;
      } else {
        event.type = EngineEventType::ModifyRejected;
        event.reason = RejectReason::UnknownOrder;
        if (info->book->contains(command.order_id)) {
          event.reason = command.new_price <= 0 ? RejectReason::InvalidPrice
                                                : RejectReason::InvalidQuantity;
        }
      }
      emit(*info, event);
      break;
    }
    }
  }
}

} // namespace cppquant
