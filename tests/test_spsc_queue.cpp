#include "cppquant/spsc_queue.hpp"
#include "test_framework.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

using namespace cppquant;

namespace {

struct Counted {
  static inline int constructed = 0;
  static inline int destroyed = 0;

  int value{};

  explicit Counted(int initial = 0) : value(initial) {
    ++constructed;
  }
  Counted(const Counted& other) : value(other.value) {
    ++constructed;
  }
  Counted(Counted&& other) noexcept : value(other.value) {
    ++constructed;
  }
  Counted& operator=(const Counted&) = default;
  Counted& operator=(Counted&&) noexcept = default;
  ~Counted() {
    ++destroyed;
  }
};

} // namespace

TEST(spsc_queue_is_fifo) {
  SpscQueue<int, 4> queue;
  CHECK(queue.empty());
  CHECK(!queue.full());

  CHECK(queue.try_push(1));
  CHECK(queue.try_push(2));
  CHECK(queue.try_push(3));
  CHECK_EQ(queue.size_approx(), 3u);

  int value = 0;
  CHECK(queue.try_pop(value));
  CHECK_EQ(value, 1);
  CHECK(queue.try_pop(value));
  CHECK_EQ(value, 2);
  CHECK(queue.try_pop(value));
  CHECK_EQ(value, 3);
  CHECK(queue.empty());
  CHECK(!queue.try_pop(value));
}

TEST(spsc_queue_wraps_around) {
  SpscQueue<int, 4> queue;
  for (int value = 0; value < 20; ++value) {
    CHECK(queue.try_push(value));
    int popped = -1;
    CHECK(queue.try_pop(popped));
    CHECK_EQ(popped, value);
  }
  CHECK(queue.empty());
}

TEST(spsc_queue_reports_full_and_recovers) {
  SpscQueue<int, 4> queue;
  for (int value = 0; value < 4; ++value) {
    CHECK(queue.try_push(value));
  }
  CHECK(queue.full());
  CHECK(!queue.try_push(4));

  int value = 0;
  CHECK(queue.try_pop(value));
  CHECK_EQ(value, 0);
  CHECK(queue.try_push(4));
  CHECK(queue.full());
}

TEST(spsc_queue_supports_move_only_types) {
  SpscQueue<std::unique_ptr<int>, 4> queue;
  CHECK(queue.try_push(std::make_unique<int>(7)));

  std::unique_ptr<int> value;
  CHECK(queue.try_pop(value));
  CHECK(value != nullptr);
  CHECK_EQ(*value, 7);
}

TEST(spsc_queue_destroys_elements) {
  Counted::constructed = 0;
  Counted::destroyed = 0;
  {
    SpscQueue<Counted, 4> queue;
    for (int value = 0; value < 3; ++value) {
      CHECK(queue.try_push(Counted(value)));
    }
    Counted output;
    while (queue.try_pop(output)) {
    }
  }
  CHECK_EQ(Counted::destroyed, Counted::constructed);
}

TEST(spsc_queue_handles_a_concurrent_producer_and_consumer) {
  constexpr std::uint64_t kMessages = 1'000'000;
  SpscQueue<std::uint64_t, 1024> queue;
  std::atomic<bool> start{false};

  std::uint64_t sum = 0;
  std::uint64_t count = 0;

  std::thread producer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (std::uint64_t value = 1; value <= kMessages; ++value) {
      while (!queue.try_push(value)) {
      }
    }
  });

  std::thread consumer([&]() {
    while (!start.load(std::memory_order_acquire)) {
    }
    std::uint64_t value = 0;
    while (count < kMessages) {
      if (queue.try_pop(value)) {
        sum += value;
        ++count;
      }
    }
  });

  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  CHECK_EQ(count, kMessages);
  CHECK_EQ(sum, kMessages * (kMessages + 1) / 2);
  CHECK(queue.empty());
}
