#include "cppquant/replay.hpp"
#include "test_framework.hpp"

#include <stdexcept>

using namespace cppquant;

TEST(replay_parses_submit_commands) {
  const auto limit = parse_replay_line("1, add, 42, B, 100, 7, 9");
  CHECK(limit.has_value());
  CHECK(limit->type == ReplayCommandType::Submit);
  CHECK_EQ(limit->order.id, 42u);
  CHECK_EQ(limit->order.account, 9u);
  CHECK(limit->order.side == Side::Buy);
  CHECK(limit->order.type == OrderType::Limit);
  CHECK_EQ(limit->order.price, 100);
  CHECK_EQ(limit->order.quantity, 7);

  const auto market = parse_replay_line("2,market,43,S,0,5");
  CHECK(market.has_value());
  CHECK(market->order.side == Side::Sell);
  CHECK(market->order.type == OrderType::Market);
  CHECK_EQ(market->order.account, 0u);

  for (const char* text : {"3,ioc,44,B,99,1", "4,fok,45,S,101,2"}) {
    CHECK(parse_replay_line(text).has_value());
  }
}

TEST(replay_parses_cancel_and_modify_commands) {
  const auto cancel = parse_replay_line("10,cancel,7");
  CHECK(cancel.has_value());
  CHECK(cancel->type == ReplayCommandType::Cancel);
  CHECK_EQ(cancel->order_id, 7u);

  const auto modify = parse_replay_line("11,modify,7,98,3");
  CHECK(modify.has_value());
  CHECK(modify->type == ReplayCommandType::Modify);
  CHECK_EQ(modify->order_id, 7u);
  CHECK_EQ(modify->new_price, 98);
  CHECK_EQ(modify->new_quantity, 3);
}

TEST(replay_skips_blank_and_comment_lines) {
  CHECK(!parse_replay_line("").has_value());
  CHECK(!parse_replay_line("   \t ").has_value());
  CHECK(!parse_replay_line("# a comment").has_value());
  CHECK(!parse_replay_line("  # indented comment").has_value());
}

TEST(replay_rejects_malformed_lines) {
  CHECK_THROW(parse_replay_line("1,cancel"), std::runtime_error);
  CHECK_THROW(parse_replay_line("1,unknown,2"), std::runtime_error);
  CHECK_THROW(parse_replay_line("1,add,2,X,100,5"), std::runtime_error);
  CHECK_THROW(parse_replay_line("1,add,2,B,100"), std::runtime_error);
  CHECK_THROW(parse_replay_line("1,add,not-a-number,B,100,5"),
              std::runtime_error);
  CHECK_THROW(parse_replay_line("1,modify,2,100"), std::runtime_error);
  CHECK_THROW(parse_replay_line("1,add,999999999999999999999,B,100,5"),
              std::runtime_error);
}

TEST(replay_parsed_commands_apply_to_an_order_book) {
  OrderBook book(SelfTradePrevention::Allow, 32);
  const auto submit = parse_replay_line("1,add,1,B,100,5");
  CHECK(submit.has_value());
  CHECK(book.submit(submit->order).accepted);

  const auto modify = parse_replay_line("2,modify,1,99,3");
  CHECK(modify.has_value());
  CHECK(book.modify(modify->order_id, modify->new_price, modify->new_quantity));
  CHECK_EQ(book.quantity_of(1), std::optional<Quantity>{3});

  const auto cancel = parse_replay_line("3,cancel,1");
  CHECK(cancel.has_value());
  CHECK(book.cancel(cancel->order_id));
  CHECK(book.empty());
}