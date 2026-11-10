#include "cppquant/market_data.hpp"

#include <bit>
#include <stdexcept>
#include <string>

namespace cppquant {
namespace {

constexpr std::uint32_t kMagic = 0x444D5143; // "CQMD" in little-endian
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kOrderRecordSize = 8 + 4 + 1 + 8 + 8;

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
      throw std::runtime_error("truncated market data message");
    }
  }

  std::span<const std::byte> input_;
  std::size_t offset_{};
};

struct Header {
  MarketDataMessageType type{};
  std::uint32_t symbol_id{};
  std::uint64_t sequence{};
  std::span<const std::byte> payload;
};

Header read_header(std::span<const std::byte> message) {
  if (message.size() < kHeaderSize) {
    throw std::runtime_error("market data message is shorter than the header");
  }

  Reader reader(message);
  const std::uint32_t magic = reader.u32();
  const std::uint16_t version = reader.u16();
  const std::uint8_t type = reader.u8();
  const std::uint8_t reserved = reader.u8();
  const std::uint32_t symbol_id = reader.u32();
  const std::uint64_t sequence = reader.u64();
  const std::uint32_t payload_size = reader.u32();

  if (magic != kMagic) {
    throw std::runtime_error("bad market data magic");
  }
  if (version != kVersion) {
    throw std::runtime_error("unsupported market data version");
  }
  if (reserved != 0) {
    throw std::runtime_error("non-zero reserved header byte");
  }
  if (type < 1 || type > 5) {
    throw std::runtime_error("unknown market data message type");
  }
  if (payload_size != message.size() - kHeaderSize) {
    throw std::runtime_error("market data payload size mismatch");
  }

  Header header;
  header.type = static_cast<MarketDataMessageType>(type);
  header.symbol_id = symbol_id;
  header.sequence = sequence;
  header.payload = message.subspan(kHeaderSize);
  return header;
}

void write_header(Writer& writer, MarketDataMessageType type,
                  std::uint32_t symbol_id, std::uint64_t sequence,
                  std::size_t payload_size) {
  writer.u32(kMagic);
  writer.u16(kVersion);
  writer.u8(static_cast<std::uint8_t>(type));
  writer.u8(0);
  writer.u32(symbol_id);
  writer.u64(sequence);
  writer.u32(static_cast<std::uint32_t>(payload_size));
}

void write_order(Writer& writer, const RestingOrderView& order) {
  writer.u64(order.id);
  writer.u32(order.account);
  writer.u8(order.side == Side::Buy ? 0 : 1);
  writer.i64(order.price);
  writer.i64(order.quantity);
}

RestingOrderView read_order(Reader& reader) {
  RestingOrderView order;
  order.id = reader.u64();
  order.account = reader.u32();
  const std::uint8_t side = reader.u8();
  if (side > 1) {
    throw std::runtime_error("invalid order side in market data");
  }
  order.side = side == 0 ? Side::Buy : Side::Sell;
  order.price = reader.i64();
  order.quantity = reader.i64();
  if (order.quantity <= 0 || order.price <= 0) {
    throw std::runtime_error("invalid order in market data");
  }
  return order;
}

std::vector<std::byte> frame(MarketDataMessageType type,
                             std::uint32_t symbol_id, std::uint64_t sequence,
                             const std::vector<std::byte>& payload) {
  std::vector<std::byte> message;
  message.reserve(kHeaderSize + payload.size());
  Writer writer(message);
  write_header(writer, type, symbol_id, sequence, payload.size());
  message.insert(message.end(), payload.begin(), payload.end());
  return message;
}

} // namespace

const char* to_string(MarketDataMessageType type) noexcept {
  switch (type) {
  case MarketDataMessageType::Snapshot:
    return "snapshot";
  case MarketDataMessageType::AddOrder:
    return "add-order";
  case MarketDataMessageType::RemoveOrder:
    return "remove-order";
  case MarketDataMessageType::ReduceOrder:
    return "reduce-order";
  case MarketDataMessageType::ModifyOrder:
    return "modify-order";
  }
  return "unknown";
}

std::vector<std::byte> encode_snapshot(const MarketDataSnapshot& snapshot) {
  std::vector<std::byte> payload;
  payload.reserve(4 + snapshot.orders.size() * kOrderRecordSize);
  Writer writer(payload);
  writer.u32(static_cast<std::uint32_t>(snapshot.orders.size()));
  for (const auto& order : snapshot.orders) {
    write_order(writer, order);
  }
  return frame(MarketDataMessageType::Snapshot, snapshot.symbol_id,
               snapshot.sequence, payload);
}

MarketDataSnapshot decode_snapshot(std::span<const std::byte> message) {
  const Header header = read_header(message);
  if (header.type != MarketDataMessageType::Snapshot) {
    throw std::runtime_error("message is not a snapshot");
  }

  Reader reader(header.payload);
  MarketDataSnapshot snapshot;
  snapshot.symbol_id = header.symbol_id;
  snapshot.sequence = header.sequence;
  const std::uint32_t count = reader.u32();
  if (count > reader.remaining() / kOrderRecordSize) {
    throw std::runtime_error("snapshot order count exceeds payload");
  }
  snapshot.orders.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    snapshot.orders.push_back(read_order(reader));
  }
  if (reader.remaining() != 0) {
    throw std::runtime_error("trailing bytes in snapshot payload");
  }
  return snapshot;
}

std::vector<std::byte> encode_delta(const BookDelta& delta) {
  std::vector<std::byte> payload;
  Writer writer(payload);

  switch (delta.type) {
  case MarketDataMessageType::AddOrder:
    write_order(writer, delta.order);
    break;
  case MarketDataMessageType::RemoveOrder:
    writer.u64(delta.order_id);
    break;
  case MarketDataMessageType::ReduceOrder:
    writer.u64(delta.order_id);
    writer.i64(delta.new_quantity);
    break;
  case MarketDataMessageType::ModifyOrder:
    writer.u64(delta.order_id);
    writer.i64(delta.new_price);
    writer.i64(delta.new_quantity);
    break;
  case MarketDataMessageType::Snapshot:
    throw std::invalid_argument("snapshot is not a delta message");
  }

  return frame(delta.type, delta.symbol_id, delta.sequence, payload);
}

BookDelta decode_delta(std::span<const std::byte> message) {
  const Header header = read_header(message);
  if (header.type == MarketDataMessageType::Snapshot) {
    throw std::runtime_error("message is a snapshot, not a delta");
  }

  Reader reader(header.payload);
  BookDelta delta;
  delta.type = header.type;
  delta.symbol_id = header.symbol_id;
  delta.sequence = header.sequence;

  switch (delta.type) {
  case MarketDataMessageType::AddOrder:
    delta.order = read_order(reader);
    break;
  case MarketDataMessageType::RemoveOrder:
    delta.order_id = reader.u64();
    break;
  case MarketDataMessageType::ReduceOrder:
    delta.order_id = reader.u64();
    delta.new_quantity = reader.i64();
    if (delta.new_quantity <= 0) {
      throw std::runtime_error("reduce quantity must be positive");
    }
    break;
  case MarketDataMessageType::ModifyOrder:
    delta.order_id = reader.u64();
    delta.new_price = reader.i64();
    delta.new_quantity = reader.i64();
    if (delta.new_price <= 0 || delta.new_quantity <= 0) {
      throw std::runtime_error("modify price and quantity must be positive");
    }
    break;
  case MarketDataMessageType::Snapshot:
    throw std::runtime_error("snapshot is not a delta");
  }

  if (reader.remaining() != 0) {
    throw std::runtime_error("trailing bytes in delta payload");
  }
  return delta;
}

MarketDataSnapshot make_snapshot(const OrderBook& book, std::uint32_t symbol_id,
                                 std::uint64_t sequence) {
  MarketDataSnapshot snapshot;
  snapshot.symbol_id = symbol_id;
  snapshot.sequence = sequence;
  snapshot.orders = book.resting_orders();
  return snapshot;
}

void restore_snapshot(OrderBook& book, const MarketDataSnapshot& snapshot) {
  if (!book.empty()) {
    throw std::runtime_error("snapshot restore requires an empty book");
  }
  for (const auto& order : snapshot.orders) {
    const SubmitResult result = book.add_limit_order(
        order.id, order.side, order.price, order.quantity, order.account);
    if (!result.accepted || result.filled != 0 || !result.rested) {
      throw std::runtime_error(
          "snapshot order could not be restored without matching");
    }
  }
}

void apply_delta(OrderBook& book, const BookDelta& delta) {
  switch (delta.type) {
  case MarketDataMessageType::AddOrder: {
    const SubmitResult result = book.add_limit_order(
        delta.order.id, delta.order.side, delta.order.price,
        delta.order.quantity, delta.order.account);
    if (!result.accepted || result.filled != 0 || !result.rested) {
      throw std::runtime_error(
          "add delta could not be applied without matching");
    }
    break;
  }
  case MarketDataMessageType::RemoveOrder:
    if (!book.cancel(delta.order_id)) {
      throw std::runtime_error("remove delta references an unknown order");
    }
    break;
  case MarketDataMessageType::ReduceOrder:
    if (!book.reduce(delta.order_id, delta.new_quantity)) {
      throw std::runtime_error("reduce delta could not be applied");
    }
    break;
  case MarketDataMessageType::ModifyOrder:
    if (!book.modify(delta.order_id, delta.new_price, delta.new_quantity)) {
      throw std::runtime_error("modify delta could not be applied");
    }
    break;
  case MarketDataMessageType::Snapshot:
    throw std::runtime_error("snapshot is not a delta");
  }
}

MarketDataPublisher::MarketDataPublisher(std::uint32_t symbol_id,
                                         std::uint64_t first_sequence) noexcept
    : symbol_id_(symbol_id), sequence_(first_sequence) {}

std::vector<std::byte> MarketDataPublisher::snapshot(const OrderBook& book) {
  return encode_snapshot(make_snapshot(book, symbol_id_, take_sequence()));
}

std::vector<std::byte> MarketDataPublisher::add(const RestingOrderView& order) {
  BookDelta delta;
  delta.type = MarketDataMessageType::AddOrder;
  delta.symbol_id = symbol_id_;
  delta.sequence = take_sequence();
  delta.order = order;
  return encode_delta(delta);
}

std::vector<std::byte> MarketDataPublisher::remove(OrderId id) {
  BookDelta delta;
  delta.type = MarketDataMessageType::RemoveOrder;
  delta.symbol_id = symbol_id_;
  delta.sequence = take_sequence();
  delta.order_id = id;
  return encode_delta(delta);
}

std::vector<std::byte> MarketDataPublisher::reduce(OrderId id,
                                                   Quantity new_quantity) {
  BookDelta delta;
  delta.type = MarketDataMessageType::ReduceOrder;
  delta.symbol_id = symbol_id_;
  delta.sequence = take_sequence();
  delta.order_id = id;
  delta.new_quantity = new_quantity;
  return encode_delta(delta);
}

std::vector<std::byte> MarketDataPublisher::modify(OrderId id, Price new_price,
                                                   Quantity new_quantity) {
  BookDelta delta;
  delta.type = MarketDataMessageType::ModifyOrder;
  delta.symbol_id = symbol_id_;
  delta.sequence = take_sequence();
  delta.order_id = id;
  delta.new_price = new_price;
  delta.new_quantity = new_quantity;
  return encode_delta(delta);
}

} // namespace cppquant
