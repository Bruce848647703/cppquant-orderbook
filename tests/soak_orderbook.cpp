#include "cppquant/orderbook.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

struct Options {
  std::size_t events{10'000'000};
  std::uint64_t seed{42};
};

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--events" && index + 1 < argc) {
      options.events = static_cast<std::size_t>(std::stoull(argv[++index]));
    } else if (argument == "--seed" && index + 1 < argc) {
      options.seed = std::stoull(argv[++index]);
    } else if (argument == "--help" || argument == "-h") {
      std::printf("usage: %s [--events N] [--seed N]\n", argv[0]);
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", argument.c_str());
      std::exit(2);
    }
  }
  if (options.events == 0) {
    std::fprintf(stderr, "event count must be positive\n");
    std::exit(2);
  }
  return options;
}

bool check_invariants(const OrderBook& book, std::size_t event) {
  if (!book.validate()) {
    std::fprintf(stderr, "validate() failed at event %zu\n", event);
    return false;
  }
  const auto bid = book.best_bid();
  const auto ask = book.best_ask();
  if (bid && ask && *bid >= *ask) {
    std::fprintf(stderr, "crossed book at event %zu: %lld >= %lld\n", event,
                 static_cast<long long>(*bid), static_cast<long long>(*ask));
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  OrderBook book(SelfTradePrevention::Allow, 200'000);
  std::vector<OrderId> live;
  live.reserve(100'000);

  std::mt19937_64 rng(options.seed);
  std::uniform_int_distribution<int> operation(0, 99);
  std::uniform_int_distribution<int> side(0, 1);
  std::uniform_int_distribution<Price> price_offset(-500, 500);
  std::uniform_int_distribution<Quantity> quantity(1, 100);

  OrderId next_id = 1;
  std::size_t submitted = 0;
  std::size_t rejected = 0;
  std::size_t cancelled = 0;
  std::size_t modified = 0;
  std::size_t modify_failed = 0;
  std::size_t max_live = 0;

  for (std::size_t event = 0; event < options.events; ++event) {
    const int choice = operation(rng);
    if (choice < 45 || live.size() < 1'000) {
      const Side order_side = side(rng) == 0 ? Side::Buy : Side::Sell;
      const Price price = 10'000 + price_offset(rng);
      const SubmitResult result =
          book.add_limit_order(next_id++, order_side, price, quantity(rng),
                               static_cast<AccountId>(event % 17));
      ++submitted;
      if (!result.accepted) {
        ++rejected;
      } else if (result.rested) {
        live.push_back(next_id - 1);
        max_live = std::max(max_live, live.size());
      }
    } else if (choice < 85 || live.size() > 50'000) {
      const std::size_t index = static_cast<std::size_t>(rng() % live.size());
      if (book.cancel(live[index])) {
        ++cancelled;
      }
      live[index] = live.back();
      live.pop_back();
    } else {
      const std::size_t index = static_cast<std::size_t>(rng() % live.size());
      const Price price = 10'000 + price_offset(rng);
      if (book.modify(live[index], price, quantity(rng))) {
        ++modified;
      } else {
        ++modify_failed;
        live[index] = live.back();
        live.pop_back();
      }
    }

    if ((event + 1) % 100'000 == 0) {
      if (!check_invariants(book, event + 1)) {
        return 1;
      }
      live.clear();
      for (const auto& order : book.resting_orders()) {
        live.push_back(order.id);
      }
    }
    if ((event + 1) % 1'000'000 == 0) {
      std::printf("events %zu  live %zu  resting %zu\n", event + 1, live.size(),
                  book.resting_order_count());
    }
  }

  if (!check_invariants(book, options.events)) {
    return 1;
  }

  const auto pool = book.pool_stats();
  const auto& stats = book.statistics();
  std::printf("soak ok: events=%zu seed=%llu submitted=%zu rejected=%zu "
              "cancelled=%zu modified=%zu modify_failed=%zu\n",
              options.events, static_cast<unsigned long long>(options.seed),
              submitted, rejected, cancelled, modified, modify_failed);
  std::printf(
      "book: live=%zu resting=%zu trades=%llu max_live=%zu pool_blocks=%zu "
      "heap_allocations=%zu\n",
      live.size(), book.resting_order_count(),
      static_cast<unsigned long long>(stats.trades_executed), max_live,
      pool.blocks_in_use, pool.heap_allocations);
  return 0;
}