#include "cppquant/orderbook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

struct ScenarioResult {
  std::size_t orders{};
  double seconds{};
  std::size_t peak_orders{};
  std::uint64_t trades{};
  Quantity quantity{};
};

double elapsed_seconds(std::chrono::steady_clock::time_point start,
                       std::chrono::steady_clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}

ScenarioResult run_passive(std::size_t operations, std::uint64_t seed) {
  OrderBook book;
  std::mt19937_64 rng(seed);
  std::vector<OrderId> live;
  live.reserve(2'000);

  OrderId next_id = 1;
  std::size_t peak = 0;
  std::size_t cancelled = 0;

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    const Side side = (rng() & 1U) != 0 ? Side::Buy : Side::Sell;
    const Price price = side == Side::Buy
                            ? 9'950 - static_cast<Price>(rng() % 100)
                            : 10'050 + static_cast<Price>(rng() % 100);
    const Quantity quantity = static_cast<Quantity>(1 + rng() % 100);

    book.add_limit_order(next_id, side, price, quantity);
    live.push_back(next_id);
    ++next_id;

    if (live.size() > 1'000) {
      const auto victim = rng() % live.size();
      book.cancel(live[victim]);
      live[victim] = live.back();
      live.pop_back();
      ++cancelled;
    }

    peak = std::max(peak, book.resting_order_count());
  }
  const auto end = std::chrono::steady_clock::now();

  ScenarioResult result;
  result.orders = operations;
  result.seconds = elapsed_seconds(start, end);
  result.peak_orders = peak;
  result.trades = book.statistics().trades_executed;
  result.quantity = book.statistics().quantity_traded;
  (void)cancelled;
  return result;
}

ScenarioResult run_crossing(std::size_t pairs, std::uint64_t seed) {
  OrderBook book;
  std::mt19937_64 rng(seed);

  OrderId next_id = 1;
  std::size_t peak = 0;

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < pairs; ++index) {
    const Quantity buy_quantity = static_cast<Quantity>(1 + rng() % 100);
    const Quantity sell_quantity = static_cast<Quantity>(1 + rng() % 100);

    const Price buy_price = 10'005 + static_cast<Price>(rng() % 10);
    book.add_limit_order(next_id++, Side::Buy, buy_price, buy_quantity);

    const Price sell_price = 9'995 - static_cast<Price>(rng() % 10);
    book.add_limit_order(next_id++, Side::Sell, sell_price, sell_quantity);

    peak = std::max(peak, book.resting_order_count());
  }
  const auto end = std::chrono::steady_clock::now();

  ScenarioResult result;
  result.orders = pairs * 2;
  result.seconds = elapsed_seconds(start, end);
  result.peak_orders = peak;
  result.trades = book.statistics().trades_executed;
  result.quantity = book.statistics().quantity_traded;
  return result;
}

void report(const char* name, const ScenarioResult& result) {
  const double ns_per_order =
      result.seconds * 1e9 / static_cast<double>(result.orders);
  const double millions_per_second =
      static_cast<double>(result.orders) / result.seconds / 1e6;

  std::printf("%-20s %10zu orders  %8.1f ns/order  %7.2f M orders/s"
              "  peak book %zu",
              name, result.orders, ns_per_order, millions_per_second,
              result.peak_orders);

  if (result.trades != 0) {
    const double trades_per_order =
        static_cast<double>(result.trades) / static_cast<double>(result.orders);
    std::printf("  %llu trades (%.2f/order)",
                static_cast<unsigned long long>(result.trades),
                trades_per_order);
  }
  std::printf("\n");
}

} // namespace

int main(int argc, char** argv) {
  std::size_t passive_operations = 500'000;
  std::size_t crossing_pairs = 250'000;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--quick") {
      passive_operations = 20'000;
      crossing_pairs = 10'000;
    } else if (argument == "--help" || argument == "-h") {
      std::printf("usage: %s [--quick]\n", argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      return 2;
    }
  }

  std::printf("cppquant-orderbook benchmark\n");
  std::printf("seed: 42 (deterministic workload)\n\n");

  const auto passive = run_passive(passive_operations, 42);
  report("passive add/cancel", passive);

  const auto crossing = run_crossing(crossing_pairs, 42);
  report("crossing flow", crossing);

  const auto checksum = passive.quantity + crossing.quantity;
  std::printf("\nchecksum (quantity traded): %lld\n",
              static_cast<long long>(checksum));
  return 0;
}