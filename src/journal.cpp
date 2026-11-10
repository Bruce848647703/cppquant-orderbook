#include "cppquant/journal.hpp"

#include <bit>
#include <cstddef>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cppquant {
namespace {

constexpr std::uint32_t kMagic = 0x314A5143; // "CQJ1" little-endian
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kFileHeaderSize = 8;
constexpr std::size_t kRecordHeaderSize = 16;

class Writer {
public:
  explicit Writer(std::vector<std::byte>& output) : output_(output) {}
  void u8(std::uint8_t value) {
    output_.push_back(static_cast<std::byte>(value));
  }
  void u16(std::uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }
  void u32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }
  void u64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
  }
  void i64(std::int64_t value) {
    u64(std::bit_cast<std::uint64_t>(value));
  }

private:
  std::vector<std::byte>& output_;
};

class Reader {
public:
  explicit Reader(std::span<const std::byte> input) : input_(input) {}

  std::uint8_t u8() {
    require(1);
    return std::to_integer<std::uint8_t>(input_[offset_++]);
  }
  std::uint16_t u16() {
    std::uint16_t value = 0;
    for (int shift = 0; shift < 16; shift += 8) {
      value |= static_cast<std::uint16_t>(u8()) << shift;
    }
    return value;
  }
  std::uint32_t u32() {
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(u8()) << shift;
    }
    return value;
  }
  std::uint64_t u64() {
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      value |= static_cast<std::uint64_t>(u8()) << shift;
    }
    return value;
  }
  std::int64_t i64() {
    return std::bit_cast<std::int64_t>(u64());
  }
  std::size_t remaining() const noexcept {
    return input_.size() - offset_;
  }

private:
  void require(std::size_t count) const {
    if (count > input_.size() - offset_) {
      throw std::runtime_error("truncated journal");
    }
  }

  std::span<const std::byte> input_;
  std::size_t offset_{};
};

std::size_t payload_size(JournalRecordType type) {
  switch (type) {
  case JournalRecordType::Submit:
    return 8 + 4 + 1 + 1 + 8 + 8;
  case JournalRecordType::Cancel:
    return 8;
  case JournalRecordType::Modify:
    return 8 + 8 + 8;
  }
  throw std::runtime_error("unknown journal record type");
}

std::vector<std::byte> read_file(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  const auto size = file.tellg();
  if (size < 0) {
    throw std::runtime_error("cannot size " + path);
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(size));
  file.seekg(0);
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      throw std::runtime_error("cannot read " + path);
    }
  }
  return bytes;
}

void write_file(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    throw std::runtime_error("cannot open " + path);
  }
  if (!bytes.empty()) {
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  if (!file) {
    throw std::runtime_error("cannot write " + path);
  }
}

void write_order(Writer& writer, const Order& order) {
  writer.u64(order.id);
  writer.u32(order.account);
  writer.u8(order.side == Side::Buy ? 0 : 1);
  writer.u8(static_cast<std::uint8_t>(order.type));
  writer.i64(order.price);
  writer.i64(order.quantity);
}

Order read_order(Reader& reader) {
  Order order;
  order.id = reader.u64();
  order.account = reader.u32();
  const std::uint8_t side = reader.u8();
  const std::uint8_t type = reader.u8();
  if (side > 1 || type > 3) {
    throw std::runtime_error("invalid order in journal");
  }
  order.side = side == 0 ? Side::Buy : Side::Sell;
  order.type = static_cast<OrderType>(type);
  order.price = reader.i64();
  order.quantity = reader.i64();
  return order;
}

} // namespace

void CommandJournal::append_submit(const Order& order) {
  JournalRecord record;
  record.type = JournalRecordType::Submit;
  record.order = order;
  append(std::move(record));
}

void CommandJournal::append_cancel(OrderId id) {
  JournalRecord record;
  record.type = JournalRecordType::Cancel;
  record.order_id = id;
  append(std::move(record));
}

void CommandJournal::append_modify(OrderId id, Price price, Quantity quantity) {
  JournalRecord record;
  record.type = JournalRecordType::Modify;
  record.order_id = id;
  record.new_price = price;
  record.new_quantity = quantity;
  append(std::move(record));
}

std::uint64_t CommandJournal::append(JournalRecord record) {
  record.sequence = next_sequence_++;
  const std::uint64_t sequence = record.sequence;
  records_.push_back(std::move(record));
  return sequence;
}

void CommandJournal::clear() noexcept {
  records_.clear();
  next_sequence_ = 1;
}

void write_journal_file(const std::vector<JournalRecord>& records,
                        const std::string& path) {
  std::vector<std::byte> bytes;
  Writer writer(bytes);
  writer.u32(kMagic);
  writer.u16(kVersion);
  writer.u16(0);

  for (const auto& record : records) {
    const std::size_t size = payload_size(record.type);
    writer.u8(static_cast<std::uint8_t>(record.type));
    writer.u8(0);
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(size));
    writer.u64(record.sequence);

    switch (record.type) {
    case JournalRecordType::Submit:
      write_order(writer, record.order);
      break;
    case JournalRecordType::Cancel:
      writer.u64(record.order_id);
      break;
    case JournalRecordType::Modify:
      writer.u64(record.order_id);
      writer.i64(record.new_price);
      writer.i64(record.new_quantity);
      break;
    }
  }

  write_file(path, bytes);
}

JournalReadResult read_journal_file(const std::string& path) {
  const auto bytes = read_file(path);
  if (bytes.size() < kFileHeaderSize) {
    throw std::runtime_error("journal file is shorter than the header");
  }

  Reader reader(bytes);
  if (reader.u32() != kMagic) {
    throw std::runtime_error("bad journal magic");
  }
  if (reader.u16() != kVersion) {
    throw std::runtime_error("unsupported journal version");
  }
  if (reader.u16() != 0) {
    throw std::runtime_error("non-zero journal reserved field");
  }

  JournalReadResult result;
  while (reader.remaining() > 0) {
    if (reader.remaining() < kRecordHeaderSize) {
      result.truncated = true;
      break;
    }

    const auto type = static_cast<JournalRecordType>(reader.u8());
    const std::uint8_t reserved = reader.u8();
    const std::uint16_t reserved16 = reader.u16();
    const std::uint32_t size = reader.u32();
    const std::uint64_t sequence = reader.u64();

    if (reserved != 0 || reserved16 != 0) {
      throw std::runtime_error("invalid journal record header");
    }
    const std::size_t expected = payload_size(type);
    if (size != expected) {
      throw std::runtime_error("journal payload size mismatch");
    }
    if (reader.remaining() < size) {
      result.truncated = true;
      break;
    }

    JournalRecord record;
    record.sequence = sequence;
    record.type = type;
    switch (type) {
    case JournalRecordType::Submit:
      record.order = read_order(reader);
      break;
    case JournalRecordType::Cancel:
      record.order_id = reader.u64();
      break;
    case JournalRecordType::Modify:
      record.order_id = reader.u64();
      record.new_price = reader.i64();
      record.new_quantity = reader.i64();
      break;
    }
    result.records.push_back(std::move(record));
  }

  return result;
}

void replay_journal(const std::vector<JournalRecord>& records,
                    OrderBook& book) {
  std::uint64_t previous_sequence = 0;
  for (const auto& record : records) {
    if (record.sequence <= previous_sequence) {
      throw std::runtime_error("journal sequences are not increasing");
    }
    previous_sequence = record.sequence;

    switch (record.type) {
    case JournalRecordType::Submit: {
      const SubmitResult result = book.submit(record.order);
      if (!result.accepted) {
        throw std::runtime_error("journal submit was rejected during replay");
      }
      break;
    }
    case JournalRecordType::Cancel:
      if (!book.cancel(record.order_id)) {
        throw std::runtime_error("journal cancel failed during replay");
      }
      break;
    case JournalRecordType::Modify:
      if (!book.modify(record.order_id, record.new_price,
                       record.new_quantity)) {
        throw std::runtime_error("journal modify failed during replay");
      }
      break;
    }
  }
}

std::uint64_t book_checksum(const OrderBook& book) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  auto mix = [&](std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      hash ^= (value >> shift) & 0xFFU;
      hash *= 1099511628211ULL;
    }
  };

  for (const auto& order : book.resting_orders()) {
    mix(order.id);
    mix(order.account);
    mix(order.side == Side::Buy ? 0 : 1);
    mix(static_cast<std::uint64_t>(order.price));
    mix(static_cast<std::uint64_t>(order.quantity));
  }
  return hash;
}

void write_snapshot_file(const MarketDataSnapshot& snapshot,
                         const std::string& path) {
  write_file(path, encode_snapshot(snapshot));
}

MarketDataSnapshot read_snapshot_file(const std::string& path) {
  const auto bytes = read_file(path);
  return decode_snapshot(bytes);
}

} // namespace cppquant
