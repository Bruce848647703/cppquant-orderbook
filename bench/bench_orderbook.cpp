#include "cppquant/engine.hpp"
#include "cppquant/journal.hpp"
#include "cppquant/market_data.hpp"
#include "cppquant/orderbook.hpp"
#include "cppquant/spsc_queue.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace cppquant;

namespace {

double sink = 0.0;

struct ScenarioResult {
  std::size_t orders{};
  double seconds{};
  std::size_t peak_orders{};
  std::uint64_t trades{};
  Quantity quantity{};
  std::size_t pool_chunks{};
  std::size_t heap_allocations{};
  std::size_t blocks_in_use{};
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
  const auto pool = book.pool_stats();
  result.pool_chunks = pool.chunks;
  result.heap_allocations = pool.heap_allocations;
  result.blocks_in_use = pool.blocks_in_use;
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
  const auto pool = book.pool_stats();
  result.pool_chunks = pool.chunks;
  result.heap_allocations = pool.heap_allocations;
  result.blocks_in_use = pool.blocks_in_use;
  return result;
}

bool run_spsc_queue(std::size_t messages) {
  SpscQueue<std::uint64_t, 1024> queue;
  std::atomic<bool> start{false};
  std::uint64_t consumed_sum = 0;

  const auto start_time = std::chrono::steady_clock::now();
  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t value = 1; value <= messages; ++value) {
      while (!queue.try_push(value)) {
      }
    }
  });

  std::thread consumer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    std::uint64_t value = 0;
    std::size_t count = 0;
    while (count < messages) {
      if (queue.try_pop(value)) {
        consumed_sum += value;
        ++count;
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();
  const auto end_time = std::chrono::steady_clock::now();
  const double seconds = elapsed_seconds(start_time, end_time);

  const std::uint64_t expected = static_cast<std::uint64_t>(messages) *
                                 (static_cast<std::uint64_t>(messages) + 1) / 2;
  const bool ok = consumed_sum == expected && queue.empty();

  std::printf(
      "SPSC queue (2 threads) %10zu msgs  %8.1f ns/msg  %6.2f M msgs/s  %s\n",
      messages, seconds * 1e9 / static_cast<double>(messages),
      static_cast<double>(messages) / seconds / 1e6,
      ok ? "ok" : "checksum-mismatch");
  return ok;
}

void run_engine(bool with_risk, std::size_t commands) {
  RiskLimits limits;
  if (with_risk) {
    limits.max_order_quantity = 100;
    limits.max_order_notional = 10'000.0;
    limits.max_position = 1'000;
    limits.price_collar_bps = 500.0;
    limits.max_open_orders = 100;
  }

  MatchingEngine engine(SelfTradePrevention::Allow,
                        PoolResource::kDefaultCapacity, limits);
  engine.risk().set_reference_price(100);

  constexpr std::size_t kBatch = 1'000;
  EngineEvent event;
  OrderId id = 1;
  std::size_t processed = 0;

  const auto start = std::chrono::steady_clock::now();
  while (processed < commands) {
    const std::size_t batch = std::min(kBatch, commands - processed);
    for (std::size_t index = 0; index < batch; ++index) {
      engine.submit(EngineCommand::submit(
          Order{id++, 1, Side::Buy, OrderType::Market, 0, 10}));
    }
    engine.process();
    while (engine.poll(event)) {
    }
    processed += batch;
  }
  const auto end = std::chrono::steady_clock::now();
  const double seconds = elapsed_seconds(start, end);

  std::printf("engine %s %10zu cmds  %8.1f ns/cmd  %6.2f M cmds/s\n",
              with_risk ? "with risk   " : "without risk", commands,
              seconds * 1e9 / static_cast<double>(commands),
              static_cast<double>(commands) / seconds / 1e6);
}

void run_market_data(std::size_t operations) {
  OrderBook book(SelfTradePrevention::Allow, 256);
  for (int index = 0; index < 100; ++index) {
    book.add_limit_order(static_cast<OrderId>(index + 1), Side::Buy,
                         1'000 - index, 1);
    book.add_limit_order(static_cast<OrderId>(index + 101), Side::Sell,
                         2'000 + index, 1);
  }

  const auto snapshot = make_snapshot(book, 1, 1);
  const auto delta =
      encode_delta(BookDelta{MarketDataMessageType::AddOrder,
                             1,
                             2,
                             RestingOrderView{999, 0, Side::Buy, 900, 1},
                             {},
                             {},
                             {}});

  auto start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    const auto message = encode_snapshot(snapshot);
    const auto decoded = decode_snapshot(message);
    sink += static_cast<double>(decoded.orders.size());
  }
  double seconds = elapsed_seconds(start, std::chrono::steady_clock::now());
  std::printf("snapshot encode+decode  %10zu ops  %8.1f ns/op  %6.2f M ops/s\n",
              operations, seconds * 1e9 / static_cast<double>(operations),
              static_cast<double>(operations) / seconds / 1e6);

  start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < operations; ++index) {
    const auto decoded = decode_delta(delta);
    sink += static_cast<double>(decoded.order.quantity);
  }
  seconds = elapsed_seconds(start, std::chrono::steady_clock::now());
  std::printf("delta decode            %10zu ops  %8.1f ns/op  %6.2f M ops/s\n",
              operations, seconds * 1e9 / static_cast<double>(operations),
              static_cast<double>(operations) / seconds / 1e6);
}

void run_journal(std::size_t records) {
  std::vector<JournalRecord> journal;
  journal.reserve(records);
  for (std::size_t index = 0; index < records; ++index) {
    JournalRecord record;
    record.sequence = index + 1;
    record.type = JournalRecordType::Submit;
    record.order =
        Order{static_cast<OrderId>(index + 1),   0, Side::Buy, OrderType::Limit,
              1'000 + static_cast<Price>(index), 1};
    journal.push_back(record);
  }

  const char* path = "/tmp/cppquant_orderbook_journal.bin";
  const auto start = std::chrono::steady_clock::now();
  write_journal_file(journal, path);
  const auto read = read_journal_file(path);
  OrderBook book(SelfTradePrevention::Allow, records + 1);
  replay_journal(read.records, book);
  const auto end = std::chrono::steady_clock::now();
  std::remove(path);

  const double seconds = elapsed_seconds(start, end);
  const bool ok = !read.truncated && book.resting_order_count() == records;
  std::printf("journal write+read+replay %8zu records  %8.1f ns/record  %6.2f "
              "M records/s  %s\n",
              records, seconds * 1e9 / static_cast<double>(records),
              static_cast<double>(records) / seconds / 1e6,
              ok ? "ok" : "checksum-mismatch");
  sink += static_cast<double>(book.resting_order_count());
}

void report(const char* name, const ScenarioResult& result) {
  const double ns_per_order =
      result.seconds * 1e9 / static_cast<double>(result.orders);
  const double millions_per_second =
      static_cast<double>(result.orders) / result.seconds / 1e6;

  std::printf("%-20s %10zu orders  %8.1f ns/order  %7.2f M orders/s"
              "  peak book %zu  pool %zu chunks / %zu heap allocs",
              name, result.orders, ns_per_order, millions_per_second,
              result.peak_orders, result.pool_chunks, result.heap_allocations);

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
  std::size_t queue_messages = 10'000'000;
  std::size_t engine_commands = 1'000'000;
  std::size_t market_data_operations = 100'000;
  std::size_t journal_records = 100'000;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--quick") {
      passive_operations = 20'000;
      crossing_pairs = 10'000;
      queue_messages = 1'000'000;
      engine_commands = 100'000;
      market_data_operations = 10'000;
      journal_records = 10'000;
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

  const bool queue_ok = run_spsc_queue(queue_messages);
  run_engine(false, engine_commands);
  run_engine(true, engine_commands);
  run_market_data(market_data_operations);
  run_journal(journal_records);

  const auto checksum = passive.quantity + crossing.quantity;
  std::printf("\nchecksum (quantity traded): %lld\n",
              static_cast<long long>(checksum));
  std::printf("checksum (market data): %.0f\n", sink);
  return queue_ok ? 0 : 1;
}
