#include "cppquant/orderbook.hpp"

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

long long parse_number(const std::vector<std::string>& fields,
                       std::size_t index, const char* what) {
  if (index >= fields.size() || fields[index].empty()) {
    throw std::runtime_error(std::string("missing ") + what);
  }
  return std::stoll(fields[index]);
}

void print_book(const OrderBook& book) {
  const auto snapshot = book.snapshot(5);

  auto print_side = [](const char* label,
                       const std::vector<LevelView>& levels) {
    std::printf("  %s\n", label);
    if (levels.empty()) {
      std::printf("    (empty)\n");
      return;
    }
    for (const auto& level : levels) {
      std::printf("    %10lld  %8lld  (%zu orders)\n",
                  static_cast<long long>(level.price),
                  static_cast<long long>(level.quantity), level.orders);
    }
  };

  print_side("bids", snapshot.bids);
  print_side("asks", snapshot.asks);

  if (snapshot.best_bid && snapshot.best_ask) {
    std::printf(
        "  best bid %lld / best ask %lld / spread %lld\n",
        static_cast<long long>(*snapshot.best_bid),
        static_cast<long long>(*snapshot.best_ask),
        static_cast<long long>(*snapshot.best_ask - *snapshot.best_bid));
  } else if (snapshot.best_bid) {
    std::printf("  best bid %lld / best ask none\n",
                static_cast<long long>(*snapshot.best_bid));
  } else if (snapshot.best_ask) {
    std::printf("  best bid none / best ask %lld\n",
                static_cast<long long>(*snapshot.best_ask));
  } else {
    std::printf("  book is empty\n");
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

  OrderBook book;
  std::vector<Trade> all_trades;
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
      if (fields.size() < 3) {
        throw std::runtime_error("expected at least event and id");
      }

      const std::string& event = fields[1];
      const auto id = static_cast<OrderId>(parse_number(fields, 2, "id"));

      if (event == "cancel") {
        if (!book.cancel(id)) {
          std::printf("line %zu: cancel %llu ignored (unknown order)\n",
                      line_number, static_cast<unsigned long long>(id));
        }
        ++events;
        continue;
      }

      if (event == "modify") {
        const auto price = parse_number(fields, 3, "price");
        const auto quantity = parse_number(fields, 4, "quantity");
        if (!book.modify(id, price, quantity)) {
          std::printf("line %zu: modify %llu rejected\n", line_number,
                      static_cast<unsigned long long>(id));
          ++rejected;
        }
        ++events;
        continue;
      }

      const auto type = parse_type(event);
      const auto side = parse_side(fields.size() > 3 ? fields[3] : "");
      const auto price = parse_number(fields, 4, "price");
      const auto quantity = parse_number(fields, 5, "quantity");
      const auto account = fields.size() > 6 && !fields[6].empty()
                               ? static_cast<AccountId>(std::stoul(fields[6]))
                               : AccountId{0};

      auto result =
          book.submit(Order{id, account, side, type, price, quantity});
      ++events;
      if (!result.accepted) {
        ++rejected;
        std::printf("line %zu: %s %llu rejected (%s)\n", line_number,
                    event.c_str(), static_cast<unsigned long long>(id),
                    to_string(result.reason));
      }
      all_trades.insert(all_trades.end(), result.trades.begin(),
                        result.trades.end());
    } catch (const std::exception& error) {
      std::fprintf(stderr, "line %zu: %s\n", line_number, error.what());
      return 1;
    }
  }

  const auto& stats = book.statistics();

  std::printf("cppquant-orderbook replay\n");
  std::printf("events: %zu, rejected: %zu\n", events, rejected);
  std::printf("trades: %llu, quantity: %lld, notional: %.2Lf, vwap: %.4f\n",
              static_cast<unsigned long long>(stats.trades_executed),
              static_cast<long long>(stats.quantity_traded),
              stats.notional_traded,
              stats.quantity_traded == 0
                  ? 0.0
                  : static_cast<double>(
                        stats.notional_traded /
                        static_cast<long double>(stats.quantity_traded)));

  if (!all_trades.empty()) {
    constexpr std::size_t kMaxPrinted = 50;
    const std::size_t printed = std::min(all_trades.size(), kMaxPrinted);
    std::printf("\ntrades\n");
    std::printf("  %6s  %8s  %8s  %8s  %8s  %s\n", "seq", "taker", "maker",
                "price", "qty", "side");
    for (std::size_t index = 0; index < printed; ++index) {
      const auto& trade = all_trades[index];
      std::printf("  %6llu  %8llu  %8llu  %8lld  %8lld  %c\n",
                  static_cast<unsigned long long>(trade.sequence),
                  static_cast<unsigned long long>(trade.taker_id),
                  static_cast<unsigned long long>(trade.maker_id),
                  static_cast<long long>(trade.price),
                  static_cast<long long>(trade.quantity),
                  trade.taker_side == Side::Buy ? 'B' : 'S');
    }
    if (all_trades.size() > printed) {
      std::printf("  ... %zu more trades\n", all_trades.size() - printed);
    }
  }

  std::printf("\nfinal book\n");
  print_book(book);

  std::printf("\nstatistics\n");
  std::printf("  submitted %llu, rested %llu, cancelled %llu, modified %llu\n",
              static_cast<unsigned long long>(stats.orders_submitted),
              static_cast<unsigned long long>(stats.orders_rested),
              static_cast<unsigned long long>(stats.orders_cancelled),
              static_cast<unsigned long long>(stats.orders_modified));
  std::printf("  rejected %llu, trades %llu, resting orders %zu\n",
              static_cast<unsigned long long>(stats.orders_rejected),
              static_cast<unsigned long long>(stats.trades_executed),
              book.resting_order_count());

  return 0;
}