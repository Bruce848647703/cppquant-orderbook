#include "cppquant/replay.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cppquant {
namespace {

std::string trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return "";
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(begin, end - begin + 1));
}

std::vector<std::string> split(std::string_view line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t comma = line.find(',', start);
    if (comma == std::string_view::npos) {
      fields.push_back(trim(line.substr(start)));
      break;
    }
    fields.push_back(trim(line.substr(start, comma - start)));
    start = comma + 1;
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

ReplayCommand parse_submit(const std::vector<std::string>& fields) {
  ReplayCommand command;
  command.type = ReplayCommandType::Submit;
  const auto id = parse_number(fields, 2, "id");
  const auto type = parse_type(fields[1]);
  const auto side = parse_side(fields.size() > 3 ? fields[3] : "");
  const auto price = parse_number(fields, 4, "price");
  const auto quantity = parse_number(fields, 5, "quantity");
  const auto account =
      fields.size() > 6 && !fields[6].empty() ? std::stoul(fields[6]) : 0U;
  command.order = Order{static_cast<OrderId>(id),
                        static_cast<AccountId>(account),
                        side,
                        type,
                        price,
                        quantity};
  return command;
}

} // namespace

std::optional<ReplayCommand> parse_replay_line(std::string_view line) {
  const std::string cleaned = trim(line);
  if (cleaned.empty() || cleaned[0] == '#') {
    return std::nullopt;
  }

  try {
    const auto fields = split(cleaned);
    if (fields.size() < 3) {
      throw std::runtime_error("expected at least event and id");
    }

    const std::string& event = fields[1];
    if (event == "cancel") {
      ReplayCommand command;
      command.type = ReplayCommandType::Cancel;
      command.order_id = static_cast<OrderId>(parse_number(fields, 2, "id"));
      return command;
    }

    if (event == "modify") {
      ReplayCommand command;
      command.type = ReplayCommandType::Modify;
      command.order_id = static_cast<OrderId>(parse_number(fields, 2, "id"));
      command.new_price = parse_number(fields, 3, "price");
      command.new_quantity = parse_number(fields, 4, "quantity");
      return command;
    }

    return parse_submit(fields);
  } catch (const std::exception& error) {
    throw std::runtime_error("invalid replay line: " +
                             std::string(error.what()));
  }
}

} // namespace cppquant