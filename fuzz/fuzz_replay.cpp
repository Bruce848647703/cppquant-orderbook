#include "cppquant/orderbook.hpp"
#include "cppquant/replay.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

using namespace cppquant;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  const std::string_view text(reinterpret_cast<const char*>(data), size);
  OrderBook book(SelfTradePrevention::Allow, 10'000);

  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line = end == std::string_view::npos
                                      ? text.substr(start)
                                      : text.substr(start, end - start);

    try {
      const auto command = parse_replay_line(line);
      if (command) {
        switch (command->type) {
        case ReplayCommandType::Submit:
          (void)book.submit(command->order);
          break;
        case ReplayCommandType::Cancel:
          (void)book.cancel(command->order_id);
          break;
        case ReplayCommandType::Modify:
          (void)book.modify(command->order_id, command->new_price,
                            command->new_quantity);
          break;
        }
      }
    } catch (...) {
    }

    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  if (!book.validate()) {
    __builtin_trap();
  }
  return 0;
}