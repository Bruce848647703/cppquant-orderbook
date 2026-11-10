#include "cppquant/engine.hpp"
#include "cppquant/journal.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace cppquant;

namespace {

std::filesystem::path temporary_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

Order limit_order(OrderId id, Side side, Price price, Quantity quantity) {
  return Order{id, 0, side, OrderType::Limit, price, quantity};
}

std::vector<EngineEvent> drain(MatchingEngine& engine) {
  std::vector<EngineEvent> events;
  EngineEvent event;
  while (engine.poll(event)) {
    events.push_back(event);
  }
  return events;
}

} // namespace

TEST(journal_records_and_replays_a_session) {
  CommandJournal journal;
  MatchingEngine engine(SelfTradePrevention::Allow, 64, RiskLimits{}, &journal);

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Sell, 100, 5))));
  CHECK(
      engine.submit(EngineCommand::submit(limit_order(2, Side::Buy, 100, 3))));
  CHECK(engine.submit(EngineCommand::submit(limit_order(3, Side::Buy, 99, 2))));
  CHECK(engine.submit(EngineCommand::modify(3, 98, 1)));
  CHECK(engine.submit(EngineCommand::cancel(3)));
  engine.process();
  drain(engine);

  CHECK_EQ(journal.records().size(), 5u);
  for (std::size_t index = 0; index < journal.records().size(); ++index) {
    CHECK_EQ(journal.records()[index].sequence, index + 1);
  }

  const auto path = temporary_path("cppquant_journal.bin");
  write_journal_file(journal.records(), path.string());
  const auto read = read_journal_file(path.string());
  std::remove(path.string().c_str());

  CHECK(!read.truncated);
  CHECK_EQ(read.records.size(), journal.records().size());
  CHECK_EQ(read.records[2].type, JournalRecordType::Submit);
  CHECK_EQ(read.records[3].type, JournalRecordType::Modify);
  CHECK_EQ(read.records[4].type, JournalRecordType::Cancel);

  OrderBook restored(SelfTradePrevention::Allow, 64);
  replay_journal(read.records, restored);

  CHECK(restored.resting_orders() == engine.book().resting_orders());
  CHECK_EQ(book_checksum(restored), book_checksum(engine.book()));
  CHECK(restored.validate());
}

TEST(journal_skips_rejected_commands) {
  CommandJournal journal;
  RiskLimits limits;
  limits.max_order_quantity = 5;
  MatchingEngine engine(SelfTradePrevention::Allow, 64, limits, &journal);

  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Buy, 100, 6))));
  CHECK(
      engine.submit(EngineCommand::submit(limit_order(2, Side::Buy, 100, 1))));
  CHECK(
      engine.submit(EngineCommand::submit(limit_order(2, Side::Sell, 101, 1))));
  CHECK(engine.submit(EngineCommand::cancel(99)));
  engine.process();
  drain(engine);

  CHECK_EQ(journal.records().size(), 1u);
  CHECK_EQ(journal.records()[0].sequence, 1u);
  CHECK_EQ(journal.records()[0].order.id, 2u);
}

TEST(journal_truncated_prefix_replays) {
  CommandJournal journal;
  MatchingEngine engine(SelfTradePrevention::Allow, 64, RiskLimits{}, &journal);
  CHECK(
      engine.submit(EngineCommand::submit(limit_order(1, Side::Sell, 100, 5))));
  CHECK(
      engine.submit(EngineCommand::submit(limit_order(2, Side::Buy, 100, 3))));
  CHECK(engine.submit(EngineCommand::submit(limit_order(3, Side::Buy, 99, 2))));
  CHECK(engine.submit(EngineCommand::modify(3, 98, 1)));
  CHECK(engine.submit(EngineCommand::cancel(3)));
  engine.process();
  drain(engine);

  const auto path = temporary_path("cppquant_journal_truncated.bin");
  write_journal_file(journal.records(), path.string());
  const auto original_size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, original_size - 5);

  const auto read = read_journal_file(path.string());
  std::remove(path.string().c_str());

  CHECK(read.truncated);
  CHECK_EQ(read.records.size(), 4u);

  OrderBook restored(SelfTradePrevention::Allow, 64);
  replay_journal(read.records, restored);
  CHECK(restored.validate());
  CHECK_EQ(restored.quantity_of(1), std::optional<Quantity>{2});
  CHECK_EQ(restored.best_bid(), Price{98});
}

TEST(journal_empty_file_round_trips) {
  const auto path = temporary_path("cppquant_journal_empty.bin");
  write_journal_file({}, path.string());
  const auto read = read_journal_file(path.string());
  std::remove(path.string().c_str());

  CHECK(!read.truncated);
  CHECK(read.records.empty());

  OrderBook book;
  replay_journal(read.records, book);
  CHECK(book.empty());
}

TEST(journal_rejects_bad_magic_and_sequences) {
  const auto path = temporary_path("cppquant_journal_bad.bin");
  {
    std::ofstream file(path, std::ios::binary);
    file << "NOT A JOURNAL";
  }
  CHECK_THROW(read_journal_file(path.string()), std::runtime_error);
  std::remove(path.string().c_str());

  JournalRecord first;
  first.sequence = 2;
  first.type = JournalRecordType::Submit;
  first.order = limit_order(1, Side::Buy, 100, 1);
  JournalRecord second = first;
  second.sequence = 1;
  second.order.id = 2;

  OrderBook book;
  CHECK_THROW(replay_journal({first, second}, book), std::runtime_error);

  JournalRecord cancel;
  cancel.sequence = 1;
  cancel.type = JournalRecordType::Cancel;
  cancel.order_id = 99;
  CHECK_THROW(replay_journal({cancel}, book), std::runtime_error);
}

TEST(snapshot_file_round_trips) {
  OrderBook book(SelfTradePrevention::Allow, 64);
  book.add_limit_order(1, Side::Buy, 100, 5);
  book.add_limit_order(2, Side::Sell, 101, 7);

  const auto snapshot = make_snapshot(book, 3, 42);
  const auto path = temporary_path("cppquant_snapshot.bin");
  write_snapshot_file(snapshot, path.string());
  const auto loaded = read_snapshot_file(path.string());
  std::remove(path.string().c_str());

  OrderBook restored(SelfTradePrevention::Allow, 64);
  restore_snapshot(restored, loaded);

  CHECK(restored.resting_orders() == book.resting_orders());
  CHECK_EQ(book_checksum(restored), book_checksum(book));
  CHECK_EQ(loaded.symbol_id, 3u);
  CHECK_EQ(loaded.sequence, 42u);
}

TEST(truncated_snapshot_is_rejected) {
  OrderBook book;
  book.add_limit_order(1, Side::Buy, 100, 1);
  const auto path = temporary_path("cppquant_snapshot_truncated.bin");
  write_snapshot_file(make_snapshot(book, 1, 1), path.string());
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 1);

  CHECK_THROW(read_snapshot_file(path.string()), std::runtime_error);
  std::remove(path.string().c_str());
}

TEST(checksum_changes_with_book_state) {
  OrderBook book;
  const auto empty = book_checksum(book);
  book.add_limit_order(1, Side::Buy, 100, 1);
  const auto one = book_checksum(book);
  book.add_limit_order(2, Side::Sell, 101, 1);
  const auto two = book_checksum(book);
  book.cancel(1);
  const auto cancelled = book_checksum(book);

  CHECK(empty != one);
  CHECK(one != two);
  CHECK(two != cancelled);
}

TEST(command_journal_sequences_reset_on_clear) {
  CommandJournal journal;
  journal.append_submit(limit_order(1, Side::Buy, 100, 1));
  journal.append_cancel(1);
  CHECK_EQ(journal.records().size(), 2u);
  CHECK_EQ(journal.records()[0].sequence, 1u);
  CHECK_EQ(journal.records()[1].sequence, 2u);

  journal.clear();
  journal.append_submit(limit_order(2, Side::Buy, 99, 1));
  CHECK_EQ(journal.records().size(), 1u);
  CHECK_EQ(journal.records()[0].sequence, 1u);
}
