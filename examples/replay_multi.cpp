#include "cppquant/engine.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

std::string trim(const std::string& value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

std::vector<std::string> split(const std::string& line) {
  std::vector<std::string> fields;
  std::string field;
  std::istringstream stream(line);
  while (std::getline(stream, field, ',')) {
    fields.push_back(trim(field));
  }
  return fields;
}

Side parse_side(const std::string& text) {
  if (text == "B" || text == "BUY" || text == "buy" || text == "Buy") {
    return Side::Buy;
  }
  if (text == "S" || text == "SELL" || text == "sell" || text == "Sell") {
    return Side::Sell;
  }
  throw std::runtime_error("invalid side: " + text);
}

OrderType parse_type(const std::string& text) {
  if (text == "limit" || text == "add") {
    return OrderType::Limit;
  }
  if (text == "market") {
    return OrderType::Market;
  }
  if (text == "ioc") {
    return OrderType::ImmediateOrCancel;
  }
  if (text == "fok") {
    return OrderType::FillOrKill;
  }
  throw std::runtime_error("invalid order type: " + text);
}

long long number(const std::vector<std::string>& fields, std::size_t index,
                 const char* what) {
  if (index >= fields.size() || fields[index].empty()) {
    throw std::runtime_error(std::string("missing ") + what);
  }
  return std::stoll(fields[index]);
}

void print_book(const std::string& symbol, const OrderBook& book) {
  std::printf("\n[%s]\n", symbol.c_str());
  const auto snapshot = book.snapshot(3);
  for (const auto& level : snapshot.bids) {
    std::printf("  bid %10lld x %8lld (%zu)\n",
                static_cast<long long>(level.price),
                static_cast<long long>(level.quantity), level.orders);
  }
  for (const auto& level : snapshot.asks) {
    std::printf("  ask %10lld x %8lld (%zu)\n",
                static_cast<long long>(level.price),
                static_cast<long long>(level.quantity), level.orders);
  }
  if (snapshot.bids.empty() && snapshot.asks.empty()) {
    std::printf("  (empty)\n");
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 2) {
    std::fprintf(stderr, "usage: %s [events.csv]\n", argv[0]);
    return 2;
  }

  std::ifstream file;
  std::istream* input = &std::cin;
  if (argc == 2) {
    file.open(argv[1]);
    if (!file) {
      std::fprintf(stderr, "cannot open %s\n", argv[1]);
      return 1;
    }
    input = &file;
  }

  MultiSymbolEngine engine;
  struct DisplayTrade {
    SymbolId symbol{};
    Trade trade{};
  };
  std::vector<DisplayTrade> trades;
  std::size_t events = 0;
  std::size_t rejected = 0;
  std::size_t line_number = 0;

  std::string line;
  while (std::getline(*input, line)) {
    ++line_number;
    const std::string cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }

    try {
      const auto fields = split(cleaned);
      if (fields.size() < 4) {
        throw std::runtime_error(
            "expected at least timestamp, symbol, event, id");
      }
      const std::string symbol = fields[1];
      const std::string event = fields[2];
      const auto id = static_cast<OrderId>(number(fields, 3, "id"));
      const SymbolId symbol_id = engine.add_symbol(symbol);

      if (event == "cancel") {
        engine.submit(EngineCommand::cancel(symbol_id, id));
      } else if (event == "modify") {
        const auto price = number(fields, 4, "price");
        const auto quantity = number(fields, 5, "quantity");
        engine.submit(EngineCommand::modify(symbol_id, id, price, quantity));
      } else {
        const auto side = parse_side(fields.size() > 4 ? fields[4] : "");
        const auto price = number(fields, 5, "price");
        const auto quantity = number(fields, 6, "quantity");
        const auto account = fields.size() > 7 && !fields[7].empty()
                                 ? static_cast<AccountId>(std::stoul(fields[7]))
                                 : AccountId{0};
        engine.submit(EngineCommand::submit(
            symbol_id,
            Order{id, account, side, parse_type(event), price, quantity}));
      }

      ++events;
      engine.process();
      EngineEvent output;
      while (engine.poll(output)) {
        if (output.type == EngineEventType::Trade) {
          trades.push_back(DisplayTrade{output.symbol_id, output.trade});
        } else if (output.type == EngineEventType::Rejected ||
                   output.type == EngineEventType::CancelRejected ||
                   output.type == EngineEventType::ModifyRejected) {
          ++rejected;
          std::printf("line %zu: %s %llu rejected (%s)\n", line_number,
                      event.c_str(), static_cast<unsigned long long>(id),
                      to_string(output.reason));
        }
      }
    } catch (const std::exception& error) {
      std::fprintf(stderr, "line %zu: %s\n", line_number, error.what());
      return 1;
    }
  }

  std::printf("cppquant-orderbook multi-symbol replay\n");
  std::printf("events: %zu, rejected: %zu, symbols: %zu\n", events, rejected,
              engine.symbol_count());

  if (!trades.empty()) {
    std::printf("\ntrades\n");
    std::printf("  %-6s  %6s  %8s  %8s  %8s  %8s\n", "symbol", "seq", "taker",
                "maker", "price", "qty");
    constexpr std::size_t kMaxPrinted = 50;
    const std::size_t printed = std::min(trades.size(), kMaxPrinted);
    for (std::size_t index = 0; index < printed; ++index) {
      const auto& entry = trades[index];
      const auto& trade = entry.trade;
      std::printf("  %-6s  %6llu  %8llu  %8llu  %8lld  %8lld\n",
                  engine.symbol_name(entry.symbol).c_str(),
                  static_cast<unsigned long long>(trade.sequence),
                  static_cast<unsigned long long>(trade.taker_id),
                  static_cast<unsigned long long>(trade.maker_id),
                  static_cast<long long>(trade.price),
                  static_cast<long long>(trade.quantity));
    }
    if (trades.size() > printed) {
      std::printf("  ... %zu more trades\n", trades.size() - printed);
    }
  }

  for (SymbolId symbol = 1; symbol <= engine.symbol_count(); ++symbol) {
    print_book(engine.symbol_name(symbol), *engine.book(symbol));
  }
  return 0;
}
